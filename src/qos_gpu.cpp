/*
  ISC License

  Copyright (c) 2026, Antonio SJ Musumeci <trapexit@spawn.link>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "qos_gpu.hpp"

#if USE_QOS && USE_QOS_GPU && defined(__linux__)

#include "fmt/core.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>


namespace
{
  constexpr u64 NS_PER_SEC = 1000000000ULL;

  // A hardware transcode does not change state meaningfully faster
  // than this, and the throttle path reads the cached value on every
  // request. Four reads a second is plenty and costs nothing.
  constexpr u64 REFRESH_NS = (NS_PER_SEC / 4);

  std::mutex               g_mutex;
  std::vector<std::string> g_paths;
  bool                     g_discovered = false;

  std::atomic<int> g_busy{-1};
  std::atomic<u64> g_read_at{0};

  // Guards the sysfs read itself so that when many threads find the
  // cache stale at once, only one of them goes to the kernel and the
  // rest carry on with the slightly stale value.
  std::atomic<bool> g_refreshing{false};

  std::atomic<u32>    g_threshold{0};
  std::atomic<double> g_floor{0.0};

  u64
  _now_ns()
  {
    struct timespec ts;

    ::clock_gettime(CLOCK_MONOTONIC,&ts);

    return ((static_cast<u64>(ts.tv_sec) * NS_PER_SEC) +
            static_cast<u64>(ts.tv_nsec));
  }

  bool
  _is_card(const char *name_)
  {
    // "card0" yes, "card0-DP-1" no: the connector nodes have no
    // device/ counters and would just be dead opens.
    if(::strncmp(name_,"card",4) != 0)
      return false;

    const char *p = (name_ + 4);

    if(*p == '\0')
      return false;

    for(; *p != '\0'; p++)
      {
        if((*p < '0') || (*p > '9'))
          return false;
      }

    return true;
  }

  // Done once. A GPU does not appear or vanish while the mount is up,
  // and a host with none should not pay a directory walk for it.
  void
  _discover()
  {
    if(g_discovered)
      return;

    g_discovered = true;

    DIR *d = ::opendir("/sys/class/drm");
    if(d == nullptr)
      return;

    struct dirent *de;
    while((de = ::readdir(d)) != nullptr)
      {
        if(!::_is_card(de->d_name))
          continue;

        std::string path = fmt::format("/sys/class/drm/{}/device/gpu_busy_percent",
                                       de->d_name);

        if(::access(path.c_str(),R_OK) != 0)
          continue;

        g_paths.push_back(std::move(path));
      }

    ::closedir(d);
  }

  int
  _read_one(const std::string &path_)
  {
    const int fd = ::open(path_.c_str(),O_RDONLY);
    if(fd < 0)
      return -1;

    char          buf[32];
    const ssize_t n = ::read(fd,buf,sizeof(buf) - 1);

    ::close(fd);

    if(n <= 0)
      return -1;

    buf[n] = '\0';

    char      *end = nullptr;
    const long v   = ::strtol(buf,&end,10);

    if((end == buf) || (v < 0) || (v > 100))
      return -1;

    return static_cast<int>(v);
  }

  void
  _refresh()
  {
    std::lock_guard<std::mutex> lk(g_mutex);

    ::_discover();

    if(g_paths.empty())
      {
        g_busy.store(-1,std::memory_order_relaxed);
        return;
      }

    // The busiest engine is what matters. A second idle card does not
    // make the one doing the transcoding any less saturated.
    int busiest = -1;

    for(const auto &path : g_paths)
      {
        const int v = ::_read_one(path);

        if(v > busiest)
          busiest = v;
      }

    g_busy.store(busiest,std::memory_order_relaxed);
  }
}

bool
qos::gpu::supported()
{
  std::lock_guard<std::mutex> lk(g_mutex);

  ::_discover();

  return !g_paths.empty();
}

int
qos::gpu::busy()
{
  // Disabled means nobody is asking, so do not even keep the cache
  // warm -- a mount with no GPU policy pays nothing for this file
  // existing.
  if(g_threshold.load(std::memory_order_relaxed) == 0)
    return -1;

  const u64 now  = ::_now_ns();
  const u64 last = g_read_at.load(std::memory_order_relaxed);

  if((last == 0) || ((now - last) >= REFRESH_NS))
    {
      bool expected = false;

      if(g_refreshing.compare_exchange_strong(expected,true,
                                              std::memory_order_acq_rel))
        {
          ::_refresh();

          g_read_at.store(now,std::memory_order_relaxed);
          g_refreshing.store(false,std::memory_order_release);
        }
    }

  return g_busy.load(std::memory_order_relaxed);
}

void
qos::gpu::set_threshold(const u32    percent_,
                        const double floor_)
{
  g_threshold.store(percent_,std::memory_order_relaxed);
  g_floor.store(floor_,std::memory_order_relaxed);
}

u32
qos::gpu::threshold()
{
  return g_threshold.load(std::memory_order_relaxed);
}

double
qos::gpu::floor()
{
  return g_floor.load(std::memory_order_relaxed);
}

double
qos::gpu::pressure_floor()
{
  const u32 threshold = g_threshold.load(std::memory_order_relaxed);

  if(threshold == 0)
    return 0.0;

  const int busy = qos::gpu::busy();

  if(busy < 0)
    return 0.0;

  if(static_cast<u32>(busy) < threshold)
    return 0.0;

  return g_floor.load(std::memory_order_relaxed);
}

std::string
qos::gpu::status()
{
  const u32 threshold = g_threshold.load(std::memory_order_relaxed);

  std::size_t ncards;
  {
    std::lock_guard<std::mutex> lk(g_mutex);

    ::_discover();

    ncards = g_paths.size();
  }

  if(ncards == 0)
    return "counters=0 (no amdgpu gpu_busy_percent found)\n";

  if(threshold == 0)
    return fmt::format("counters={} threshold=0 (disabled)\n",ncards);

  return fmt::format("counters={} busy={} threshold={} floor={:.2f}\n",
                     ncards,
                     qos::gpu::busy(),
                     threshold,
                     g_floor.load(std::memory_order_relaxed));
}

#else

/*
  Built without the GPU signal, or built for a platform with no sysfs
  to read it from.

  Every entry point stays present and inert so that nothing else in the
  daemon is conditionally compiled: qos::pressure() calls
  pressure_floor() unconditionally and gets a flat 0.0, which is
  exactly the behaviour of the signal being switched off at runtime.
 */

bool
qos::gpu::supported()
{
  return false;
}

int
qos::gpu::busy()
{
  return -1;
}

void
qos::gpu::set_threshold(const u32,
                        const double)
{
}

u32
qos::gpu::threshold()
{
  return 0;
}

double
qos::gpu::floor()
{
  return 0.0;
}

double
qos::gpu::pressure_floor()
{
  return 0.0;
}

std::string
qos::gpu::status()
{
  return "unsupported (built without USE_QOS_GPU)\n";
}

#endif
