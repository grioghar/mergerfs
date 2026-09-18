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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "qos_capacity.hpp"

#include <errno.h>

#if USE_QOS && USE_QOS_CALIBRATE

#include "ioprio.hpp"
#include "qos.hpp"
#include "qos_class.hpp"

#include "fmt/core.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <random>
#include <thread>


namespace
{
  constexpr u64 NS_PER_SEC = 1000000000ULL;

  // O_DIRECT reads must be sector aligned, and a block this size is
  // large enough that per-request overhead does not show up in the
  // result on a spinning disk.
  constexpr u64 BLOCK = (4 * 1024 * 1024);

  // Below this a file is too small to read from for a meaningful
  // length of time without simply re-reading the same tracks.
  constexpr u64 MIN_PROBE_SIZE = (256ULL * 1024 * 1024);

  // A hard cap on the search for something to read. On a disk full of
  // media the first large file turns up almost immediately; the cap is
  // what stops a pathological layout turning this into a full tree
  // walk.
  constexpr u64 MAX_SCAN_ENTRIES = 20000;

  constexpr u64 MAX_SECONDS = 60;

  struct Probe
  {
    std::string branch;
    u64         read_bps  = 0;
    u64         write_bps = 0;
    std::string error;
  };

  std::mutex         g_mutex;
  std::atomic<bool>  g_running{false};
  std::vector<Probe> g_results;
  std::string        g_started;
  u64                g_total = 0;

  u64
  _now_ns()
  {
    struct timespec ts;

    ::clock_gettime(CLOCK_MONOTONIC,&ts);

    return ((static_cast<u64>(ts.tv_sec) * NS_PER_SEC) +
            static_cast<u64>(ts.tv_nsec));
  }

  // Drop the probing thread to the bottom of both schedulers. A
  // calibration that collides with playback must yield rather than
  // compete -- the number it produces matters much less than the
  // stream it could otherwise interrupt.
  void
  _be_polite()
  {
    ::setpriority(PRIO_PROCESS,0,19);
    ::ioprio::set(0,qos::ioprio::value(qos::ioprio::CLASS_IDLE,0));
  }

  // Breadth first so a branch whose root holds large files is answered
  // without descending into a deep tree first.
  std::string
  _find_probe_file(const std::string &branch_)
  {
    u64                     seen = 0;
    std::deque<std::string> queue{branch_};

    while(!queue.empty())
      {
        const std::string dir = queue.front();
        queue.pop_front();

        DIR *d = ::opendir(dir.c_str());
        if(d == nullptr)
          continue;

        struct dirent *de;
        while((de = ::readdir(d)) != nullptr)
          {
            if((::strcmp(de->d_name,".") == 0) ||
               (::strcmp(de->d_name,"..") == 0))
              continue;

            if(++seen > MAX_SCAN_ENTRIES)
              {
                ::closedir(d);
                return {};
              }

            const std::string path = (dir + "/" + de->d_name);

            struct stat st;
            // Deliberately lstat: a symlink out of the branch would
            // measure a different device entirely.
            if(::lstat(path.c_str(),&st) != 0)
              continue;

            if(S_ISDIR(st.st_mode))
              queue.push_back(path);
            else if(S_ISREG(st.st_mode) &&
                    (static_cast<u64>(st.st_size) >= MIN_PROBE_SIZE))
              {
                ::closedir(d);
                return path;
              }
          }

        ::closedir(d);
      }

    return {};
  }

  // Sequential O_DIRECT read from a random aligned offset. Returns
  // bytes/sec, or 0 with `err_` set.
  u64
  _measure_read(const std::string &path_,
                const u64          seconds_,
                std::string       *err_)
  {
    const int fd = ::open(path_.c_str(),O_RDONLY | O_DIRECT);
    if(fd < 0)
      {
        *err_ = fmt::format("open O_DIRECT: {}",::strerror(errno));
        return 0;
      }

    struct stat st;
    if((::fstat(fd,&st) != 0) || (static_cast<u64>(st.st_size) < (BLOCK * 2)))
      {
        ::close(fd);
        *err_ = "probe file too small";
        return 0;
      }

    void *buf = nullptr;
    if(::posix_memalign(&buf,4096,BLOCK) != 0)
      {
        ::close(fd);
        *err_ = "cannot allocate aligned buffer";
        return 0;
      }

    const u64 size = static_cast<u64>(st.st_size);

    // Start somewhere other than the front of the file so repeated
    // runs do not all measure the same tracks.
    std::mt19937_64 rng(::_now_ns());
    const u64 blocks = ((size - BLOCK) / BLOCK);
    u64 offset = ((blocks > 0) ? ((rng() % blocks) * BLOCK) : 0);

    const u64 t0       = ::_now_ns();
    const u64 deadline = (t0 + (seconds_ * NS_PER_SEC));

    u64 total = 0;
    while(::_now_ns() < deadline)
      {
        if((offset + BLOCK) > size)
          offset = 0;

        const ssize_t n = ::pread(fd,buf,BLOCK,static_cast<off_t>(offset));
        if(n <= 0)
          break;

        total  += static_cast<u64>(n);
        offset += static_cast<u64>(n);
      }

    const u64 elapsed = (::_now_ns() - t0);

    ::free(buf);
    ::close(fd);

    if((elapsed == 0) || (total == 0))
      {
        *err_ = "read produced no data";
        return 0;
      }

    return static_cast<u64>((static_cast<unsigned __int128>(total) *
                             NS_PER_SEC) / elapsed);
  }

