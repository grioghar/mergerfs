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

#include "config_qos.hpp"

#include "qos_class.hpp"   // build-time feature flags
#include "qos_stats.hpp"

#include <errno.h>

#if USE_QOS

#include "config.hpp"
#include "from_string.hpp"
#include "qos.hpp"
#include "qos_capacity.hpp"
#include "qos_govern.hpp"
#include "qos_gpu.hpp"
#include "qos_mover.hpp"
#include "qos_stats.hpp"
#include "syslog.hpp"

#include "fmt/core.h"

#include <errno.h>
#include <stdlib.h>


QoS::QoS(const bool b_)
{
  qos::enable(b_);
}

std::string
QoS::to_string(void) const
{
  return (qos::enabled() ? "true" : "false");
}

int
QoS::from_string(const std::string_view s_)
{
  int rv;
  bool enable;

  rv = str::from(s_,&enable);
  if(rv)
    return rv;

  qos::enable(enable);

  return 0;
}

std::string
QoSRules::to_string(void) const
{
  return qos::rules_path();
}

int
QoSRules::from_string(const std::string_view s_)
{
  int rv;
  std::string err;
  const std::string path{s_};

  rv = qos::load_file(path,&err);
  if(rv < 0)
    {
      SysLog::error("qos.rules: {}: {}",path,err);
      return rv;
    }

  SysLog::info("qos.rules: loaded {}",path);

  return 0;
}

QoSRuleSet::QoSRuleSet()
{
  ro = true;
}

std::string
QoSRuleSet::to_string(void) const
{
  qos::RuleSet::Ptr rs = qos::ruleset();

  if(rs == nullptr)
    return {};

  return rs->to_string();
}

int
QoSRuleSet::from_string(const std::string_view)
{
  return -EINVAL;
}

std::string
QoSStats::to_string(void) const
{
  return qos::stats();
}

int
QoSStats::from_string(const std::string_view s_)
{
  if(s_ != "reset")
    return -EINVAL;

  qos::reset_stats();

  return 0;
}

std::string
QoSMaxSleepers::to_string(void) const
{
  return std::to_string(qos::max_sleepers.load(std::memory_order_relaxed));
}

int
QoSMaxSleepers::from_string(const std::string_view s_)
{
  int rv;
  int n;

  rv = str::from(s_,&n);
  if(rv)
    return rv;
  // -1 keeps the automatic sizing.
  if(n < -1)
    return -EINVAL;

  qos::max_sleepers.store(n,std::memory_order_relaxed);

  return 0;
}

std::string
QoSMaxSleepMS::to_string(void) const
{
  return std::to_string(qos::max_sleep_ns.load(std::memory_order_relaxed) /
                        (1000 * 1000));
}

int
QoSMaxSleepMS::from_string(const std::string_view s_)
{
  int rv;
  int n;

  rv = str::from(s_,&n);
  if(rv)
    return rv;
  if(n < 0)
    return -EINVAL;

  qos::max_sleep_ns.store(static_cast<u64>(n) * 1000 * 1000,
                          std::memory_order_relaxed);

  return 0;
}

std::string
QoSDistressMS::to_string(void) const
{
  return std::to_string(qos::distress_floor_ns.load(std::memory_order_relaxed) /
                        (1000 * 1000));
}

int
QoSDistressMS::from_string(const std::string_view s_)
{
  int rv;
  int n;

  rv = str::from(s_,&n);
  if(rv)
    return rv;
  if(n < 0)
    return -EINVAL;

  qos::distress_floor_ns.store(static_cast<u64>(n) * 1000 * 1000,
                               std::memory_order_relaxed);

  return 0;
}

std::string
QoSDistressFactor::to_string(void) const
{
  return fmt::format("{:.2f}",
                     qos::distress_factor.load(std::memory_order_relaxed));
}

int
QoSDistressFactor::from_string(const std::string_view s_)
{
  const std::string str{s_};
  char *end = nullptr;

  errno = 0;
  const double v = ::strtod(str.c_str(),&end);
  if(errno || (end == str.c_str()) || (*end != '\0') || (v < 1.0))
    return -EINVAL;

  qos::distress_factor.store(v,std::memory_order_relaxed);

  return 0;
}

std::string
QoSCalibrate::to_string(void) const
{
  return qos::capacity::status();
}

int
QoSCalibrate::from_string(const std::string_view s_)
{
  u64         seconds = 3;
  bool        allow_write = false;
  std::string arg(s_);

  // "<seconds>", "write", or "<seconds>,write" -- and an empty
  // assignment means "probe with the defaults", which is what makes
  // `setfattr -n user.mergerfs.qos.calibrate -v ""` a usable trigger.
  const auto comma = arg.find(',');
  if(comma != std::string::npos)
    {
      if(arg.substr(comma + 1) != "write")
        return -EINVAL;

      allow_write = true;
      arg.erase(comma);
    }
  else if(arg == "write")
    {
      allow_write = true;
      arg.clear();
    }

  if(!arg.empty())
    {
      char *end = nullptr;
      const unsigned long n = ::strtoul(arg.c_str(),&end,10);

      if((end == arg.c_str()) || (*end != '\0') || (n == 0))
        return -EINVAL;

      seconds = static_cast<u64>(n);
    }

  Branches::Ptr branches = cfg.branches;

  return qos::capacity::start(branches->to_paths(),seconds,allow_write);
}

