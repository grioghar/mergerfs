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
#include "fs_copy_file_range.hpp"
#include "fs_copyfile.hpp"
#include "fs_fadvise.hpp"
#include "fs_ficlone.hpp"
#include "fs_file_unchanged.hpp"
#include "fs_info.hpp"
#include "fs_open_beneath.hpp"
#include "fs_path.hpp"
#include "ioprio.hpp"
#include "qos.hpp"
#include "qos_class.hpp"
#include "qos_rules.hpp"
#include "syslog.hpp"

#include "fmt/core.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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
    // While a protected class has read either end of a move within
    // `hold_ns`, the copy is paced at `hold_rate` instead of `rate`
    // (0 == wait, not copy). The same rule a class carries as
    // `hold=`, applied to the daemon's own bulk traffic, which never
    // passes through throttle().
    u64         hold_ns   = 0;
    u64         hold_rate = (20ULL * 1024 * 1024);
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
  bool                    g_post_fork = false;

  Settings g_settings;

  // Totals and last-pass state, for the status key.
  u64         g_passes    = 0;
  u64         g_moved     = 0;
  u64         g_bytes     = 0;
  u64         g_skipped   = 0;
  u64         g_errors    = 0;
  // Files whose copy was slowed or paused by a hold.
  u64         g_holds     = 0;
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

  // strerror(3) is not thread safe and this runs beside the FUSE
  // threads, which also format errors.
  std::string
  _errstr(const int err_)
  {
    char buf[128];

    return ::strerror_r(err_,buf,sizeof(buf));
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
  //
  // Everything here is done through directory fds pinned by a walk
  // that refuses to follow symlinks (see fs_open_beneath.hpp). The
  // scan saw a regular file at this relative path some seconds ago;
  // nothing about the path may be trusted since. A path-based version
  // of this function, run as root against a branch that pool users can
  // write to, is a "delete any file on the host" primitive: swap a
  // directory in the chain for a symlink to /etc between the scan and
  // the unlink.
  // Which resources a move touches, for pacing it against playback.
  struct Pace
  {
    const Settings    *s;
    const std::string *src;
    const std::string *dst;
  };

  constexpr u64 COPY_CHUNK = (8ULL << 20);

  // True while a protected class has read either end within the hold
  // window.
  bool
  _held(const Pace &p_)
  {
    if((p_.s == nullptr) || (p_.s->hold_ns == 0))
      return false;

    return ((qos::protected_age_ns(*p_.src) <= p_.s->hold_ns) ||
            (qos::protected_age_ns(*p_.dst) <= p_.s->hold_ns));
  }

  // Copies the data in chunks, re-deciding the rate between chunks.
  //
  // Whole-file pacing (copy, then sleep off the difference) cannot
  // react to playback that starts partway through a 20GB file, and
  // that is exactly the case that matters: the copy is what makes the
  // stream stutter, and it has to slow *now*. The host-side governors
  // this replaces got that reaction from a cgroup io.max on the copy's
  // process; here the copy is our own loop, so it simply asks between
  // chunks.
  //
  // Returns bytes copied, or -errno. -EINTR when the mover is being
  // stopped.
  s64
  _paced_copy(const int          src_fd_,
              const struct stat &st_,
              const int          dst_fd_,
              const Pace        &pace_,
              bool              *was_held_)
  {
    // A reflink is free and instant: nothing to pace.
    if(fs::ficlone(src_fd_,dst_fd_) >= 0)
      return st_.st_size;

    const u64 total = static_cast<u64>(st_.st_size);

    fs::fadvise_sequential(src_fd_,0,total);

    u64  done        = 0;
    u64  paced_rate  = 0;   // the rate the current stretch is paced at
    u64  paced_start = 0;
    u64  paced_bytes = 0;
    bool use_cfr     = true;

    *was_held_ = false;

    while(done < total)
      {
        // ---- decide the rate for this chunk ----
        u64 rate = pace_.s ? pace_.s->rate : 0;

        if(::_held(pace_))
          {
            *was_held_ = true;
            rate = pace_.s->hold_rate;

            if(rate == 0)
              {
                // Wait it out rather than crawl: re-checked every half
                // second, and abandoned at once on stop.
                std::unique_lock<std::mutex> lk(g_mutex);

                if(g_cv.wait_for(lk,std::chrono::milliseconds(500),
                                 []{ return g_stop; }))
                  return -EINTR;

                continue;
              }
          }
        else
          {
            std::lock_guard<std::mutex> lk(g_mutex);
            if(g_stop)
              return -EINTR;
          }

        if(rate != paced_rate)
          {
            paced_rate  = rate;
            paced_start = ::_now_ns();
            paced_bytes = 0;
          }

        // ---- copy one chunk ----
        const u64 want = std::min<u64>(COPY_CHUNK,total - done);
        s64       rv;

        if(use_cfr)
          {
            s64 off_in  = static_cast<s64>(done);
            s64 off_out = static_cast<s64>(done);

            rv = fs::copy_file_range(src_fd_,&off_in,dst_fd_,&off_out,want,0);
            if((rv == -EINTR) || (rv == -EAGAIN))
              continue;
            if(rv < 0)
              {
                // Not supported across these filesystems on this
                // kernel: fall back for the rest of the file.
                use_cfr = false;
                continue;
              }
            if(rv == 0)
              return -EIO;   // shorter than fstat said
          }
        else
          {
            static thread_local std::vector<char> buf;
            if(buf.size() < COPY_CHUNK)
              buf.resize(COPY_CHUNK);

            rv = ::pread(src_fd_,buf.data(),want,static_cast<off_t>(done));
            if(rv < 0)
              {
                if(errno == EINTR)
                  continue;
                return -errno;
              }
            if(rv == 0)
              return -EIO;

            s64 wdone = 0;
            while(wdone < rv)
              {
                const s64 w = ::pwrite(dst_fd_,buf.data() + wdone,
                                       (rv - wdone),
                                       static_cast<off_t>(done + wdone));
                if(w < 0)
                  {
                    if(errno == EINTR)
                      continue;
                    return -errno;
                  }
                wdone += w;
              }
          }

        done        += static_cast<u64>(rv);
        paced_bytes += static_cast<u64>(rv);

        // ---- sleep off what this stretch is ahead of its rate ----
        if(paced_rate != 0)
          {
            const u64 now     = ::_now_ns();
            const u64 elapsed = ((now > paced_start) ? (now - paced_start) : 0);
            const u64 want_ns =
              static_cast<u64>((static_cast<unsigned __int128>(paced_bytes) *
                                NS_PER_SEC) / paced_rate);

            if(want_ns > elapsed)
              {
                std::unique_lock<std::mutex> lk(g_mutex);

                if(g_cv.wait_for(lk,std::chrono::nanoseconds(want_ns - elapsed),
                                 []{ return g_stop; }))
                  return -EINTR;
              }
          }
      }

    return static_cast<s64>(done);
  }

  int
  _move_one(const int          srcroot_,
            const int          dstroot_,
            const std::string &relpath_,
            const Pace        &pace_)
  {
    s64         rv;
    std::string base;

    const int srcdir = fs::open_parent_beneath(srcroot_,relpath_,&base);
    if(srcdir < 0)
      return srcdir;

    int src_fd = ::openat(srcdir,base.c_str(),
                          O_RDONLY | O_NOFOLLOW | O_NOATIME | O_CLOEXEC);
    if((src_fd < 0) && (errno == EPERM))
      src_fd = ::openat(srcdir,base.c_str(),O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if(src_fd < 0)
      {
        const int err = errno;
        ::close(srcdir);
        return -err;
      }

    struct stat st;
    if(::fstat(src_fd,&st) != 0)
      {
        const int err = errno;
        ::close(src_fd);
        ::close(srcdir);
        return -err;
      }

    // Re-checked on the open fd, not the scan's lstat: a link could
    // have been added since, and this is the one property whose loss
    // cannot be repaired afterwards.
    if(!S_ISREG(st.st_mode) || (st.st_nlink != 1))
      {
        ::close(src_fd);
        ::close(srcdir);
        return (S_ISREG(st.st_mode) ? -EMLINK : -EINVAL);
      }

    std::string dstbase;
    const int dstdir = fs::mkdir_parent_beneath(srcroot_,dstroot_,relpath_,&dstbase);
    if(dstdir < 0)
      {
        ::close(src_fd);
        ::close(srcdir);
        return dstdir;
      }

    // O_EXCL on a name nobody else has reason to use. Kept out of the
    // pool's namespace by the leading dot for the seconds it exists.
    static std::atomic<u64> seq{0};
    const std::string tmp = fmt::format(".mergerfs-mover.{}.{}",
                                        static_cast<int>(::getpid()),
                                        seq.fetch_add(1));

    const int dst_fd = ::openat(dstdir,tmp.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                0600);
    if(dst_fd < 0)
      {
        const int err = errno;
        ::close(dstdir);
        ::close(src_fd);
        ::close(srcdir);
        return -err;
      }

    auto abandon = [&](const int err_)
      {
        ::unlinkat(dstdir,tmp.c_str(),0);
        ::close(dst_fd);
        ::close(dstdir);
        ::close(src_fd);
        ::close(srcdir);
        return err_;
      };

    // Data first, paced against playback; then xattrs, attrs,
    // ownership, mode, times -- all onto the fd.
    bool was_held = false;
    rv = ::_paced_copy(src_fd,st,dst_fd,pace_,&was_held);
    if(rv < 0)
      return abandon(static_cast<int>(rv));

    if(was_held)
      {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_holds++;
      }

    rv = fs::copyfile_metadata(src_fd,st,dst_fd);
    if(rv < 0)
      return abandon(static_cast<int>(rv));

    // The source is about to be deleted on the strength of this copy,
    // so the copy has to be on stable storage first. This is the one
    // place in mergerfs that removes data and it is not on any I/O
    // path, so the cost is accepted.
    if(::fsync(dst_fd) != 0)
      return abandon(-errno);

    // A writer that raced the copy leaves a source newer than what was
    // read. copyfile's path variant retries; here the pass simply
    // moves on and the next one finds the file settled.
    if(fs::file_changed(src_fd,st) != FS_FILE_UNCHANGED)
      return abandon(-EBUSY);

    ::close(dst_fd);

    // Never overwrite: the kernel refuses atomically rather than this
    // code checking first and racing.
#ifdef SYS_renameat2
    rv = ::syscall(SYS_renameat2,dstdir,tmp.c_str(),dstdir,dstbase.c_str(),
                   static_cast<unsigned>(1) /* RENAME_NOREPLACE */);
#else
    rv = -1; errno = ENOSYS;
#endif
    if(rv != 0)
      {
        const int err = errno;
        ::unlinkat(dstdir,tmp.c_str(),0);
        ::close(dstdir);
        ::close(src_fd);
        ::close(srcdir);
        return -err;
      }

    ::close(dstdir);

    // Last look before the unlink: the name must still be the inode
    // that was copied. If the entry was replaced meanwhile, both
    // copies are left standing -- a duplicate is recoverable, a
    // deleted stranger's file is not.
    struct stat now;
    if((::fstatat(srcdir,base.c_str(),&now,AT_SYMLINK_NOFOLLOW) != 0) ||
       (now.st_ino != st.st_ino) || (now.st_dev != st.st_dev))
      {
        ::close(src_fd);
        ::close(srcdir);
        return -ESTALE;
      }

    if(::unlinkat(srcdir,base.c_str(),0) != 0)
      {
        // The copy is good but the source will not go away. Removing
        // the copy would need its directory fd back; the duplicate is
        // reported instead and the next pass skips it as EEXIST.
        const int err = errno;
        ::close(src_fd);
        ::close(srcdir);
        return -err;
      }

    ::close(src_fd);
    ::close(srcdir);

    return 0;
  }

  // Opens a branch root for the duration of a pass. O_PATH: it is only
  // ever a base for *at calls.
  int
  _open_root(const std::string &path_)
  {
    const int fd = ::open(path_.c_str(),O_PATH | O_DIRECTORY | O_CLOEXEC);

    return ((fd < 0) ? -errno : fd);
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
        g_last_error = fmt::format("{}: {}",relpath_,::_errstr(-rv_));
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
    // fewest chances to interrupt somebody. Only the head of the list
    // is ever used, so only the head is ordered.
    const std::size_t keep = std::min(candidates.size(),MAX_CANDIDATES);

    std::partial_sort(candidates.begin(),candidates.begin() + keep,candidates.end(),
                      [](const Candidate &a_, const Candidate &b_)
                      {
                        return (a_.size > b_.size);
                      });
    candidates.resize(keep);

    const int srcroot = ::_open_root(srcpath);
    if(srcroot < 0)
      return;
    const int dstroot = ::_open_root(dstpath);
    if(dstroot < 0)
      {
        ::close(srcroot);
        return;
      }
    struct Roots { int a, b; ~Roots() { ::close(a); ::close(b); } } roots{srcroot,dstroot};

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

        const Pace pace{&s_,&srcpath,&dstpath};
        const int  rv = ::_move_one(srcroot,dstroot,c.relpath,pace);

        if(rv == -EINTR)
          return;

        ::_account(rv,c.relpath,c.size);

        if(rv < 0)
          continue;

        freed  += c.size;
        moved++;

        dst->spaceavail -= c.size;
        budget          -= std::min(budget,c.size);

        // Pacing, including the explicit rate cap, happens inside the
        // copy now, chunk by chunk; see _paced_copy.
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

    // Coldest first; only the head is used, so only the head is ordered.
    const std::size_t keep = std::min(candidates.size(),MAX_CANDIDATES);

    std::partial_sort(candidates.begin(),candidates.begin() + keep,candidates.end(),
                      [](const Candidate &a_, const Candidate &b_)
                      {
                        return (a_.atime < b_.atime);
                      });
    candidates.resize(keep);

    const int srcroot = ::_open_root(srcpath);
    if(srcroot < 0)
      return;
    const int dstroot = ::_open_root(dstpath);
    if(dstroot < 0)
      {
        ::close(srcroot);
        return;
      }
    struct Roots { int a, b; ~Roots() { ::close(a); ::close(b); } } roots{srcroot,dstroot};

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

        const Pace pace{&s_,&srcpath,&dstpath};
        const int  rv = ::_move_one(srcroot,dstroot,c.relpath,pace);

        if(rv == -EINTR)
          return;

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
    if(g_started || !g_post_fork)
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
          else if(key == "hold")
            {
              std::string num  = val;
              u64         mult = 1;
              if(!num.empty() && ((num.back() == 's') || (num.back() == 'm')))
                {
                  mult = ((num.back() == 'm') ? 60 : 1);
                  num.pop_back();
                }
              const unsigned long n = ::strtoul(num.c_str(),&end,10);
              if(num.empty() || (end == num.c_str()) || (*end != '\0') ||
                 ((n * mult) > 3600))
                return -EINVAL;
              s.hold_ns = (static_cast<u64>(n) * mult * NS_PER_SEC);
            }
          else if(key == "hold-rate")
            {
              if(qos::parse_size(val,&s.hold_rate))
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
          g_settings.hold_ns,
          g_settings.hold_rate,
          g_holds,
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
                "pressure={:.2f} rate={} hold={}s hold-rate={} max-files={}\n",
                ::_policy_name(s.policy),
                s.interval,
                s.high,
                s.low,
                s.age_days,
                s.pressure,
                s.rate,
                (s.hold_ns / NS_PER_SEC),
                s.hold_rate,
                s.max_files);

  if(s.policy == MoverPolicy::TIME_BASED)
    out += fmt::format("from={} to={}\n",s.from,s.to);

  out += fmt::format("state={} passes={} moved={} bytes={} skipped={} "
                     "errors={} holds={}\n",
                     g_state,
                     g_passes,
                     g_moved,
                     g_bytes,
                     g_skipped,
                     g_errors,
                     g_holds);

  if(!g_last_error.empty())
    out += fmt::format("last-error={}\n",g_last_error);

  return out;
}

void
qos::mover::post_fork()
{
  std::lock_guard<std::mutex> lk(g_mutex);

  g_post_fork = true;

  if(g_settings.policy != MoverPolicy::OFF)
    ::_start_locked();
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

void
qos::mover::post_fork()
{
}

#endif
