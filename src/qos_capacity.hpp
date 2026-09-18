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
  Active measurement of what each branch can actually deliver.

  Passive observation (see qos.hpp) is free and always running, but it
  can only ever see as much throughput as something asked for: a pool
  that has never been driven hard looks slow. This saturates one branch
  at a time and records a real ceiling, which is what turns a
  percentage rate into a number.

  The probe is deliberately cautious about running against a pool that
  is in use:

    * Reads only, unless writes are explicitly asked for. Nothing in
      the pool is created or modified on the read path.
    * O_DIRECT, so it neither measures the page cache nor evicts what
      the running services still want.
    * Idle I/O priority and nice 19 on the probing thread, so a probe
      that collides with playback yields instead of competing. That
      also means a figure measured while the pool is busy is a floor,
      not a ceiling -- calibrate when it is quiet.
    * Off the FUSE threads entirely, and one branch at a time, so a
      calibration never occupies a request-serving thread and never
      has two disks fighting each other for the bus.

  This is the in-daemon equivalent of the `mergerfs.qos-bench` tool,
  and exists so a pool does not need an external benchmark run and a
  hand-edited `capacity` line to make percentage rates work.
 */

#pragma once

#include "base_types.h"
#include "fs_path.hpp"
#include "qos_class.hpp"

#include <string>
#include <vector>


namespace qos
{
  namespace capacity
  {
    // Starts a background calibration of `branches`. Returns 0 on
    // success, -EBUSY when one is already running, and -EINVAL when
    // there is nothing to probe.
    //
    // Results are installed as they are measured, so a long run over
    // many branches improves the ruleset progressively rather than
    // only at the end.
    int start(const std::vector<fs::path> &branches,
              const u64                    seconds,
              const bool                   allow_write);

    // Human readable progress and results, for `qos.calibrate`.
    std::string status();

    // True while a probe is in flight. The mover consults this so its
    // own I/O does not corrupt the measurement it is waiting on.
    bool running();

    // Asks a running probe to stop and waits for it. Called at
    // unmount: a probe thread still touching governor state while the
    // process's statics are being destroyed is a crash on the way out.
    void stop();
  }
}
