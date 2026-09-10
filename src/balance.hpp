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
  Conditional write concentration.

  Spreading every new file across all branches keeps them evenly full, but it
  also keeps every disk awake. On a pool of USB spinners that is the worst of
  both worlds: no disk gets to idle, they all run warm, and an imbalance that
  does appear takes forever to close because each branch only receives its
  share.

  This concentrates new writes onto the branches that are actually behind, and
  only while they are behind. A branch is "behind" when its used percentage is
  at least `gap` below the fullest branch in the pool. Once at least `count`
  branches qualify, every create goes to the emptiest of them until the gap
  closes on its own -- at which point the normal create policy resumes and
  nothing is being steered at all.

  The effect is that the pool self-levels during ordinary use, the disks that
  are already full stop being written to (and can spin down), and no
  administrator has to schedule a rebalance to move terabytes after the fact.

  It deliberately does not move existing data. Only new creates are steered.
 */

#pragma once

#include "branch.hpp"
#include "branches.hpp"

#include <atomic>
#include <string>


namespace balance
{
  extern std::atomic<bool> _enabled;

  [[gnu::always_inline]]
  inline
  bool
  enabled()
  {
    return _enabled.load(std::memory_order_relaxed);
  }

  void enable(const bool);

  // A branch counts as behind when (fullest_used_pct - its_used_pct) >= gap.
  extern std::atomic<u64> gap;

  // How many branches must be behind before concentration kicks in. With the
  // default of 1, a single lagging disk is enough; raising it means the pool
  // only intervenes when several disks have fallen behind.
  extern std::atomic<u64> count;

  // Never steer writes onto a branch with less than this free, whatever the
  // percentages say.
  extern std::atomic<u64> min_free;

  // Chooses a target among the lagging branches, or returns nullptr when the
  // pool is level enough and the caller should use its normal policy.
  Branch *target(const Branches::Ptr &);

  // Human readable explanation of the current decision.
  std::string status(const Branches::Ptr &);
}
