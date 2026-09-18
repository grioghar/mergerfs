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
  GPU utilisation as an input to the I/O governor.

  The GPU does no work for mergerfs and cannot: classifying a request
  is a string match costing a few hundred nanoseconds, and a dispatch
  to a device costs tens of microseconds before any work begins. On a
  code path whose entire purpose is protecting playback latency, that
  trade is backwards.

  What the GPU is good for here is *evidence*. On a media server the
  video engine is busy exactly when hardware transcodes are running,
  which is to say when somebody is watching something. Two things
  follow that the disk-side signals cannot see on their own:

    * A saturated video engine means further transcodes fall back to
      software, which changes both the CPU picture and the read
      pattern. The pool is about to get harder to serve, and bulk
      traffic should be giving way before the latency shows it.
    * A busy engine is a positive statement that playback is happening
      even during the gaps between reads -- a player with a full buffer
      issues no I/O for seconds at a time, which the latency governor
      reads as "nobody is streaming".

  So this contributes a *floor* under the governor's pressure rather
  than a term in its feedback: while the engine is busy, yielding
  classes never return to full speed, however quiet the disks look.
  The measured latency loop still runs on top and can push pressure
  higher.

  Source is the AMD amdgpu sysfs counter, which is what an integrated
  Radeon exposes and what the pool this was written for has. Intel i915
  and NVIDIA do not publish an equivalent single percentage here; on
  those, and on a host with no GPU at all, this reports unavailable and
  changes nothing.

  Optional three times over, because a signal read off one vendor's
  sysfs node has no business being mandatory:

    * Compile time -- `make USE_QOS_GPU=0` removes it, and every entry
      point below becomes an inert stub. Nothing else in the daemon
      needs to know, so no caller is conditionally compiled.
    * Platform -- the implementation is Linux only regardless of the
      flag, since it reads /sys.
    * Run time -- off unless `qos.gpu` is given a threshold, and a host
      where no counter is found reports unsupported and changes
      nothing.

  There is no link time dependency in any configuration: this opens a
  file in sysfs and parses an integer. It does not link against, load,
  or require a GPU driver, runtime or userspace library.
 */

#pragma once

#include "base_types.h"
#include "qos_class.hpp"

#include <string>


namespace qos
{
  namespace gpu
  {
    // Percent busy, 0-100, or -1 when no counter could be read. Cached
    // and refreshed at most a few times a second: this is consulted
    // from the throttle path and a sysfs read per request would cost
    // far more than the signal is worth.
    int busy();

    // False when the signal was compiled out, when the platform has no
    // sysfs, or when no counter could be found. Callers use this to
    // tell "no GPU policy wanted" apart from "GPU policy asked for and
    // not available".
    bool supported();

    // Above `threshold` percent, pressure on a contended resource is
    // held at no less than `floor`. A threshold of 0 disables the
    // signal entirely, which is the default.
    void   set_threshold(const u32 percent, const double floor);
    u32    threshold();
    double floor();

    // Pressure floor to apply right now: `floor` when the engine is
    // over the threshold, and 0.0 otherwise or when unavailable.
    double pressure_floor();

    std::string status();
  }
}
