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

#include "branch.hpp"
#include "branches.hpp"
#include "config.hpp"
#include "fs_info.hpp"
#include "health.hpp"
#include "ioprio.hpp"
#include "qos.hpp"
#include "qos_capacity.hpp"
#include "qos_govern.hpp"
#include "qos_gpu.hpp"
#include "qos_mover.hpp"

#include "fmt/core.h"

#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <mutex>
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

  // ---- host-side evidence beside each branch -------------------------
  //
  // A dashboard reading this document wants to know not just what the
  // QoS decided but whether the decision can mean anything: an ioprio
  // is a no-op on a disk running mq-deadline, and a branch at 95% busy
  // explains a stall better than any class counter. All of it is read
  // from /sys and /proc, none of it on an I/O path.

  std::string
  _read_file(const std::string &path_)
  {
    FILE *f = ::fopen(path_.c_str(),"r");
    if(f == nullptr)
      return {};

    char   buf[512];
    size_t n = ::fread(buf,1,sizeof(buf) - 1,f);

    ::fclose(f);

    buf[n] = '\0';

    return buf;
  }

  // "[bfq]" out of "none mq-deadline [bfq]"
  std::string
  _scheduler(const std::string &disk_)
  {
    const std::string s = ::_read_file("/sys/block/" + disk_ + "/queue/scheduler");

    const auto l = s.find('[');
    const auto r = s.find(']');

    if((l == std::string::npos) || (r == std::string::npos) || (r <= l))
      return {};

    return s.substr(l + 1,r - l - 1);
  }

  // Cumulative counters from /sys/block/<disk>/stat; rates are deltas
  // between two reads of this document, which is the cadence a
  // dashboard polls at. The first read of a disk reports zero rates.
  struct DiskRates
  {
    double util_pct  = 0;
    double await_ms  = 0;
    double read_bps  = 0;
    double write_bps = 0;
  };

  struct DiskSample
  {
    u64 ios      = 0;   // reads + writes completed
    u64 sectors_r = 0;
    u64 sectors_w = 0;
    u64 svc_ms   = 0;   // ms spent reading + writing (summed per request)
    u64 io_ticks = 0;   // ms the queue was non-empty
    u64 at_ns    = 0;
    DiskRates last;     // what the previous full window computed
  };

  // A window shorter than this is not measured, it is re-reported.
  //
  // getxattr is issued twice per read -- once to learn the size, once
  // for the value -- so the document is generated twice a millisecond
  // apart, and the copy the caller sees is the second. A window that
  // short holds one io_ticks tick and no completed request: "100% busy,
  // 0 MB/s". Concurrent readers (a dashboard polling every 5s and an
  // operator's getfattr) would likewise shorten each other's windows.
  // Holding the last full window's figures until this much time has
  // passed gives every reader the same, meaningful number.
  constexpr double MIN_WINDOW_S = 2.0;

  std::mutex                       g_disk_mutex;
  std::map<std::string,DiskSample> g_disk_prev;

  u64
  _now_ns()
  {
    struct timespec ts;

    ::clock_gettime(CLOCK_MONOTONIC,&ts);

    return ((static_cast<u64>(ts.tv_sec) * 1000000000ULL) +
            static_cast<u64>(ts.tv_nsec));
  }

  bool
  _disk_rates(const std::string &disk_,
              DiskRates         *out_)
  {
    const std::string s = ::_read_file("/sys/block/" + disk_ + "/stat");
    if(s.empty())
      return false;

    unsigned long long f[11] = {};
    if(::sscanf(s.c_str(),"%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                &f[0],&f[1],&f[2],&f[3],&f[4],&f[5],&f[6],&f[7],&f[8],&f[9],&f[10]) < 11)
      return false;

    DiskSample cur;
    cur.ios       = (f[0] + f[4]);
    cur.sectors_r = f[2];
    cur.sectors_w = f[6];
    cur.svc_ms    = (f[3] + f[7]);
    cur.io_ticks  = f[9];
    cur.at_ns     = ::_now_ns();

    std::lock_guard<std::mutex> lk(g_disk_mutex);

    DiskSample &prev = g_disk_prev[disk_];

    if((prev.at_ns != 0) && (cur.at_ns > prev.at_ns))
      {
        const double dt = (static_cast<double>(cur.at_ns - prev.at_ns) / 1e9);

        if(dt < MIN_WINDOW_S)
          {
            *out_ = prev.last;
            return true;
          }

        const u64 d_ios = ((cur.ios >= prev.ios) ? (cur.ios - prev.ios) : 0);
        const u64 d_svc = ((cur.svc_ms >= prev.svc_ms) ? (cur.svc_ms - prev.svc_ms) : 0);
        const u64 d_tk  = ((cur.io_ticks >= prev.io_ticks) ? (cur.io_ticks - prev.io_ticks) : 0);

        out_->util_pct  = std::min(100.0,(static_cast<double>(d_tk) / (dt * 1000.0)) * 100.0);
        out_->await_ms  = (d_ios ? (static_cast<double>(d_svc) / d_ios) : 0.0);
        out_->read_bps  = ((cur.sectors_r >= prev.sectors_r) ? (cur.sectors_r - prev.sectors_r) : 0) * 512.0 / dt;
        out_->write_bps = ((cur.sectors_w >= prev.sectors_w) ? (cur.sectors_w - prev.sectors_w) : 0) * 512.0 / dt;

        cur.last = *out_;
      }

    prev = cur;

    return true;
  }

  // "some avg10=1.23 avg60=4.56 ..." / "full ..." from /proc/pressure/io
  void
  _psi_io(double *some10_, double *some60_, double *full10_, double *full60_)
  {
    *some10_ = *some60_ = *full10_ = *full60_ = 0.0;

    const std::string s = ::_read_file("/proc/pressure/io");

    const char *p = ::strstr(s.c_str(),"some avg10=");
    if(p) ::sscanf(p,"some avg10=%lf avg60=%lf",some10_,some60_);
    p = ::strstr(s.c_str(),"full avg10=");
    if(p) ::sscanf(p,"full avg10=%lf avg60=%lf",full10_,full60_);
  }

  const char *
  _mode(const Branch::Mode m_)
  {
    switch(m_)
      {
      case Branch::Mode::RO: return "RO";
      case Branch::Mode::NC: return "NC";
      default:               return "RW";
      }
  }

  std::string
  _join(const std::vector<std::string> &v_)
  {
    std::string out;
    for(const auto &s : v_)
      out += (out.empty() ? "" : "|") + s;
    return out;
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

  {
    double s10,s60,f10,f60;
    ::_psi_io(&s10,&s60,&f10,&f60);

    out += fmt::format("  \"updated\": {},\n"
                       "  \"mount\": \"{}\",\n"
                       "  \"daemon\": {{ \"pid\": {}, \"nice\": {}, \"ioprio\": \"{}\" }},\n"
                       "  \"psi_io\": {{ \"some10\": {:.2f}, \"some60\": {:.2f}, "
                       "\"full10\": {:.2f}, \"full60\": {:.2f} }},\n",
                       static_cast<long long>(::time(nullptr)),
                       ::_esc(cfg.mountpoint.native()),
                       static_cast<int>(::getpid()),
                       ::getpriority(PRIO_PROCESS,0),
                       qos::ioprio::to_string(::ioprio::get(0)),
                       s10,s60,f10,f60);
  }

  {
    Branches::Ptr branches = cfg.branches;

    out += fmt::format("  \"pool\": {{ \"policy_create\": \"{}\", \"minfreespace\": \"{}\", "
                       "\"branches\": [\n",
                       ::_esc(cfg.func.create.to_string()),
                       ::_esc(cfg.minfreespace.to_string()));

    for(std::size_t i = 0; i < branches->size(); i++)
      {
        const Branch     &b    = (*branches)[i];
        const std::string path = b.path.native();
        const std::string disk = health::resolve_disk(path);
        DiskRates         r;
        fs::info_t        info;
        double            used_pct = -1;
        u64               avail    = 0;

        if(!disk.empty())
          ::_disk_rates(disk,&r);

        if(fs::info(b.path,&info) == 0)
          {
            const u64 total = (info.spaceavail + info.spaceused);
            if(total)
              used_pct = (100.0 * info.spaceused / total);
            avail = info.spaceavail;
          }

        out += fmt::format("    {{ \"path\": \"{}\", \"mode\": \"{}\", \"accept\": \"{}\", "
                           "\"reject\": \"{}\", \"device\": \"{}\", \"scheduler\": \"{}\", "
                           "\"util_pct\": {:.1f}, \"await_ms\": {:.1f}, \"read_bps\": {:.0f}, "
                           "\"write_bps\": {:.0f}, \"used_pct\": {:.1f}, \"avail_bytes\": {} }}{}\n",
                           ::_esc(path),
                           ::_mode(b.mode),
                           ::_esc(::_join(b.accept)),
                           ::_esc(::_join(b.reject)),
                           ::_esc(disk),
                           ::_esc(disk.empty() ? std::string{} : ::_scheduler(disk)),
                           r.util_pct,r.await_ms,r.read_bps,r.write_bps,
                           used_pct,
                           static_cast<unsigned long long>(avail),
                           ((i + 1 < branches->size()) ? "," : ""));
      }

    out += "  ] },\n";
  }

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

  {
    const auto procs = qos::govern::processes();

    out += fmt::format("  \"govern\": {{ \"supported\": {}, \"interval\": {}, "
                       "\"sweeps\": {}, \"examined\": {}, \"matched\": {}, "
                       "\"reapplied\": {}, \"failed\": {}, \"processes\": [\n",
                       ::_bool(govern.supported),
                       govern.interval,
                       govern.sweeps,
                       govern.examined,
                       govern.matched,
                       govern.reapplied,
                       govern.failed);

    for(std::size_t i = 0; i < procs.size(); i++)
      {
        const auto &p = procs[i];

        out += fmt::format("    {{ \"pid\": {}, \"comm\": \"{}\", \"class\": \"{}\", "
                           "\"nice\": {}, \"ioprio\": \"{}\" }}{}\n",
                           p.pid,
                           ::_esc(p.comm),
                           ::_esc(p.cls),
                           ((p.nice == qos::UNSET) ? 0 : p.nice),
                           qos::ioprio::to_string(p.ioprio),
                           ((i + 1 < procs.size()) ? "," : ""));
      }

    out += "  ] },\n";
  }

  out += fmt::format("  \"mover\": {{ \"supported\": {}, \"policy\": \"{}\", "
                     "\"state\": \"{}\", \"interval\": {}, \"high\": {}, "
                     "\"low\": {}, \"pressure\": {:.2f}, \"rate\": {}, "
                     "\"hold_s\": {}, \"hold_rate\": {}, \"holds\": {}, "
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
                     (mover.hold_ns / 1000000000ULL),
                     mover.hold_rate,
                     mover.holds,
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

      // Age of the last protected request, in ms; null if there has
      // never been one. A consumer compares it with a class's hold_s
      // to see whether that class is currently held on this resource.
      const std::string protected_age =
        ((r.protected_age_ns == UINT64_MAX)
         ? std::string("null")
         : std::to_string(r.protected_age_ns / 1000000ULL));

      out += fmt::format("    {{ \"name\": \"{}\", \"contended\": {}, "
                         "\"pressure\": {:.2f}, \"observed_bps\": {}, "
                         "\"probed_bps\": {}, \"latency_ms\": {:.1f}, "
                         "\"baseline_ms\": {:.1f}, \"distress_events\": {}, "
                         "\"protected_age_ms\": {} }}{}\n",
                         ::_esc(r.name),
                         ::_bool(r.contended),
                         r.pressure,
                         r.observed,
                         r.probed,
                         (static_cast<double>(r.latency_ewma_ns) / 1000000.0),
                         (static_cast<double>(r.latency_base_ns) / 1000000.0),
                         r.distress_events,
                         protected_age,
                         ((i + 1 < resources.size()) ? "," : ""));
    }
  out += "  ],\n";

  out += "  \"classes\": [\n";
  for(std::size_t i = 0; i < classes.size(); i++)
    {
      const auto &c = classes[i];

      out += fmt::format("    {{ \"name\": \"{}\", \"protect\": {}, "
                         "\"critical\": {}, \"govern\": {}, \"yield\": {}, "
                         "\"hold_s\": {}, "
                         "\"requests\": {}, \"bytes\": {}, \"throttled\": {}, "
                         "\"throttled_ms\": {}, \"passed\": {}, \"held\": {} }}{}\n",
                         ::_esc(c.name),
                         ::_bool(c.protect),
                         ::_bool(c.critical),
                         ::_bool(c.govern),
                         c.yield,
                         (c.hold_ns / 1000000000ULL),
                         c.requests,
                         c.bytes,
                         c.throttled,
                         (c.throttled_ns / (1000 * 1000)),
                         c.passed,
                         c.held,
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
