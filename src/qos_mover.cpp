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

#include "qos_mover.hpp"

#include <errno.h>

#if USE_QOS && USE_QOS_MOVER

#include "branch.hpp"
#include "branches.hpp"
#include "config.hpp"
#include "fs_clonepath.hpp"
#include "fs_copyfile.hpp"
#include "fs_info.hpp"
#include "fs_path.hpp"
#include "ioprio.hpp"
#include "qos.hpp"
#include "qos_class.hpp"
#include "qos_rules.hpp"
#include "syslog.hpp"

#include "fmt/core.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


namespace
{
  constexpr u64 NS_PER_SEC = 1000000000ULL;

  // Bound on how much of a branch is examined in one pass. A scan is
  // not the expensive part -- the copying is -- but an unbounded walk
  // of a 14TB disk holds directory state for minutes and delays the
  // first useful move, so a pass takes what it finds and the next one
  // continues.
  constexpr u64 MAX_SCAN_ENTRIES = 200000;

  // How many candidates to keep. The policy only ever moves a few
  // files per pass, and sorting the whole disk to pick them would be
  // work thrown away.
  constexpr std::size_t MAX_CANDIDATES = 512;

  // mergerfs already has a global `Policy` (create/search policies),
  // which wins name lookup inside namespace qos::mover.
  enum class MoverPolicy
    {
     OFF,
     PERCENT_FULL,
     TIME_BASED
    };

  struct Settings
  {
    MoverPolicy policy    = MoverPolicy::OFF;
    u64         interval  = 60;
    u64         high      = 90;
    u64         low       = 85;
    u64         age_days  = 90;
    double      pressure  = 0.05;
    u64         rate      = 0;   // bytes/sec, 0 == unlimited
    u64         max_files = 0;   // per pass, 0 == unlimited
    std::string from;
    std::string to;
  };

  struct Candidate
  {
    std::string relpath;
    u64         size;
    u64         atime;
  };

  std::mutex              g_mutex;
  std::condition_variable g_cv;
  std::thread             g_thread;
  bool                    g_stop    = false;
  bool                    g_started = false;

  Settings g_settings;

  // Totals and last-pass state, for the status key.
  u64         g_passes    = 0;
  u64         g_moved     = 0;
  u64         g_bytes     = 0;
  u64         g_skipped   = 0;
  u64         g_errors    = 0;
  std::string g_last_error;
  std::string g_state = "idle";

  u64
  _now_ns()
  {
    struct timespec ts;

    ::clock_gettime(CLOCK_MONOTONIC,&ts);

    return ((static_cast<u64>(ts.tv_sec) * NS_PER_SEC) +
            static_cast<u64>(ts.tv_nsec));
  }

  const char *
  _policy_name(const MoverPolicy p_)
  {
    switch(p_)
      {
      case MoverPolicy::PERCENT_FULL: return "percent-full";
      case MoverPolicy::TIME_BASED:   return "time-based";
      default:                        return "off";
      }
  }

  void
  _set_state(const std::string &s_)
  {
    std::lock_guard<std::mutex> lk(g_mutex);

    g_state = s_;
  }

  // True when either end of a prospective move is under enough
  // pressure that moving would be felt by whoever is being protected.
  //
  // Checked against both branches because a move is a read from one
  // and a write to the other, and a player streaming off *either* is a
  // reason to wait.
  bool
  _too_busy(const Settings    &s_,
            const std::string &src_,
            const std::string &dst_)
  {
    return ((qos::pressure(src_) > s_.pressure) ||
            (qos::pressure(dst_) > s_.pressure));
  }

  struct Survey
  {
    Branch *branch;
    u64     used_pct;
    u64     spaceavail;
    u64     minfreespace;
  };

