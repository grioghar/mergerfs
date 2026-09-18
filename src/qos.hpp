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

/*
  Per-client quality of service.

  mergerfs is a single daemon serving every process that touches the
  pool, so from the kernel's point of view all pool I/O is issued by
  one cgroup and one set of threads. Block layer priority
  (ionice/BFQ, cgroup io.weight) therefore cannot tell a media player
  apart from a torrent client: they are flattened into the same
  issuer.

  This restores the distinction inside the daemon. Each request is
  attributed to the calling process, matched against a ruleset, and
  the worker thread is given the ioprio and nice value of the class it
  matched for the duration of the request. Optionally the class is
  also rate limited.

  Compared to the `proxy_ioprio` option, which copies whatever ioprio
  the caller happens to have, the policy lives in one place and does
  not depend on every client being correctly ioniced by whoever
  started it -- which matters most for clients that respawn workers,
  or that live in a container the daemon does not control.
 */

#pragma once

#include "qos_class.hpp"
#include "qos_rules.hpp"

#include "fuse_req_ctx.h"

#include <atomic>
#include <string>
#include <vector>


// Defined by the build; defaulted here so the header is self contained.
#ifndef USE_QOS
#define USE_QOS 1
#endif

#if !USE_QOS

/*
  Built without per-client QoS.

  The point of this arm is that the read and write paths do not merely
  skip the work, they do not contain it. `enabled()` is a constant
  expression, the Apply constructor has an empty body, and throttle and
  the timing pair are inline no-ops, so every call in fuse_read and
  fuse_write folds away at compile time and no branch, atomic load or
  relocation survives into the binary.
 */

namespace qos
{
  constexpr
  bool
  enabled()
  {
    return false;
  }

  inline void enable(const bool) {}

  class Apply
  {
  public:
    inline
    Apply(const fuse_req_ctx_t *,
          const std::string *,
          const Direction)
    {
    }

    const Class   *cls() const { return nullptr; }
    const RuleSet *ruleset() const { return nullptr; }
  };

  inline u64  timing_start(const Apply &) { return 0; }
  inline void timing_end(const Apply &, const std::string &, const u64) {}
  inline void throttle(const Apply &, const u64, const std::string &) {}

  inline double pressure(const std::string &) { return 0.0; }
  inline void   note_yielding(const std::string &) {}
  inline u64    measured_capacity(const std::string &) { return 0; }
  inline void   set_probed_capacity(const std::string &, const u64) {}
  inline void   note_throughput(const std::string &, const u64) {}
}

#else

namespace qos
{
  // Exposed so the disabled-path check inlines into every
  // fuse_read/fuse_write as an atomic load and a branch.
  extern std::atomic<bool> _enabled;

  [[gnu::always_inline]]
  inline
  bool
  enabled()
  {
    return _enabled.load(std::memory_order_relaxed);
  }

  void enable(const bool);

  // Replaces the active ruleset. Safe to call while requests are in
  // flight.
  void set_ruleset(RuleSet::Ptr);
  RuleSet::Ptr ruleset();

  // Loads and installs a ruleset from `path`. On failure the existing
  // ruleset is left untouched and `err` describes the problem.
  int load_file(const std::string &path, std::string *err);

  // Number of process threads allowed to be sleeping in a throttle at
  // once, and the longest any single request may be delayed.
  //
  // Both exist to bound the damage throttling can do. mergerfs hands
  // requests from its read threads to a *bounded* process thread
  // queue, so a sleeping process thread is a process thread not
  // serving anyone -- and once the queue backs up, requests of every
  // class queue behind it. Rather than let a rate limit turn into a
  // pool-wide stall, a request that would exceed either bound is
  // allowed through unthrottled and counted in the `passed` stat.
  extern std::atomic<int> max_sleepers;
  extern std::atomic<u64> max_sleep_ns;

  // What counts as a protected request being in distress: its smoothed
  // service time must exceed `distress_factor` times the quietest time
  // that resource has managed, and also exceed `distress_floor_ns`.
  //
  // The floor is what stops ordinary jitter tripping the loop, and it
  // is entirely device dependent -- 50ms is a stall on a spinning disk
  // and unreachable on NVMe. A flash pool wants single-digit
  // milliseconds or the governor will never engage.
  extern std::atomic<u64>    distress_floor_ns;
  extern std::atomic<double> distress_factor;

  class Apply;

  // Path most recently loaded by load_file(), or empty.
  std::string rules_path();

  std::string stats();
  void        reset_stats();

