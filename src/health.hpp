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
  Branch health monitoring.

  A pool is only as trustworthy as its worst disk, and a disk usually warns
  before it dies: pending sectors accumulate, temperature climbs, the
  reallocation count creeps up. mergerfs is the one process that knows which
  filesystem every write is about to land on, so it is well placed to stop
  sending writes to a disk that is failing -- without an administrator being
  awake to notice.

  When a branch trips a threshold it is *quarantined*: switched to RO in the
  live branch list. Reads keep working, so nothing disappears from the pool
  and in-flight readers are undisturbed; only new writes are steered
  elsewhere. That is deliberately the mildest useful action.

  What this does NOT do is repair anything. Reallocating a pending sector
  means writing to it, and a filesystem daemon must never take that decision
  on its own -- the sector belongs to a file, and overwriting it destroys
  whatever it held. Repair is left to `mergerfs.diskrepair`, run by a human
  who has decided what the data is worth.
 */

#pragma once

#include "branches.hpp"

#include <atomic>
#include <string>


namespace health
{
  enum class Mode
    {
     OFF,          // no monitoring
     MONITOR,      // observe and report, change nothing
     QUARANTINE    // observe and set failing branches RO
    };

  extern std::atomic<u64> interval;      // seconds between polls
  extern std::atomic<u64> max_temp;      // celsius; 0 disables the check
  extern std::atomic<u64> max_pending;   // Current_Pending_Sector
  extern std::atomic<u64> max_realloc;   // Reallocated_Sector_Ct

  void set_mode(const Mode);
  Mode mode();

  // Registered with the maintenance thread; called roughly once a minute and
  // decides for itself whether enough time has passed to poll again.
  void tick(const u64 count);

  // Human readable per-branch health, for the qos/health status key.
  std::string status();

  // Re-check every branch immediately, ignoring the interval.
  void poll_now();
}
