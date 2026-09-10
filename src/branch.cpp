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

#include "branch.hpp"

#include <fnmatch.h>
#include "num.hpp"

Branch::Branch()
  : mode(Branch::Mode::RW)
{
}

Branch::Branch(const Branch &branch_)
  : _minfreespace(branch_._minfreespace),
    mode(branch_.mode),
    path(branch_.path),
    accept(branch_.accept),
    reject(branch_.reject)
{
}

Branch::Branch(const u64 &default_minfreespace_)
  : _minfreespace(&default_minfreespace_)
{
}

std::string
Branch::to_string(void) const
{
  std::string rv;

  rv  = path;
  rv += '=';
  switch(mode)
    {
    default:
    case Branch::Mode::RW:
      rv += "RW";
      break;
    case Branch::Mode::RO:
      rv += "RO";
      break;
    case Branch::Mode::NC:
      rv += "NC";
      break;
    }

  if(std::holds_alternative<u64>(_minfreespace))
    {
      rv += ',';
      rv += num::humanize(std::get<u64>(_minfreespace));
    }

  // Emitted as key=value so the string round-trips through from_string.
  if(!accept.empty())
    {
      rv += ",accept=";
      for(std::size_t i = 0; i < accept.size(); i++)
        { if(i) rv += '|'; rv += accept[i]; }
    }

  if(!reject.empty())
    {
      rv += ",reject=";
      for(std::size_t i = 0; i < reject.size(); i++)
        { if(i) rv += '|'; rv += reject[i]; }
    }

  return rv;
}

void
Branch::set_minfreespace(const u64 minfreespace_)
{
  _minfreespace = minfreespace_;
}

u64
Branch::minfreespace(void) const
{
  if(std::holds_alternative<const u64*>(_minfreespace))
    return *std::get<const u64*>(_minfreespace);
  return std::get<u64>(_minfreespace);
}

bool
Branch::ro(void) const
{
  return (mode == Branch::Mode::RO);
}

bool
Branch::nc(void) const
{
  return (mode == Branch::Mode::NC);
}

bool
Branch::ro_or_nc(void) const
{
  return ((mode == Branch::Mode::RO) ||
          (mode == Branch::Mode::NC));
}

bool
Branch::accepts(const std::string &fusepath_) const
{
  // FUSE hands paths to mergerfs without a leading '/' (branch.path /
  // fusepath relies on the right-hand side being relative). Patterns read far
  // more naturally anchored at the pool root -- "/downloads/*" rather than
  // "downloads/*" -- so normalise the subject instead of the pattern.
  std::string subject;

  if(fusepath_.empty() || (fusepath_[0] != '/'))
    subject = "/" + fusepath_;
  else
    subject = fusepath_;

  // reject wins over accept: it is the safety net, and an admin writing both
  // means "these, except those".
  for(const auto &pat : reject)
    {
      if(::fnmatch(pat.c_str(),subject.c_str(),0) == 0)
        return false;
    }

  if(accept.empty())
    return true;

  for(const auto &pat : accept)
    {
      if(::fnmatch(pat.c_str(),subject.c_str(),0) == 0)
        return true;
    }

  return false;
}
