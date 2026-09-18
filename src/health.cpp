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

#include "health.hpp"

#include "config.hpp"
#include "syslog.hpp"

#include "fmt/core.h"
#include "subprocess/subprocess.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <string>


namespace health
{
  std::atomic<u64> interval{600};
  std::atomic<u64> max_temp{0};
  std::atomic<u64> max_pending{1};
  std::atomic<u64> max_realloc{0};
}

namespace
{
  struct Report
  {
    std::string device;
    u64  temp     = 0;
    u64  pending  = 0;
    u64  realloc  = 0;
    bool read_ok  = false;
    bool failing  = false;
    std::string reason;
    u64  checked_at = 0;
  };

  std::atomic<int>            g_mode{(int)health::Mode::OFF};
  std::mutex                  g_mutex;
  std::map<std::string,Report> g_reports;   // branch path -> report
  u64                         g_last_poll = 0;

  // Walk /sys from the filesystem's device number up to the physical disk.
  //
  // A branch is usually a partition, and may be a dm/LVM target stacked on
  // one. SMART lives on the disk, so follow `..` for partitions and `slaves/`
  // for device mapper until something with a `device` directory appears.
  std::string
  _resolve_disk(const std::string &mountpoint_)
  {
    struct stat st;

    if(::stat(mountpoint_.c_str(),&st) != 0)
      return {};

    std::string cur = fmt::format("/sys/dev/block/{}:{}",
                                  major(st.st_dev),minor(st.st_dev));

    for(int depth = 0; depth < 8; depth++)
      {
        char buf[PATH_MAX];
        const ssize_t n = ::readlink(cur.c_str(),buf,sizeof(buf)-1);
        std::string real = cur;
        if(n > 0)
          {
            buf[n] = '\0';
            std::string link(buf);
            const auto pos = link.rfind('/');
            real = fmt::format("/sys/class/block/{}",
                               (pos == std::string::npos) ? link : link.substr(pos+1));
          }

        // A whole disk has a `device` link; a partition does not.
        struct stat ds;
        if(::stat((real + "/device").c_str(),&ds) == 0)
          {
            const auto pos = real.rfind('/');
            return real.substr(pos + 1);
          }

        // device-mapper: descend into the first slave. Read directly;
        // no reason to start a shell to list one directory, and every
        // reason not to from a root daemon.
        std::string slaves = real + "/slaves";
        if(DIR *sd = ::opendir(slaves.c_str()))
          {
            std::string first;
            while(struct dirent *de = ::readdir(sd))
              {
                if((de->d_name[0] == '.'))
                  continue;
                first = de->d_name;
                break;
              }
            ::closedir(sd);
            if(!first.empty()) { cur = "/sys/class/block/" + first; continue; }
          }

        // partition: parent directory is the disk
        const auto pos = real.rfind('/');
        if(pos == std::string::npos)
          break;
        std::string parent = real.substr(0,pos);
        if(parent == "/sys/class/block" || parent.empty())
          break;
        cur = parent;
      }

    return {};
  }

  // Ask smartctl for the attributes we care about.
  //
  // Shelling out is not elegant, but re-implementing ATA passthrough for
  // SATA, USB bridges and NVMe inside a filesystem daemon would be far worse,
  // and this runs on the maintenance thread every `interval` seconds -- not
  // on any I/O path.
  bool
  _read_smart(const std::string &disk_,
              Report            &r_)
  {
    // argv, not a shell: the device name comes from sysfs and cannot
    // carry metacharacters, but a root daemon has no business handing
    // anything to /bin/sh -c, and an absolute path removes the PATH
    // lookup from the question as well.
    std::string out;
    try
      {
        auto buf = subprocess::check_output({"/usr/sbin/smartctl","-A",
                                             fmt::format("/dev/{}",disk_)});
        out.assign(buf.buf.data(),buf.length);
      }
    catch(const std::exception &)
      {
        // smartctl exits non-zero for a great many benign reasons
        // (bits set in its status mask for any attribute past
        // threshold). Its output is still worth parsing; only a
        // failure to run at all leaves nothing to read.
        return false;
      }

    bool any = false;
    std::size_t pos = 0;
    while(pos < out.size())
      {
        const std::size_t nl = out.find('\n',pos);
        std::string s = out.substr(pos,((nl == std::string::npos) ? std::string::npos : (nl - pos)));
        pos = ((nl == std::string::npos) ? out.size() : (nl + 1));
        u64 *dst = nullptr;

        if(s.find("Current_Pending_Sector") != std::string::npos)
          dst = &r_.pending;
        else if(s.find("Reallocated_Sector_Ct") != std::string::npos)
          dst = &r_.realloc;
        else if(s.find("Temperature_Celsius") != std::string::npos ||
                s.find("Airflow_Temperature") != std::string::npos)
          dst = &r_.temp;
        else
          continue;

        // The raw value is the last whitespace-separated field that parses
        // as a number; Seagate appends "(0 19 0 0 0)" style noise after it.
        std::string tok, val;
        for(std::size_t i = 0, n = 0; i <= s.size(); i++)
          {
            if(i == s.size() || std::isspace((unsigned char)s[i]))
              {
                if(!tok.empty()) { n++; if(n == 10) { val = tok; break; } tok.clear(); }
              }
            else tok += s[i];
          }
        if(val.empty())
          continue;

        try { *dst = std::stoull(val); any = true; }
        catch(...) { }
      }

    return any;
  }
}

