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
  The whole QoS state as one JSON document.

  `qos.stats` is written for a person reading a terminal, and anything
  consuming it programmatically has to parse prose -- which is what a
  status-collector script ends up doing, badly, and re-doing every time
  a field is added. This is the same information in a form a dashboard
  can read directly: one xattr read, one document, no scraping.

  It is a snapshot taken subsystem by subsystem rather than under one
  global lock. Counters are monotonic and the locks involved are held
  for microseconds, so the worst case is two sections that disagree by
  a few requests -- which is a better trade than stalling the read and
  write paths to make a status page self consistent.
 */

#pragma once

#include "qos_class.hpp"

#include <string>


namespace qos
{
  // Named to avoid colliding with qos::stats(), which is the human
  // readable form of the same information.
  std::string stats_json();
}