  std::vector<Survey>
  _survey(const Branches::Ptr &branches_)
  {
    std::vector<Survey> out;

    for(auto &branch : *branches_)
      {
        fs::info_t info;

        if(fs::info(branch.path,&info) < 0)
          continue;
        if(info.readonly)
          continue;

        const u64 total = (info.spaceavail + info.spaceused);
        if(total == 0)
          continue;

        out.push_back({&branch,
                       ((info.spaceused * 100) / total),
                       info.spaceavail,
                       branch.minfreespace()});
      }

    return out;
  }

  // Walks `root_` collecting regular files that `keep_` accepts.
  //
  // Hardlinked files are rejected here rather than at move time so
  // they never occupy a candidate slot: copying one would split it
  // into two independent files, which is data corruption of a kind
  // that cannot be undone later.
  void
  _scan(const std::string             &root_,
        std::vector<Candidate>        &out_,
        const std::function<bool(const Candidate&)> &keep_)
  {
    u64                     seen = 0;
    std::deque<std::string> queue{""};

    while(!queue.empty())
      {
        const std::string rel = queue.front();
        queue.pop_front();

        const std::string dir = (rel.empty() ? root_ : (root_ + "/" + rel));

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
                return;
              }

            const std::string childrel = (rel.empty()
                                          ? std::string(de->d_name)
                                          : (rel + "/" + de->d_name));

            struct stat st;
            if(::lstat((root_ + "/" + childrel).c_str(),&st) != 0)
              continue;

            if(S_ISDIR(st.st_mode))
              {
                queue.push_back(childrel);
                continue;
              }

            if(!S_ISREG(st.st_mode))
              continue;

            if(st.st_nlink > 1)
              continue;

            Candidate c{childrel,
                        static_cast<u64>(st.st_size),
                        static_cast<u64>(st.st_atime)};

            if(!keep_(c))
              continue;

            out_.push_back(std::move(c));
          }

