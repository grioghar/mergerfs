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
  Background relocation of existing files between branches.

  `balance` steers new creates towards the branches that are behind,
  which levels a pool over time but only as fast as new data arrives.
  A disk that is already full stays full. Closing that gap means moving
  data that is already written, which is what this does.

  Doing it inside the daemon rather than from a script is not about
  saving a cron entry. A mover is a bulk sequential reader and writer
  hitting two disks at once, which is the single most disruptive thing
  that can happen to a pool somebody is streaming from, and an external
  script has no way to know when that is. The daemon does: the QoS
  governor already tracks, per branch, whether a protected class is
  suffering. Wiring the mover to that signal means it runs flat out at
  three in the morning and gets out of the way within a second or two
  of somebody pressing play -- without a schedule, a guess, or a
  playback-detection heuristic of its own.

  Two policies, matching the two jobs the external movers did:

    percent-full  Move the largest files off any branch over `high`
                  until it drops below `low`, onto the emptiest branch
                  that can take them. This is the rebalance.

    time-based    Move files not accessed in `age` days from branch
                  `from` to branch `to`. This is tiering: cold media
                  off the fast disk, or off the disk that is about to
                  be retired.

  Safety, which matters more here than speed, since this is the only
  part of mergerfs that deletes data:

    * Copy, verify, rename, and only then unlink the source.
      fs::copyfile does the first three and re-copies if the source
      changed underneath it.
    * Never move a file with more than one link. Copying it would
      silently break the hardlink into two independent files, and no
      amount of care afterwards can put that back together.
    * Never overwrite: a relative path that already exists on the
      destination is skipped, not merged.
    * Never move onto a branch that is RO or NC, or that lacks room
      for the file plus its minfreespace.
    * A process holding the file open keeps reading the copy it
      already has, by ordinary unlink semantics. It sees no error.
 */

#pragma once

#include "base_types.h"
#include "qos_class.hpp"

#include <string>


namespace qos
{
  namespace mover
  {
    // Applies a `key=value,...` specification. Returns 0, or -EINVAL
    // with nothing changed.
    int configure(const std::string_view spec);

    // Settings, state and totals, for `qos.mover`.
    std::string status();

    struct Stats
    {
      bool        supported;
      std::string policy;
      std::string state;
      u64         interval;
      u64         high;
      u64         low;
      double      pressure;
      u64         rate;
      u64         hold_ns;
      u64         hold_rate;
      u64         holds;
      u64         passes;
      u64         moved;
      u64         bytes;
      u64         skipped;
      u64         errors;
      std::string last_error;
    };

    Stats stats();

    void stop();

    // See qos::govern::post_fork(): a policy given at mount time is
    // recorded, and its thread is created from FUSE::init.
    void post_fork();
  }
}
