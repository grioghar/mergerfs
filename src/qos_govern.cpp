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

#include "qos_govern.hpp"

#if USE_QOS && USE_QOS_GOVERN && defined(__linux__)

#include "ioprio.hpp"
#include "procfs.hpp"
#include "qos.hpp"
#include "qos_class.hpp"
#include "qos_rules.hpp"
#include "syslog.hpp"

#include "fmt/core.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>


using qos::Class;
using qos::Direction;
using qos::RuleSet;
using qos::Subject;

namespace
{
  // What was last applied to a process, so a steady state costs no
  // syscalls at all. `ntasks` is carried because a process that has
  // spawned new threads since the last sweep needs them covered:
  // a thread created before its parent was governed does not inherit
  // the new values.
  struct Applied
  {
    const Class *cls    = nullptr;
    std::size_t  ntasks = 0;
    bool         seen   = false;
  };

  std::mutex                             g_mutex;
  std::condition_variable                g_cv;
  std::thread                            g_thread;
  bool                                   g_stop = false;
  bool                                   g_started = false;
  std::atomic<u64>                       g_interval{0};
  std::unordered_map<pid_t,Applied>      g_applied;

  // Last sweep's summary, for the status key.
  u64 g_sweeps    = 0;
  u64 g_examined  = 0;
  u64 g_matched   = 0;
  u64 g_changed   = 0;
  u64 g_failed    = 0;

  bool
  _all_digits(const char *s_)
  {
    if(*s_ == '\0')
      return false;

    for(const char *p = s_; *p != '\0'; p++)
      {
        if((*p < '0') || (*p > '9'))
          return false;
      }

    return true;
  }

  // uid and gid of a process, read from /proc/<pid>/status. Returns
  // false when the process is gone, which during a /proc walk is
  // entirely ordinary.
  bool
  _ids_of(const pid_t pid_,
          u32        *uid_,
          u32        *gid_)
  {
    struct stat st;

    // The owner of /proc/<pid> is the process's real uid/gid, which
    // saves parsing `status` line by line.
    const std::string path = fmt::format("/proc/{}",static_cast<int>(pid_));

    if(::stat(path.c_str(),&st) != 0)
      return false;

    *uid_ = static_cast<u32>(st.st_uid);
    *gid_ = static_cast<u32>(st.st_gid);

    return true;
  }

  std::vector<pid_t>
  _tasks_of(const pid_t pid_)
  {
    std::vector<pid_t> rv;

    const std::string path = fmt::format("/proc/{}/task",
                                         static_cast<int>(pid_));

    DIR *d = ::opendir(path.c_str());
    if(d == nullptr)
      return rv;

    struct dirent *de;
    while((de = ::readdir(d)) != nullptr)
      {
        if(!::_all_digits(de->d_name))
          continue;

        rv.push_back(static_cast<pid_t>(::atoi(de->d_name)));
      }

    ::closedir(d);

    return rv;
  }

  // Applies a class to every thread of a process. Returns the number
  // of threads that could not be set, which is normally zero and
  // becomes non-zero when the daemon lacks the privilege to lower
  // somebody else's nice value.
  u64
  _apply(const std::vector<pid_t> &tasks_,
         const Class              *cls_)
  {
    u64 failed = 0;

    const int ioprio = cls_->effective_govern_ioprio();
    const int nice   = cls_->effective_govern_nice();

    for(const auto tid : tasks_)
      {
        if(ioprio != qos::UNSET)
          {
            if(::ioprio::set(tid,ioprio) < 0)
              failed++;
          }

        if(nice != qos::UNSET)
          {
            // setpriority reports failure as -1, which is also a legal
            // priority, so errno is the only reliable signal.
            errno = 0;
            if((::setpriority(PRIO_PROCESS,tid,nice) != 0) &&
               (errno != 0))
              failed++;
          }
      }

    return failed;
  }