void
health::set_mode(const health::Mode m_)
{
  g_mode.store((int)m_,std::memory_order_relaxed);
}

health::Mode
health::mode()
{
  return (health::Mode)g_mode.load(std::memory_order_relaxed);
}

void
health::poll_now()
{
  if(health::mode() == health::Mode::OFF)
    return;

  const bool quarantine = (health::mode() == health::Mode::QUARANTINE);
  const u64  mt = health::max_temp.load(std::memory_order_relaxed);
  const u64  mp = health::max_pending.load(std::memory_order_relaxed);
  const u64  mr = health::max_realloc.load(std::memory_order_relaxed);

  Branches::Ptr branches = cfg.branches;

  for(auto &branch : *branches)
    {
      Report r;
      r.checked_at = ::time(nullptr);
      r.device = ::_resolve_disk(branch.path.string());
      if(r.device.empty())
        {
          std::lock_guard<std::mutex> lk(g_mutex);
          g_reports[branch.path.string()] = r;
          continue;
        }

      r.read_ok = ::_read_smart(r.device,r);

      if(r.read_ok)
        {
          if(mp && (r.pending >= mp))
            { r.failing = true; r.reason = fmt::format("{} pending sectors",r.pending); }
          else if(mr && (r.realloc >= mr))
            { r.failing = true; r.reason = fmt::format("{} reallocated sectors",r.realloc); }
          else if(mt && r.temp && (r.temp >= mt))
            { r.failing = true; r.reason = fmt::format("{}C",r.temp); }
        }

      const bool was_ro = branch.ro();

      if(r.failing && quarantine && !was_ro)
        {
          branch.mode = Branch::Mode::RO;
          SysLog::error("health: quarantined {} ({}): {} -- set read-only, "
                        "reads unaffected. Run mergerfs.diskrepair to act on it.",
                        branch.path.string(),r.device,r.reason);
        }
      else if(r.failing && !quarantine)
        {
          SysLog::warning("health: {} ({}) looks unhealthy: {} (monitor only)",
                          branch.path.string(),r.device,r.reason);
        }

      std::lock_guard<std::mutex> lk(g_mutex);
      g_reports[branch.path.string()] = r;
    }
}

void
health::tick(const u64)
{
  if(health::mode() == health::Mode::OFF)
    return;

  const u64 now = ::time(nullptr);
  const u64 iv  = health::interval.load(std::memory_order_relaxed);

  if(g_last_poll && ((now - g_last_poll) < iv))
    return;

  g_last_poll = now;

  health::poll_now();
}

std::string
health::status()
{
  std::string s;
  const char *m = "off";
  switch(health::mode())
    {
    case health::Mode::MONITOR:    m = "monitor";    break;
    case health::Mode::QUARANTINE: m = "quarantine"; break;
    default: break;
    }

  s += fmt::format("mode={} interval={}s max-temp={} max-pending={} max-realloc={}\n",
                   m,
                   health::interval.load(std::memory_order_relaxed),
                   health::max_temp.load(std::memory_order_relaxed),
                   health::max_pending.load(std::memory_order_relaxed),
                   health::max_realloc.load(std::memory_order_relaxed));

  std::lock_guard<std::mutex> lk(g_mutex);
  if(g_reports.empty())
    {
      s += "no branches polled yet\n";
      return s;
    }

  for(const auto &[path,r] : g_reports)
    {
      if(!r.read_ok)
        {
          s += fmt::format("{}: device={} smart=unavailable\n",
                           path,(r.device.empty() ? "?" : r.device));
          continue;
        }
      s += fmt::format("{}: device={} temp={} pending={} realloc={} {}\n",
                       path,r.device,r.temp,r.pending,r.realloc,
                       (r.failing ? ("FAILING: " + r.reason) : "ok"));
    }

  return s;
}

std::string
health::resolve_disk(const std::string &mountpoint_)
{
  return ::_resolve_disk(mountpoint_);
}
