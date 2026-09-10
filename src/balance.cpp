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

#include "balance.hpp"

#include "fs_info.hpp"

#include "fmt/core.h"

#include <algorithm>
#include <vector>


namespace balance
{
  std::atomic<bool> _enabled{false};
  std::atomic<u64>  gap{10};
  std::atomic<u64>  count{1};
  std::atomic<u64>  min_free{0};
}

namespace
{
  struct Candidate
  {
    Branch *branch;
    u64     used_pct;
    u64     spaceavail;
  };

  // Snapshot every writable branch that currently answers statvfs.
  //
  // A branch that cannot be stat'ed is skipped rather than treated as empty:
  // a disk that has dropped off the bus reports nothing, and calling that
  // "0% used" would aim every write straight at it.
  std::vector<Candidate>
  _survey(const Branches::Ptr &branches_)
  {
    std::vector<Candidate> out;

    for(auto &branch : *branches_)
      {
        fs::info_t info;

        if(branch.ro_or_nc())
          continue;
        if(fs::info(branch.path,&info) < 0)
          continue;
        if(info.readonly)
          continue;
        // info_t reports avail and used, not total; derive the denominator.
        const u64 total = (info.spaceavail + info.spaceused);
        if(total == 0)
          continue;

        out.push_back({&branch,
                       ((info.spaceused * 100) / total),
                       info.spaceavail});
      }

    return out;
  }
}

void
balance::enable(const bool b_)
{
  _enabled.store(b_,std::memory_order_relaxed);
}

Branch *
balance::target(const Branches::Ptr &branches_)
{
  if(!balance::enabled())
    return nullptr;

  auto cands = ::_survey(branches_);
  if(cands.size() < 2)
    return nullptr;

  const u64 fullest = std::max_element(cands.begin(),cands.end(),
                                       [](const Candidate &a, const Candidate &b)
                                       { return a.used_pct < b.used_pct; })->used_pct;

  const u64 g  = balance::gap.load(std::memory_order_relaxed);
  const u64 mf = balance::min_free.load(std::memory_order_relaxed);

  // "Behind" is measured against the fullest branch, not the mean: the point
  // is to stop writing to whichever disk is closest to full, and the mean
  // moves as soon as concentration starts working.
  std::vector<const Candidate*> behind;
  for(const auto &c : cands)
    {
      if((fullest - c.used_pct) < g)
        continue;
      if(mf && (c.spaceavail < mf))
        continue;
      behind.push_back(&c);
    }

  if(behind.size() < balance::count.load(std::memory_order_relaxed))
    return nullptr;

  // Emptiest of the laggards, so the gap closes fastest.
  const Candidate *best = *std::min_element(behind.begin(),behind.end(),
                                            [](const Candidate *a, const Candidate *b)
                                            { return a->used_pct < b->used_pct; });

  return best->branch;
}

std::string
balance::status(const Branches::Ptr &branches_)
{
  std::string s;
  auto cands = ::_survey(branches_);

  s += fmt::format("enabled={} gap={}% count={} min-free={}\n",
                   (balance::enabled() ? "true" : "false"),
                   balance::gap.load(std::memory_order_relaxed),
                   balance::count.load(std::memory_order_relaxed),
                   balance::min_free.load(std::memory_order_relaxed));

  if(cands.empty())
    {
      s += "no writable branches\n";
      return s;
    }

  const u64 fullest = std::max_element(cands.begin(),cands.end(),
                                       [](const Candidate &a, const Candidate &b)
                                       { return a.used_pct < b.used_pct; })->used_pct;
  const u64 g = balance::gap.load(std::memory_order_relaxed);

  for(const auto &c : cands)
    {
      const u64 lag = (fullest - c.used_pct);
      s += fmt::format("{}: used={}% behind-fullest={}%{}\n",
                       c.branch->path.string(),
                       c.used_pct,
                       lag,
                       ((lag >= g) ? " BEHIND" : ""));
    }

  Branch *t = balance::target(branches_);
  if(t)
    s += fmt::format("target={} (concentrating writes)\n",t->path.string());
  else
    s += "target=none (pool level enough; normal create policy in use)\n";

  return s;
}