std::string
QoSGovern::to_string(void) const
{
  return qos::govern::status();
}

int
QoSGovern::from_string(const std::string_view s_)
{
  std::string arg(s_);
  char       *end = nullptr;

  const unsigned long n = ::strtoul(arg.c_str(),&end,10);

  if((end == arg.c_str()) || (*end != '\0'))
    return -EINVAL;

  qos::govern::set_interval(static_cast<u64>(n));

  return 0;
}

std::string
QoSGPU::to_string(void) const
{
  return qos::gpu::status();
}

int
QoSGPU::from_string(const std::string_view s_)
{
  std::string arg(s_);
  double      floor = 0.5;

  const auto comma = arg.find(',');
  if(comma != std::string::npos)
    {
      const std::string f = arg.substr(comma + 1);

      char *end = nullptr;
      floor = ::strtod(f.c_str(),&end);

      if((end == f.c_str()) || (*end != '\0') ||
         (floor < 0.0) || (floor > 1.0))
        return -EINVAL;

      arg.erase(comma);
    }

  char *end = nullptr;
  const unsigned long n = ::strtoul(arg.c_str(),&end,10);

  if((end == arg.c_str()) || (*end != '\0') || (n > 100))
    return -EINVAL;

  // Asking for the signal on a build or a host that cannot provide it
  // should say so rather than silently accept a setting that will
  // never do anything. Turning it off always succeeds.
  if((n != 0) && !qos::gpu::supported())
    return -EOPNOTSUPP;

  qos::gpu::set_threshold(static_cast<u32>(n),floor);

  return 0;
}

std::string
QoSMover::to_string(void) const
{
  return qos::mover::status();
}

int
QoSMover::from_string(const std::string_view s_)
{
  return qos::mover::configure(s_);
}

std::string
QoSStatsJSON::to_string(void) const
{
  return qos::stats_json();
}

int
QoSStatsJSON::from_string(const std::string_view)
{
  return -EINVAL;
}

#else

/*
  Built without per-client QoS.

  The keys stay registered so that `mergerfs -o ...` and the runtime
  interface answer consistently across builds -- a caller gets a clear
  "unsupported" rather than an unknown-key error that looks like a
  typo. Reading any of them says so; assigning anything fails.
 */

namespace
{
  const char *const UNSUPPORTED = "unsupported (built without USE_QOS)\n";
}

QoS::QoS(const bool)
{
}

std::string QoS::to_string(void) const { return "false"; }
int QoS::from_string(const std::string_view) { return -EOPNOTSUPP; }

std::string QoSRules::to_string(void) const { return {}; }
int QoSRules::from_string(const std::string_view) { return -EOPNOTSUPP; }

QoSRuleSet::QoSRuleSet()
{
}

std::string QoSRuleSet::to_string(void) const { return UNSUPPORTED; }
int QoSRuleSet::from_string(const std::string_view) { return -EOPNOTSUPP; }

std::string QoSStats::to_string(void) const { return UNSUPPORTED; }
int QoSStats::from_string(const std::string_view) { return -EOPNOTSUPP; }

std::string QoSMaxSleepers::to_string(void) const { return "0"; }
int QoSMaxSleepers::from_string(const std::string_view) { return -EOPNOTSUPP; }

std::string QoSDistressMS::to_string(void) const { return "0"; }
int QoSDistressMS::from_string(const std::string_view) { return -EOPNOTSUPP; }

std::string QoSDistressFactor::to_string(void) const { return "0"; }
int QoSDistressFactor::from_string(const std::string_view) { return -EOPNOTSUPP; }

std::string QoSMaxSleepMS::to_string(void) const { return "0"; }
int QoSMaxSleepMS::from_string(const std::string_view) { return -EOPNOTSUPP; }

std::string QoSCalibrate::to_string(void) const { return UNSUPPORTED; }
int QoSCalibrate::from_string(const std::string_view) { return -EOPNOTSUPP; }

std::string QoSGovern::to_string(void) const { return UNSUPPORTED; }
int QoSGovern::from_string(const std::string_view) { return -EOPNOTSUPP; }

std::string QoSGPU::to_string(void) const { return UNSUPPORTED; }
int QoSGPU::from_string(const std::string_view) { return -EOPNOTSUPP; }

std::string QoSMover::to_string(void) const { return UNSUPPORTED; }
int QoSMover::from_string(const std::string_view) { return -EOPNOTSUPP; }

std::string QoSStatsJSON::to_string(void) const { return qos::stats_json(); }
int QoSStatsJSON::from_string(const std::string_view) { return -EINVAL; }

#endif