        ::closedir(d);
      }
  }

  // Copy, verify, rename, unlink. Returns 0, or a negative errno with
  // the source left exactly as it was.
  int
  _move_one(const std::string &src_branch_,
            const std::string &dst_branch_,
            const std::string &relpath_)
  {
    s64 rv;

    const fs::path src_branch(src_branch_);
    const fs::path dst_branch(dst_branch_);
    const fs::path relpath(relpath_);

    const fs::path src_file = (src_branch / relpath);
    const fs::path dst_file = (dst_branch / relpath);

    // Refusing to overwrite is deliberate. A path existing on both
    // branches is what mergerfs calls a duplicate, and resolving one
    // by picking a winner is a policy decision this has no business
    // making silently.
    struct stat st;
    if(::lstat(dst_file.c_str(),&st) == 0)
      return -EEXIST;

    // Recreate the directory chain on the destination with the same
    // ownership, permissions and timestamps the source carries.
    const fs::path relparent = relpath.parent_path();
    if(!relparent.empty())
      {
        rv = fs::clonepath(src_branch,dst_branch,relparent);
        if(rv < 0)
          return static_cast<int>(rv);
      }

    fs::CopyFileFlags flags = {};
    flags.cleanup_failure = 1;

    rv = fs::copyfile(src_file,dst_file,flags);
    if(rv < 0)
      return static_cast<int>(rv);

    // Only now is it safe to remove the original.
    if(::unlink(src_file.c_str()) != 0)
      {
        // The copy is good but the source will not go away. Remove the
        // copy rather than leave a duplicate behind: a duplicate is a
        // state the pool has to resolve on every lookup afterwards.
        const int err = errno;

        ::unlink(dst_file.c_str());

        return -err;
      }

    return 0;
  }

  // Records the outcome of one move under the lock. EEXIST is expected
  // and benign: the path already exists on the destination, so the
  // file stays where it is. Counting it as an error makes a healthy
  // pool with a few duplicates look like a failing mover.
  void
  _account(const int          rv_,
           const std::string &relpath_,
           const u64          size_)
  {
    std::lock_guard<std::mutex> lk(g_mutex);

    if(rv_ == -EEXIST)
      {
        g_skipped++;
      }
    else if(rv_ < 0)
      {
        g_errors++;
        g_last_error = fmt::format("{}: {}",relpath_,::strerror(-rv_));
      }
    else
      {
        g_moved++;
        g_bytes += size_;
      }
  }

  void
  _do_percent_full(const Settings      &s_,
                   const Branches::Ptr &branches_)
  {
    std::vector<Survey> survey = ::_survey(branches_);

    if(survey.size() < 2)
      return;

    std::sort(survey.begin(),survey.end(),
              [](const Survey &a_, const Survey &b_)
              {
                return (a_.used_pct > b_.used_pct);
              });

    Survey &src = survey.front();

    if(src.used_pct < s_.high)
      return;

    // Emptiest branch that can actually be written to.
    Survey *dst = nullptr;
    for(auto i = survey.rbegin(); i != survey.rend(); ++i)
      {
        if(i->branch == src.branch)
          continue;
        if(i->branch->ro_or_nc())
          continue;

        dst = &(*i);
        break;
      }

    if(dst == nullptr)
      return;

    // `low` is the level the pool is being levelled towards, so the
    // destination has to sit below it to have room in the band. A
    // destination that is already at the target only moves the
    // problem across the bus.
    if(dst->used_pct >= s_.low)
      return;

    const std::string srcpath = src.branch->path.native();
    const std::string dstpath = dst->branch->path.native();

    std::vector<Candidate> candidates;
    candidates.reserve(MAX_CANDIDATES);

    ::_scan(srcpath,candidates,
            [](const Candidate &c_)
            {
              // A file with nothing in it frees nothing and still
              // costs a create on the far side.
              return (c_.size > 0);
            });

    if(candidates.empty())
      return;

    // Largest first: the fewest moves that close the gap is also the
    // fewest chances to interrupt somebody.
    std::sort(candidates.begin(),candidates.end(),
              [](const Candidate &a_, const Candidate &b_)
              {
                return (a_.size > b_.size);
              });

    if(candidates.size() > MAX_CANDIDATES)
      candidates.resize(MAX_CANDIDATES);

    // How many bytes have to leave for the branch to drop under `low`.
    u64 target = 0;
    {
      fs::info_t info;

      if(fs::info(src.branch->path,&info) < 0)
        return;

      const u64 total     = (info.spaceavail + info.spaceused);
      const u64 want_used = ((total * s_.low) / 100);

      if(info.spaceused <= want_used)
        return;

      target = (info.spaceused - want_used);
    }

    // How much the destination can take before it, too, reaches the
    // target band. Without this the run is bounded only by the
    // destination running out of space, which turns levelling a pool
    // into filling one disk from another.
    u64 budget = 0;
    {
      fs::info_t info;

      if(fs::info(dst->branch->path,&info) < 0)
        return;

      const u64 total    = (info.spaceavail + info.spaceused);
      const u64 want_used = ((total * s_.low) / 100);

      if(info.spaceused >= want_used)
        return;

      budget = (want_used - info.spaceused);
    }

    u64 freed = 0;
    u64 moved = 0;

    for(const auto &c : candidates)
      {
        if(freed >= target)
          break;
        if(s_.max_files && (moved >= s_.max_files))
          break;

        {
          std::lock_guard<std::mutex> lk(g_mutex);
          if(g_stop)
            return;
        }

        if(::_too_busy(s_,srcpath,dstpath))
          {
            ::_set_state("paused: pool busy");
            return;
          }

        // Counted, not silently dropped. "moved=0" with no further
        // explanation is indistinguishable from a broken mover, and
        // the usual cause is this line: minfreespace defaults to 4GiB,
        // so a branch smaller than that never accepts anything.
        if(dst->spaceavail < (c.size + dst->minfreespace))
          {
            std::lock_guard<std::mutex> lk(g_mutex);

            g_skipped++;
            g_last_error = fmt::format("{}: destination needs {} free "
                                       "(file {} + minfreespace {}), has {}",
                                       c.relpath,
                                       (c.size + dst->minfreespace),
                                       c.size,
                                       dst->minfreespace,
                                       dst->spaceavail);
            continue;
          }

        // Stop rather than skip: the candidates are largest first, so
        // once one does not fit the budget the run has done what this
        // pass can usefully do. The next pass re-surveys.
        if(c.size > budget)
          {
            if(budget < (c.size / 2))
              break;

            std::lock_guard<std::mutex> lk(g_mutex);
            g_skipped++;
            continue;
          }

        // The move is bulk sequential I/O on both disks and it does
        // not pass through the FUSE path, so the governor would
        // otherwise never learn there is anything here willing to give
        // way. Saying so is what lets playback latency push back on
        // it at all.
        qos::note_yielding(srcpath);
        qos::note_yielding(dstpath);

        const u64 t0 = ::_now_ns();
        const int rv = ::_move_one(srcpath,dstpath,c.relpath);
        const u64 elapsed = (::_now_ns() - t0);

        ::_account(rv,c.relpath,c.size);

        if(rv < 0)
          continue;

        freed  += c.size;
        moved++;

        dst->spaceavail -= c.size;
        budget          -= std::min(budget,c.size);

        // An explicit rate cap, on top of idle priority and the
        // pressure gate. Sleeping for the time the copy "should" have
        // taken is crude, but it is the only lever that works when the
        // destination is fast enough that nothing ever registers as
        // contended.
        //
        // The wait is on the condition variable rather than a plain
        // sleep so that shutdown does not have to wait out a pacing
        // delay sized for a 20GB file.
        if(s_.rate != 0)
          {
            const u64 want_ns =
              static_cast<u64>((static_cast<unsigned __int128>(c.size) *
                                NS_PER_SEC) / s_.rate);

            if(want_ns > elapsed)
              {
                std::unique_lock<std::mutex> lk(g_mutex);

                g_cv.wait_for(lk,
                              std::chrono::nanoseconds(want_ns - elapsed),
                              []{ return g_stop; });
              }
          }
      }
  }

  void
  _do_time_based(const Settings      &s_,
                 const Branches::Ptr &branches_)
  {
    if(s_.from.empty() || s_.to.empty())
      return;

    Branch *src = nullptr;
    Branch *dst = nullptr;

    for(auto &branch : *branches_)
      {
        if(branch.path.native() == s_.from)
          src = &branch;
        if(branch.path.native() == s_.to)
          dst = &branch;
      }

    if((src == nullptr) || (dst == nullptr))
      return;
    if(dst->ro_or_nc())
      return;

    const std::string srcpath = src->path.native();
    const std::string dstpath = dst->path.native();

    fs::info_t dstinfo;
    if(fs::info(dst->path,&dstinfo) < 0)
      return;

    const u64 cutoff = (static_cast<u64>(::time(nullptr)) -
                        (s_.age_days * 24 * 60 * 60));

    std::vector<Candidate> candidates;

    ::_scan(srcpath,candidates,
            [cutoff](const Candidate &c_)
            {
              return ((c_.size > 0) && (c_.atime < cutoff));
            });

    if(candidates.empty())
      return;

    // Coldest first.
    std::sort(candidates.begin(),candidates.end(),
              [](const Candidate &a_, const Candidate &b_)
              {
                return (a_.atime < b_.atime);
              });

    if(candidates.size() > MAX_CANDIDATES)
      candidates.resize(MAX_CANDIDATES);

    u64 avail = dstinfo.spaceavail;
    u64 moved = 0;

    for(const auto &c : candidates)
      {
        if(s_.max_files && (moved >= s_.max_files))
          break;

        {
          std::lock_guard<std::mutex> lk(g_mutex);
          if(g_stop)
            return;
        }

        if(::_too_busy(s_,srcpath,dstpath))
          {
            ::_set_state("paused: pool busy");
            return;
          }

        if(avail < (c.size + dst->minfreespace()))
          {
            std::lock_guard<std::mutex> lk(g_mutex);

            g_skipped++;
            g_last_error = fmt::format("{}: destination needs {} free "
                                       "(file {} + minfreespace {}), has {}",
                                       c.relpath,
                                       (c.size + dst->minfreespace()),
                                       c.size,
                                       dst->minfreespace(),
                                       avail);
            break;
          }

        qos::note_yielding(srcpath);
        qos::note_yielding(dstpath);

        const int rv = ::_move_one(srcpath,dstpath,c.relpath);

        ::_account(rv,c.relpath,c.size);

        if(rv < 0)
          continue;

        avail -= c.size;
        moved++;
      }
  }

  void
  _pass()
  {
    Settings s;

    {
      std::lock_guard<std::mutex> lk(g_mutex);

      s = g_settings;
      g_passes++;
    }

    if(s.policy == MoverPolicy::OFF)
      return;

    Branches::Ptr branches = cfg.branches;

    ::_set_state("scanning");

    switch(s.policy)
      {
      case MoverPolicy::PERCENT_FULL:
        ::_do_percent_full(s,branches);
        break;
      case MoverPolicy::TIME_BASED:
        ::_do_time_based(s,branches);
        break;
      default:
        break;
      }

    {
      std::lock_guard<std::mutex> lk(g_mutex);

      if(g_state != "paused: pool busy")
        g_state = "idle";
    }
  }

  void
  _loop()
  {
    // The mover is the lowest priority thing in the system by
    // definition: everything it does is work that did not need doing
    // right now.
    ::setpriority(PRIO_PROCESS,0,19);
    ::ioprio::set(0,qos::ioprio::value(qos::ioprio::CLASS_IDLE,0));

    for(;;)
      {
        u64 interval;

        {
          std::unique_lock<std::mutex> lk(g_mutex);

          interval = g_settings.interval;

          if(g_settings.policy == MoverPolicy::OFF)
            g_cv.wait(lk,[]{ return (g_stop ||
                                     (g_settings.policy != MoverPolicy::OFF)); });
          else
            g_cv.wait_for(lk,std::chrono::seconds(interval),
                          []{ return g_stop; });

          if(g_stop)
            return;

          if(g_settings.policy == MoverPolicy::OFF)
            continue;
        }

        ::_pass();
      }
  }

  void
  _start_locked()
  {
    if(g_started)
      return;

    g_stop    = false;
    g_started = true;
    g_thread  = std::thread(::_loop);
  }
}

