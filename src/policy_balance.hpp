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

#include "policy.hpp"

namespace Policy
{
  namespace Balance
  {
    // Create-only. Concentrates new files on branches that have fallen
    // behind, and falls back to `mfs` once the pool is level again -- see
    // balance.hpp. Action and Search have no business steering anything, so
    // they are not provided; `category.create=balance` is the only use.
    class Create final : public Policy::CreateImpl
    {
    public:
      Create()
        : Policy::CreateImpl("balance")
      {}

    public:
      int operator()(const Branches::Ptr&,
                     const fs::path&,
                     std::vector<Branch*>&) const final;
      bool path_preserving() const final { return false; }
    };
  }
}