  // Feedback loop.
  //
  // A protected class's service time is the control signal. When reads
  // for a player start taking materially longer than that same disk
  // manages when it is quiet, pressure on the resource rises and every
  // yielding class's allowance shrinks in proportion to its `yield`
  // rank -- downloads first and hardest, a player's own background
  // work more gently, playback not at all. When the latency recovers,
  // or playback stops altogether, pressure decays and the allowances
  // return to full.
  //
  // Returns 0 for a request whose class is not protected, in which
  // case timing_end does nothing. Keeping the clock read out of the
  // unprotected path is why this is split in two.
  u64  timing_start(const Apply &);
  void timing_end(const Apply &, const std::string &resource, const u64 started);

  // Current backoff for a resource, 0.0 to 1.0. Zero when no protected
  // class has touched it recently, which is what lets bulk traffic run
  // flat out on a disk nobody is streaming from.
  double pressure(const std::string &resource);

  // ---- capacity -----------------------------------------------------
  //
  // What the daemon believes `resource` can deliver, in bytes/sec, or
  // zero when it has no idea. This is the number a percentage rate is
  // resolved against, and it is measured rather than declared.
  //
  // Two sources, in order of authority:
  //
  //   probed   -- an explicit O_DIRECT read probe (qos.calibrate),
  //               which saturates the device and so measures a real
  //               ceiling.
  //   observed -- the best rate seen in a one second window that
  //               carried enough requests to mean anything. Free, but
  //               it can only ever see as much as was asked for, so it
  //               reads as a floor on the true capacity.
  //
  // An explicit `capacity` line in the rules file still outranks both;
  // see RuleSet::capacity().
  u64  measured_capacity(const std::string &resource);
  void set_probed_capacity(const std::string &resource, const u64 bytes_per_sec);

  // Charges `bytes` against the resource's throughput window. Called
  // for every request of every class, including the ones that are
  // never throttled -- playback is the traffic that best shows what a
  // disk can do, and excluding it would bias the estimate low.
  void note_throughput(const std::string &resource, const u64 bytes);

  // Snapshot of every resource the daemon has seen, for stats and for
  // the mover's pacing decisions.
  struct ResourceInfo
  {
    std::string name;
    u64         observed;
    u64         probed;
    double      pressure;
    bool        contended;
    u64         latency_ewma_ns;
    u64         latency_base_ns;
    u64         distress_events;
  };

  std::vector<ResourceInfo> resources();

  // Per-class counters and the descriptive bits a reader needs to
  // interpret them. Snapshotted together so a consumer cannot see one
  // class's numbers from before a reset and another's from after.
  struct ClassInfo
  {
    std::string name;
    u64         requests;
    u64         bytes;
    u64         throttled;
    u64         throttled_ns;
    u64         passed;
    bool        protect;
    bool        critical;
    bool        govern;
    u32         yield;
  };

  std::vector<ClassInfo> class_stats();

  struct CoreInfo
  {
    bool        enabled;
    int         sleepers;
    int         max_sleepers;
    u64         max_sleep_ms;
    u64         distress_floor_ms;
    double      distress_factor;
    std::string rules_path;
  };

  CoreInfo core_info();

  // Records that a class willing to yield has just issued I/O against
  // a resource, which is what makes it count as contended.
  void note_yielding(const std::string &resource);

  // Charges `bytes` to the class and, if that class is over its rate
  // for `resource`, sleeps for as long as the bounds above permit.
  // Called with the size the caller asked for, before the I/O is
  // issued.
  //
  // `resource` is the branch serving the request, so a rate is a
  // per-device allowance: a class limited to 10% does not have to
  // share one budget across seven disks.
  void throttle(const Apply &, const u64 bytes, const std::string &resource);

  // Classifies the calling process and applies its class to this
  // thread. The thread keeps those settings until another request
  // changes them, so nothing is restored on destruction -- FUSE
  // worker threads do nothing between requests worth protecting, and
  // restoring would double the syscalls on the hot path.
  class Apply
  {
  public:
    [[gnu::always_inline]]
    inline
    Apply(const fuse_req_ctx_t *ctx_,
          const std::string    *fusepath_,
          const Direction       dir_)
    {
      if(qos::enabled())
        _slow_apply(ctx_,fusepath_,dir_);
    }

    // nullptr when QoS is off or the ruleset cannot change anything.
    const Class   *cls() const { return _cls; }
    const RuleSet *ruleset() const { return _rs; }

  private:
    void _slow_apply(const fuse_req_ctx_t *,
                     const std::string *,
                     const Direction);

  private:
    const Class   *_cls = nullptr;
    const RuleSet *_rs  = nullptr;
  };
}

#endif