int
qos::mover::configure(const std::string_view spec_)
{
  Settings s;

  {
    std::lock_guard<std::mutex> lk(g_mutex);

    s = g_settings;
  }

  std::string       str(spec_);
  std::size_t       pos = 0;

  while(pos <= str.size())
    {
      const std::size_t comma = str.find(',',pos);
      const std::string tok   = str.substr(pos,
                                           ((comma == std::string::npos)
                                            ? std::string::npos
                                            : (comma - pos)));

      if(!tok.empty())
        {
          const std::size_t eq = tok.find('=');
          if(eq == std::string::npos)
            return -EINVAL;

          const std::string key = tok.substr(0,eq);
          const std::string val = tok.substr(eq + 1);

          char *end = nullptr;

          if(key == "policy")
            {
              if(val == "off")
                s.policy = MoverPolicy::OFF;
              else if(val == "percent-full")
                s.policy = MoverPolicy::PERCENT_FULL;
              else if(val == "time-based")
                s.policy = MoverPolicy::TIME_BASED;
              else
                return -EINVAL;
            }
          else if(key == "interval")
            {
              const unsigned long n = ::strtoul(val.c_str(),&end,10);
              if((end == val.c_str()) || (*end != '\0') || (n == 0))
                return -EINVAL;
              s.interval = n;
            }
          else if(key == "high")
            {
              const unsigned long n = ::strtoul(val.c_str(),&end,10);
              if((end == val.c_str()) || (*end != '\0') || (n > 100))
                return -EINVAL;
              s.high = n;
            }
          else if(key == "low")
            {
              const unsigned long n = ::strtoul(val.c_str(),&end,10);
              if((end == val.c_str()) || (*end != '\0') || (n > 100))
                return -EINVAL;
              s.low = n;
            }
          else if(key == "age")
            {
              const unsigned long n = ::strtoul(val.c_str(),&end,10);
              if((end == val.c_str()) || (*end != '\0'))
                return -EINVAL;
              s.age_days = n;
            }
          else if(key == "pressure")
            {
              const double d = ::strtod(val.c_str(),&end);
              if((end == val.c_str()) || (*end != '\0') ||
                 (d < 0.0) || (d > 1.0))
                return -EINVAL;
              s.pressure = d;
            }
          else if(key == "rate")
            {
              if(qos::parse_size(val,&s.rate))
                return -EINVAL;
            }
          else if(key == "max-files")
            {
              const unsigned long n = ::strtoul(val.c_str(),&end,10);
              if((end == val.c_str()) || (*end != '\0'))
                return -EINVAL;
              s.max_files = n;
            }
          else if(key == "from")
            {
              s.from = val;
            }
          else if(key == "to")
            {
              s.to = val;
            }
          else
            {
              return -EINVAL;
            }
        }

      if(comma == std::string::npos)
        break;

      pos = (comma + 1);
    }

  // A low water mark at or above the high one would move files until
  // the branch was empty.
  if(s.low >= s.high)
    return -EINVAL;

  if((s.policy == MoverPolicy::TIME_BASED) && (s.from.empty() || s.to.empty()))
    return -EINVAL;

  if((s.policy == MoverPolicy::TIME_BASED) && (s.from == s.to))
    return -EINVAL;

  {
    std::lock_guard<std::mutex> lk(g_mutex);

    g_settings = s;

    if(s.policy != MoverPolicy::OFF)
      ::_start_locked();
  }

  g_cv.notify_all();

  return 0;
}

