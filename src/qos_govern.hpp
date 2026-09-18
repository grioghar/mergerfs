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
  Priority governor for the processes around the pool.

  The rest of the QoS machinery deals with I/O that flows *through*
  mergerfs, where the daemon is the only issuer the kernel can see.
  This deals with the other half: a media server also reads and writes
  outside the pool -- its own database, its metadata store, its
  transcode scratch directory -- and it burns CPU. Neither is flattened
  by FUSE, and both compete with playback.

  The conventional answer is a shell loop calling `renice` and `ionice`
  every few seconds. That works, but it lives outside the thing that
  knows what a class is, so the policy ends up written twice and drifts
  apart; and matching processes with `pgrep` is its own trap, because
  `comm` is capped at fifteen characters and frequently does not
  resemble the command line. Doing it here means one ruleset describes
  both halves, and the matching already understands full cmdlines.

  Mechanics that matter:

    * A class must say `govern` to be applied this way. Nothing reaches
      outside the daemon unless a rule asked for it.
    * Values are applied to every thread of a matched process, not just
      its main thread, because nice and ioprio are per-thread on Linux.
      A server that does its scanning on a worker thread is the normal
      case, not the exception.
    * mergerfs itself is never governed. Its threads are set per
      request by qos::Apply, and a periodic sweep would fight that.
    * `path` and `op` conditions cannot be evaluated here -- there is
      no request -- so rules that use them simply do not match, and
      classification falls through to the next rule.
 */

#pragma once

#include "base_types.h"
#include "qos_class.hpp"

#include <string>


namespace qos
{
  namespace govern
  {
    // Interval between sweeps, in seconds. Zero stops the governor.
    // Changing it takes effect at the next sweep.
    void set_interval(const u64 seconds);
    u64  interval();

    // Starts the sweep thread if it is not already running. Idempotent.
    void start();

    // Asks the thread to stop and waits for it. Safe to call when it
    // was never started.
    void stop();

    // What the last sweep did, for `qos.govern`.
    std::string status();

    struct Stats
    {
      bool supported;
      u64  interval;
      u64  sweeps;
      u64  examined;
      u64  matched;
      u64  reapplied;
      u64  failed;
    };

    Stats stats();
  }
}