  void
  _sweep(const RuleSet *rs_,
         const pid_t    self_)
  {
    u64 examined = 0;
    u64 matched  = 0;
    u64 changed  = 0;
    u64 failed   = 0;

    DIR *d = ::opendir("/proc");
    if(d == nullptr)
      return;

    for(auto &[pid,a] : g_applied)
      a.seen = false;

    struct dirent *de;
    while((de = ::readdir(d)) != nullptr)
      {
        if(!::_all_digits(de->d_name))
          continue;

        const pid_t pid = static_cast<pid_t>(::atoi(de->d_name));

        // Never govern ourselves. qos::Apply sets each worker thread
        // per request; a sweep would overwrite it with whatever class
        // the daemon as a whole happened to match, and the two would
        // fight every interval.
        if(pid == self_)
          continue;

        examined++;

        u32 uid = 0;
        u32 gid = 0;
        if(!::_ids_of(pid,&uid,&gid))
          continue;

        const std::string cgroup = (rs_->needs_cgroup()
                                    ? procfs::get_cgroup(pid)
                                    : std::string{});
        const std::string comm   = (rs_->needs_comm()
                                    ? procfs::get_name(pid)
                                    : std::string{});
        const std::string cmdline = (rs_->needs_cmdline()
                                     ? procfs::get_cmdline(pid)
                                     : std::string{});

        // A kernel thread has an empty cmdline and nothing worth
        // reprioritising; skipping them keeps the sweep off a few
        // hundred entries on a busy host.
        if(cmdline.empty() && comm.empty())
          continue;

        Subject subject;
        subject.cgroup  = &cgroup;
        subject.comm    = &comm;
        subject.cmdline = &cmdline;
        subject.path    = nullptr;   // no request, so no path
        subject.uid     = uid;
        subject.gid     = gid;
        subject.dir     = Direction::READ;

        const Class *cls = rs_->classify(subject);

        if((cls == nullptr) || !cls->govern)
          continue;

        // Only a process some rule *named* is touched. The default
        // class catches everything a ruleset did not describe, which
        // for pool I/O is the right fallback -- but applied here it
        // would renice init, sshd and the shell of whoever is trying
        // to fix it. `default` carrying `govern` is therefore inert for
        // the sweep, by design and not by accident.
        if(cls == rs_->default_class())
          continue;

        if((cls->effective_govern_ioprio() == qos::UNSET) &&
           (cls->effective_govern_nice()   == qos::UNSET))
          continue;

        matched++;

        const std::vector<pid_t> tasks = ::_tasks_of(pid);
        if(tasks.empty())
          continue;

        Applied &a = g_applied[pid];

        a.seen = true;

        // Steady state is free: a process whose class has not changed
        // and which has not spawned threads is left alone entirely.
        if((a.cls == cls) && (a.ntasks == tasks.size()))
          continue;

        failed += ::_apply(tasks,cls);

        a.cls    = cls;
        a.ntasks = tasks.size();

        changed++;
      }

    ::closedir(d);

    // Forget processes that have exited, so a long-lived mount does
    // not accumulate an entry per pid the machine has ever run.
    for(auto i = g_applied.begin(); i != g_applied.end(); )
      i = (i->second.seen ? std::next(i) : g_applied.erase(i));

    g_sweeps++;
    g_examined = examined;
    g_matched  = matched;
    g_changed  = changed;
    g_failed   = failed;
  }

  void
  _loop()
  {
    const pid_t self = ::getpid();

    for(;;)
      {
        u64 interval;

        {
          std::unique_lock<std::mutex> lk(g_mutex);

          interval = g_interval.load(std::memory_order_relaxed);

          // Interval zero means "off": wait to be woken rather than
          // spinning, so turning the governor back on is immediate.
          if(interval == 0)
            g_cv.wait(lk,[]{ return (g_stop ||
                                     (g_interval.load(std::memory_order_relaxed) != 0)); });
          else
            g_cv.wait_for(lk,std::chrono::seconds(interval),
                          []{ return g_stop; });

          if(g_stop)
            return;

          if(g_interval.load(std::memory_order_relaxed) == 0)
            continue;
        }

        RuleSet::Ptr rs = qos::ruleset();

        if((rs == nullptr) || !rs->has_govern())
          continue;

        std::lock_guard<std::mutex> lk(g_mutex);

        ::_sweep(rs.get(),self);
      }
  }
}

void
qos::govern::set_interval(const u64 seconds_)
{
  {
    // Taken even though the value itself is atomic: the sweep thread
    // evaluates this as a condition variable predicate under this
    // mutex, and a store that slipped between its predicate check and
    // its wait would be a notify nobody was waiting for yet.
    std::lock_guard<std::mutex> lk(g_mutex);

    g_interval.store(seconds_,std::memory_order_relaxed);
  }

  g_cv.notify_all();

  // Nothing to run before there is an interval, so the thread is
  // created on first use rather than at mount.
  if(seconds_ != 0)
    qos::govern::start();
}

u64
qos::govern::interval()
{
  return g_interval.load(std::memory_order_relaxed);
}

void
qos::govern::start()
{
  std::lock_guard<std::mutex> lk(g_mutex);

  if(g_started)
    return;

  g_stop    = false;
  g_started = true;
  g_thread  = std::thread(::_loop);
}

void
qos::govern::stop()
{
  {
    std::lock_guard<std::mutex> lk(g_mutex);

    if(!g_started)
      return;

    g_stop = true;
  }

  g_cv.notify_all();

  if(g_thread.joinable())
    g_thread.join();

  std::lock_guard<std::mutex> lk(g_mutex);
  g_started = false;
}

qos::govern::Stats
qos::govern::stats()
{
  std::lock_guard<std::mutex> lk(g_mutex);

  return {true,
          g_interval.load(std::memory_order_relaxed),
          g_sweeps,
          g_examined,
          g_matched,
          g_changed,
          g_failed};
}

std::string
qos::govern::status()
{
  std::lock_guard<std::mutex> lk(g_mutex);

  const u64 interval = g_interval.load(std::memory_order_relaxed);

  if(interval == 0)
    return "interval=0 (disabled)\n";

  return fmt::format("interval={} sweeps={} examined={} matched={} "
                     "reapplied={} failed={}\n",
                     interval,
                     g_sweeps,
                     g_examined,
                     g_matched,
                     g_changed,
                     g_failed);
}

#else

/*
  Built without the client process governor, or built for a platform
  where nice and ioprio are not per-thread values reachable through
  /proc.

  Pool I/O is unaffected -- qos::Apply still sets the worker thread
  serving each request. What is missing is only the sweep that reaches
  outside the daemon to the client processes themselves.
 */

void
qos::govern::set_interval(const u64)
{
}

u64
qos::govern::interval()
{
  return 0;
}

void
qos::govern::start()
{
}

void
qos::govern::stop()
{
}

std::string
qos::govern::status()
{
  return "unsupported (built without USE_QOS_GOVERN)\n";
}

qos::govern::Stats
qos::govern::stats()
{
  return {false,0,0,0,0,0,0};
}

#endif
