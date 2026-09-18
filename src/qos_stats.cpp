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

#include "qos_stats.hpp"

#if USE_QOS

#include "qos.hpp"
#include "qos_capacity.hpp"
#include "qos_govern.hpp"
#include "qos_gpu.hpp"
#include "qos_mover.hpp"

#include "fmt/core.h"

#include <string>


namespace
{
  // Minimal, and deliberately so: every string this emits is a branch
  // path, a class name or an strerror message. Escaping the characters
  // JSON forbids in a string is the whole requirement.
  std::string
  _esc(const std::string &s_)
  {
    std::string out;

    out.reserve(s_.size() + 8);

    for(const char c : s_)
      {
        switch(c)
          {
          case '"':  out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\b': out += "\\b";  break;
          case '\f': out += "\\f";  break;
          case '\n': out += "\\n";  break;
          case '\r': out += "\\r";  break;
          case '\t': out += "\\t";  break;
          default:
            if(static_cast<unsigned char>(c) < 0x20)
              out += fmt::format("\\u{:04x}",static_cast<unsigned>(c));
            else
              out += c;
            break;
          }
      }

    return out;
  }

  const char *
  _bool(const bool b_)
  {
    return (b_ ? "true" : "false");
  }
}

std::string
qos::stats_json()
{
  const qos::CoreInfo        core      = qos::core_info();
  const auto                 resources = qos::resources();
  const auto                 classes   = qos::class_stats();
  const qos::govern::Stats   govern    = qos::govern::stats();
  const qos::mover::Stats    mover     = qos::mover::stats();

  std::string out;

  out += "{\n";

  out += fmt::format("  \"enabled\": {},\n"
                     "  \"rules\": \"{}\",\n"
                     "  \"sleepers\": {},\n"
                     "  \"max_sleepers\": {},\n"
                     "  \"max_sleep_ms\": {},\n"
                     "  \"distress_floor_ms\": {},\n"
                     "  \"distress_factor\": {:.2f},\n",
                     ::_bool(core.enabled),
                     ::_esc(core.rules_path),
                     core.sleepers,
                     core.max_sleepers,
                     core.max_sleep_ms,
                     core.distress_floor_ms,
                     core.distress_factor);

  out += fmt::format("  \"gpu\": {{ \"supported\": {}, \"busy\": {}, "
                     "\"threshold\": {}, \"floor\": {:.2f} }},\n",
                     ::_bool(qos::gpu::supported()),
                     qos::gpu::busy(),
                     qos::gpu::threshold(),
                     qos::gpu::floor());

  out += fmt::format("  \"calibrating\": {},\n",
                     ::_bool(qos::capacity::running()));

  out += fmt::format("  \"govern\": {{ \"supported\": {}, \"interval\": {}, "
                     "\"sweeps\": {}, \"examined\": {}, \"matched\": {}, "
                     "\"reapplied\": {}, \"failed\": {} }},\n",
                     ::_bool(govern.supported),
                     govern.interval,
                     govern.sweeps,
                     govern.examined,
                     govern.matched,
                     govern.reapplied,
                     govern.failed);

  out += fmt::format("  \"mover\": {{ \"supported\": {}, \"policy\": \"{}\", "
                     "\"state\": \"{}\", \"interval\": {}, \"high\": {}, "
                     "\"low\": {}, \"pressure\": {:.2f}, \"rate\": {}, "
                     "\"passes\": {}, \"moved\": {}, \"bytes\": {}, "
                     "\"skipped\": {}, \"errors\": {}, "
                     "\"last_error\": \"{}\" }},\n",
                     ::_bool(mover.supported),
                     ::_esc(mover.policy),
                     ::_esc(mover.state),
                     mover.interval,
                     mover.high,
                     mover.low,
                     mover.pressure,
                     mover.rate,
                     mover.passes,
                     mover.moved,
                     mover.bytes,
                     mover.skipped,
                     mover.errors,
                     ::_esc(mover.last_error));

  out += "  \"resources\": [\n";
  for(std::size_t i = 0; i < resources.size(); i++)
    {
      const auto &r = resources[i];

      out += fmt::format("    {{ \"name\": \"{}\", \"contended\": {}, "
                         "\"pressure\": {:.2f}, \"observed_bps\": {}, "
                         "\"probed_bps\": {}, \"latency_ms\": {:.1f}, "
                         "\"baseline_ms\": {:.1f}, \"distress_events\": {} }}{}\n",
                         ::_esc(r.name),
                         ::_bool(r.contended),
                         r.pressure,
                         r.observed,
                         r.probed,
                         (static_cast<double>(r.latency_ewma_ns) / 1000000.0),
                         (static_cast<double>(r.latency_base_ns) / 1000000.0),
                         r.distress_events,
                         ((i + 1 < resources.size()) ? "," : ""));
    }
  out += "  ],\n";

  out += "  \"classes\": [\n";
  for(std::size_t i = 0; i < classes.size(); i++)
    {
      const auto &c = classes[i];

      out += fmt::format("    {{ \"name\": \"{}\", \"protect\": {}, "
                         "\"critical\": {}, \"govern\": {}, \"yield\": {}, "
                         "\"requests\": {}, \"bytes\": {}, \"throttled\": {}, "
                         "\"throttled_ms\": {}, \"passed\": {} }}{}\n",
                         ::_esc(c.name),
                         ::_bool(c.protect),
                         ::_bool(c.critical),
                         ::_bool(c.govern),
                         c.yield,
                         c.requests,
                         c.bytes,
                         c.throttled,
                         (c.throttled_ns / (1000 * 1000)),
                         c.passed,
                         ((i + 1 < classes.size()) ? "," : ""));
    }
  out += "  ]\n";

  out += "}\n";

  return out;
}

#else

std::string
qos::stats_json()
{
  return "{ \"enabled\": false, \"supported\": false }\n";
}

#endif
