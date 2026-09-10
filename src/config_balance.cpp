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

#include "config_balance.hpp"

#include "balance.hpp"
#include "config.hpp"
#include "from_string.hpp"
#include "health.hpp"

#include <errno.h>

// ---------------------------------------------------------------- balance ---

BalanceEnabled::BalanceEnabled(const bool b_)
{
  balance::enable(b_);
}

std::string
BalanceEnabled::to_string(void) const
{
  return (balance::enabled() ? "true" : "false");
}

int
BalanceEnabled::from_string(const std::string_view s_)
{
  int rv; bool b;
  rv = str::from(s_,&b);
  if(rv) return rv;
  balance::enable(b);
  return 0;
}

std::string
BalanceGap::to_string(void) const
{
  return std::to_string(balance::gap.load(std::memory_order_relaxed));
}

int
BalanceGap::from_string(const std::string_view s_)
{
  int rv; u64 v;
  rv = str::from(s_,&v);
  if(rv) return rv;
  if(v > 100) return -EINVAL;
  balance::gap.store(v,std::memory_order_relaxed);
  return 0;
}

std::string
BalanceCount::to_string(void) const
{
  return std::to_string(balance::count.load(std::memory_order_relaxed));
}

int
BalanceCount::from_string(const std::string_view s_)
{
  int rv; u64 v;
  rv = str::from(s_,&v);
  if(rv) return rv;
  if(v == 0) return -EINVAL;
  balance::count.store(v,std::memory_order_relaxed);
  return 0;
}

std::string
BalanceMinFree::to_string(void) const
{
  return std::to_string(balance::min_free.load(std::memory_order_relaxed));
}

int
BalanceMinFree::from_string(const std::string_view s_)
{
  int rv; u64 v;
  rv = str::from(s_,&v);   // handles size suffixes, as minfreespace does
  if(rv) return rv;
  balance::min_free.store(v,std::memory_order_relaxed);
  return 0;
}

BalanceStatus::BalanceStatus()
{
  ro = true;
}

std::string
BalanceStatus::to_string(void) const
{
  return balance::status(cfg.branches);
}

int
BalanceStatus::from_string(const std::string_view)
{
  return -EINVAL;
}

// ----------------------------------------------------------------- health ---

std::string
HealthMode::to_string(void) const
{
  switch(health::mode())
    {
    case health::Mode::MONITOR:    return "monitor";
    case health::Mode::QUARANTINE: return "quarantine";
    default:                       return "off";
    }
}

int
HealthMode::from_string(const std::string_view s_)
{
  if(s_ == "off")             health::set_mode(health::Mode::OFF);
  else if(s_ == "monitor")    health::set_mode(health::Mode::MONITOR);
  else if(s_ == "quarantine") health::set_mode(health::Mode::QUARANTINE);
  else return -EINVAL;
  return 0;
}

std::string
HealthInterval::to_string(void) const
{
  return std::to_string(health::interval.load(std::memory_order_relaxed));
}

int
HealthInterval::from_string(const std::string_view s_)
{
  int rv; u64 v;
  rv = str::from(s_,&v);
  if(rv) return rv;
  if(v < 60) return -EINVAL;   // polling SMART faster than this is pointless
  health::interval.store(v,std::memory_order_relaxed);
  return 0;
}

std::string
HealthMaxTemp::to_string(void) const
{
  return std::to_string(health::max_temp.load(std::memory_order_relaxed));
}

int
HealthMaxTemp::from_string(const std::string_view s_)
{
  int rv; u64 v;
  rv = str::from(s_,&v);
  if(rv) return rv;
  health::max_temp.store(v,std::memory_order_relaxed);
  return 0;
}

std::string
HealthMaxPending::to_string(void) const
{
  return std::to_string(health::max_pending.load(std::memory_order_relaxed));
}

int
HealthMaxPending::from_string(const std::string_view s_)
{
  int rv; u64 v;
  rv = str::from(s_,&v);
  if(rv) return rv;
  health::max_pending.store(v,std::memory_order_relaxed);
  return 0;
}

std::string
HealthMaxRealloc::to_string(void) const
{
  return std::to_string(health::max_realloc.load(std::memory_order_relaxed));
}

int
HealthMaxRealloc::from_string(const std::string_view s_)
{
  int rv; u64 v;
  rv = str::from(s_,&v);
  if(rv) return rv;
  health::max_realloc.store(v,std::memory_order_relaxed);
  return 0;
}

std::string
HealthStatus::to_string(void) const
{
  return health::status();
}

int
HealthStatus::from_string(const std::string_view s_)
{
  if(s_ != "poll")
    return -EINVAL;
  health::poll_now();
  return 0;
}
