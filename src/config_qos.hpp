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

#pragma once

#include "tofrom_string.hpp"


// qos=<bool>
class QoS : public ToFromString
{
public:
  QoS(const bool);

public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// qos.rules=<filepath>
//
// Assigning a path loads it. Assigning the same path again is how a
// ruleset is reloaded at runtime. A file that fails to parse leaves
// the running ruleset in place.
class QoSRules : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// qos.ruleset -- read only, the ruleset as parsed.
class QoSRuleSet : public ToFromString
{
public:
  QoSRuleSet();

public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// qos.stats -- counters. Assigning "reset" zeroes them.
class QoSStats : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// qos.max-sleepers=<int>
class QoSMaxSleepers : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// qos.distress-ms=<int>
class QoSDistressMS : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// qos.distress-factor=<float>
class QoSDistressFactor : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// qos.max-sleep-ms=<int>
class QoSMaxSleepMS : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// qos.calibrate=<seconds>[,write]
//
// Measures what each branch can actually deliver, which is what a
// percentage rate is resolved against. Assigning starts a probe in the
// background and returns immediately; reading reports progress and the
// figures measured so far.
//
// "write" additionally measures write throughput by creating and
// removing a temporary file on each branch. Reads only by default,
// because a pool in use is the normal case.
class QoSCalibrate : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// qos.govern=<seconds>
//
// Interval of the sweep that applies `govern` classes to the client
// processes themselves. Zero disables it. Reading reports what the
// last sweep did.
class QoSGovern : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// qos.gpu=<threshold-percent>[,<pressure-floor>]
//
// Uses GPU utilisation as evidence that playback is happening. While
// the video engine is at or above `threshold` percent busy, pressure
// on a disk that was recently streamed from is held at no less than
// `pressure-floor` (0.0-1.0, default 0.5), so yielding classes do not
// race back up to full speed during the gaps when a player's buffer is
// full and it is issuing no reads.
//
// A threshold of 0 disables the signal, which is the default. Reading
// reports the counters found and the current utilisation.
class QoSGPU : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// qos.mover=<key=value>[,<key=value>...]
//
// Background relocation of existing files between branches, paced by
// the QoS governor so it yields to playback rather than to a schedule.
//
//   policy=off|percent-full|time-based
//   interval=<seconds>      between passes                 (60)
//   high=<percent>          start moving above this        (90)
//   low=<percent>           stop moving below this         (85)
//   age=<days>              time-based: untouched for this (90)
//   from=<branch path>      time-based: source
//   to=<branch path>        time-based: destination
//   pressure=<0.0-1.0>      pause above this backoff       (0.05)
//   rate=<size>             cap, e.g. 50M; 0 is unlimited  (0)
//   max-files=<n>           per pass; 0 is unlimited       (0)
//
// Keys not given keep their current values, so a single key can be
// adjusted without restating the rest. Reading reports the settings,
// the current state, and the totals.
class QoSMover : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// qos.stats.json -- read only.
//
// Everything qos.stats reports, plus the governor, mover, GPU and
// calibration state, as one JSON document. Intended for a dashboard or
// a metrics exporter: one read, no parsing of prose.
class QoSStatsJSON : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};