  // Sequential O_DIRECT write to a temporary file on the branch, which
  // is removed whatever happens. Only ever reached when the caller
  // explicitly asked for writes.
  u64
  _measure_write(const std::string &branch_,
                 const u64          seconds_,
                 std::string       *err_)
  {
    const std::string path = fmt::format("{}/.mergerfs-qos-calibrate.{}",
                                         branch_,
                                         static_cast<int>(::getpid()));

    const int fd = ::open(path.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_DIRECT,
                          0600);
    if(fd < 0)
      {
        *err_ = fmt::format("create probe file: {}",::strerror(errno));
        return 0;
      }

    void *buf = nullptr;
    if(::posix_memalign(&buf,4096,BLOCK) != 0)
      {
        ::close(fd);
        ::unlink(path.c_str());
        *err_ = "cannot allocate aligned buffer";
        return 0;
      }

    ::memset(buf,0xA5,BLOCK);

    const u64 t0       = ::_now_ns();
    const u64 deadline = (t0 + (seconds_ * NS_PER_SEC));

    u64 total = 0;
    while(::_now_ns() < deadline)
      {
        const ssize_t n = ::pwrite(fd,buf,BLOCK,static_cast<off_t>(total));
        if(n <= 0)
          break;

        total += static_cast<u64>(n);
      }

    const u64 elapsed = (::_now_ns() - t0);

    ::free(buf);
    ::close(fd);
    ::unlink(path.c_str());

    if((elapsed == 0) || (total == 0))
      {
        *err_ = "write produced no data";
        return 0;
      }

    return static_cast<u64>((static_cast<unsigned __int128>(total) *
                             NS_PER_SEC) / elapsed);
  }

  void
  _run(std::vector<std::string> branches_,
       const u64                seconds_,
       const bool               allow_write_)
  {
    ::_be_polite();

    for(const auto &branch : branches_)
      {
        Probe p;

        p.branch = branch;

        const std::string file = ::_find_probe_file(branch);

        if(file.empty())
          {
            p.error = "no file of at least 256MiB found to read";
          }
        else
          {
            std::string err;

            p.read_bps = ::_measure_read(file,seconds_,&err);
            if(p.read_bps == 0)
              p.error = err;
          }

        if(allow_write_)
          {
            std::string err;

            p.write_bps = ::_measure_write(branch,seconds_,&err);
            if((p.write_bps == 0) && p.error.empty())
              p.error = err;
          }

        // The read figure is what a percentage rate is resolved
        // against: the pool's problem is playback contending with
        // bulk traffic, and that contention is on the read path.
        // Installed per branch as it is measured rather than in one
        // batch at the end, so a long run helps immediately.
        if(p.read_bps != 0)
          qos::set_probed_capacity(branch,p.read_bps);

        {
          std::lock_guard<std::mutex> lk(g_mutex);
          g_results.push_back(std::move(p));
        }
      }

    g_running.store(false,std::memory_order_release);
  }
}

bool
qos::capacity::running()
{
  return g_running.load(std::memory_order_acquire);
}

int
qos::capacity::start(const std::vector<fs::path> &branches_,
                     const u64                    seconds_,
                     const bool                   allow_write_)
{
  if(branches_.empty())
    return -EINVAL;

  const u64 seconds = std::min<u64>(std::max<u64>(seconds_,1),MAX_SECONDS);

  bool expected = false;
  if(!g_running.compare_exchange_strong(expected,true,
                                        std::memory_order_acq_rel))
    return -EBUSY;

  std::vector<std::string> paths;
  paths.reserve(branches_.size());
  for(const auto &b : branches_)
    paths.push_back(b.native());

  {
    std::lock_guard<std::mutex> lk(g_mutex);

    g_results.clear();
    g_total = paths.size();

    char      buf[64];
    struct tm tm;
    const time_t now = ::time(nullptr);

    ::localtime_r(&now,&tm);
    ::strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",&tm);

    g_started = buf;
  }

  // Detached: a calibration outlives the setxattr that asked for it,
  // and its results are collected by reading the key back rather than
  // by joining.
  std::thread(::_run,std::move(paths),seconds,allow_write_).detach();

  return 0;
}

std::string
qos::capacity::status()
{
  std::lock_guard<std::mutex> lk(g_mutex);

  const bool running = g_running.load(std::memory_order_acquire);

  if(g_started.empty())
    return "never run\n";

  std::string out = fmt::format("started={} state={} done={}/{}\n",
                                g_started,
                                (running ? "running" : "complete"),
                                g_results.size(),
                                g_total);

  for(const auto &p : g_results)
    {
      out += fmt::format("{}: read-bps={} write-bps={}{}\n",
                         p.branch,
                         p.read_bps,
                         p.write_bps,
                         (p.error.empty()
                          ? std::string{}
                          : fmt::format(" error='{}'",p.error)));
    }

  return out;
}

#else

/*
  Built without the active capacity probe.

  Passive measurement is part of the QoS core and is unaffected: the
  daemon still learns what each branch delivers by watching real
  traffic. What is missing here is only the ability to go and find out
  on demand.
 */

bool
qos::capacity::running()
{
  return false;
}

int
qos::capacity::start(const std::vector<fs::path> &,
                     const u64,
                     const bool)
{
  return -EOPNOTSUPP;
}

std::string
qos::capacity::status()
{
  return "unsupported (built without USE_QOS_CALIBRATE)\n";
}

#endif
