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

#include "policy_balance.hpp"

#include "balance.hpp"
#include "errno.hpp"
#include "fs_info.hpp"
#include "policies.hpp"

int
Policy::Balance::Create::operator()(const Branches::Ptr  &branches_,
                                    const fs::path       &fusepath_,
                                    std::vector<Branch*> &paths_) const
{
  Branch *target;

  target = balance::target(branches_);

  // Pool is level (or balancing is off): behave exactly like mfs. Falling
  // back rather than erroring means `category.create=balance` is always a
  // safe setting, including before any imbalance exists.
  if(target == nullptr)
    return Policies::Create::mfs(branches_,fusepath_,paths_);

  // The survey in balance::target() already rejected branches that are
  // read-only, unreadable or below minfreespace, so the chosen branch is
  // known good at this point.
  paths_.emplace_back(target);

  return 0;
}