qos::mover::Stats
qos::mover::stats()
{
  std::lock_guard<std::mutex> lk(g_mutex);

  return {true,
          ::_policy_name(g_settings.policy),
          g_state,
          g_settings.interval,
          g_settings.high,
          g_settings.low,
          g_settings.pressure,
          g_settings.rate,
          g_passes,
          g_moved,
          g_bytes,
          g_skipped,
          g_errors,
          g_last_error};
}

std::string
qos::mover::status()
{
  std::lock_guard<std::mutex> lk(g_mutex);

  const Settings &s = g_settings;

  std::string out =
    fmt::format("policy={} interval={} high={} low={} age={} "
                "pressure={:.2f} rate={} max-files={}\n",
                ::_policy_name(s.policy),
                s.interval,
                s.high,
                s.low,
                s.age_days,
                s.pressure,
                s.rate,
                s.max_files);

  if(s.policy == MoverPolicy::TIME_BASED)
    out += fmt::format("from={} to={}\n",s.from,s.to);

  out += fmt::format("state={} passes={} moved={} bytes={} skipped={} "
                     "errors={}\n",
                     g_state,
                     g_passes,
                     g_moved,
                     g_bytes,
                     g_skipped,
                     g_errors);

  if(!g_last_error.empty())
    out += fmt::format("last-error={}\n",g_last_error);

  return out;
}

void
qos::mover::stop()
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

#else

/*
  Built without the background mover.

  `balance` still steers new creates towards the branches that are
  behind; what is missing is the relocation of data already written.
 */

int
qos::mover::configure(const std::string_view)
{
  return -EOPNOTSUPP;
}

std::string
qos::mover::status()
{
  return "unsupported (built without USE_QOS_MOVER)\n";
}

qos::mover::Stats
qos::mover::stats()
{
  return {false,"off","unsupported",0,0,0,0.0,0,0,0,0,0,0,{}};
}

void
qos::mover::stop()
{
}

#endif
