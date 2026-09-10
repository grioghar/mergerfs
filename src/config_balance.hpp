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

// balance=<bool>
class BalanceEnabled : public ToFromString
{
public:
  BalanceEnabled(const bool);
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// balance.gap=<percent>       how far behind the fullest branch counts as behind
// balance.count=<n>           how many branches must be behind to act
// balance.min-free=<size>     never steer onto a branch below this
class BalanceGap : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

class BalanceCount : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

class BalanceMinFree : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// balance.status -- read only
class BalanceStatus : public ToFromString
{
public:
  BalanceStatus();
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// health=off|monitor|quarantine
class HealthMode : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

class HealthInterval : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

class HealthMaxTemp : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

class HealthMaxPending : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

class HealthMaxRealloc : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};

// health.status -- read only; assigning "poll" forces an immediate check
class HealthStatus : public ToFromString
{
public:
  std::string to_string(void) const final;
  int from_string(const std::string_view) final;
};
