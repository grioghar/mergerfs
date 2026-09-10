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

#include "branches.hpp"

#include <algorithm>
#include <memory>
#include "strvec.hpp"
#include "fs_path.hpp"

#include <string>
#include <memory>
#include <vector>


namespace Policy
{
  class ActionImpl
  {
  public:
    ActionImpl(const std::string &name_)
      : name(name_)
    {
    }

  public:
    std::string name;
    virtual int operator()(const Branches::Ptr&,
                           const fs::path&,
                           std::vector<Branch*>&) const = 0;
  };

  class Action
  {
  public:
    Action(ActionImpl *impl_)
      : impl(impl_)
    {}

    Action&
    operator=(ActionImpl *impl_)
    {
      impl = impl_;
      return *this;
    }

    const
    std::string&
    name(void) const
    {
      return impl->name;
    }

    int
    operator()(const Branches::Ptr  &branches_,
               const fs::path       &fusepath_,
               std::vector<Branch*> &output_) const
    {
      return (*impl)(branches_,fusepath_,output_);
    }

    operator bool() const
    {
      return (bool)impl;
    }

  private:
    ActionImpl *impl;
  };

  class CreateImpl
  {
  public:
    CreateImpl(const std::string &name_)
      : name(name_)
    {
    }

  public:
    std::string name;
    virtual bool path_preserving(void) const = 0;
    virtual int operator()(const Branches::Ptr&,
                           const fs::path&,
                           std::vector<Branch*>&) const = 0;
  };

  class Create
  {
  public:
    Create(CreateImpl *impl_)
      : impl(impl_)
    {}

    Create&
    operator=(CreateImpl *impl_)
    {
      impl = impl_;
      return *this;
    }

    const
    std::string&
    name(void) const
    {
      return impl->name;
    }

    bool
    path_preserving(void) const
    {
      return impl->path_preserving();
    }

    int
    operator()(const Branches::Ptr  &branches_,
               const fs::path       &fusepath_,
               std::vector<Branch*> &output_) const
    {
      return (*this)(branches_,fusepath_,fusepath_,output_);
    }

    // Directory creation is deliberately NOT pattern-filtered. Patterns say
    // which files a branch will hold; the directory tree has to be allowed to
    // exist on every branch or a path-preserving policy could never place a
    // file on a filtered branch at all.
    int
    create_dir(const Branches::Ptr  &branches_,
               const fs::path       &fusepath_,
               std::vector<Branch*> &output_) const
    {
      return (*impl)(branches_,fusepath_,output_);
    }

    // searchpath_ is what the policy itself examines. For create/mknod/symlink
    // that is the PARENT DIRECTORY, because the file does not exist yet.
    // filterpath_ is the full path of the thing being created, and it is what
    // accept/reject match against -- matching patterns against the parent
    // directory would make every "*.mkv" rule match nothing.
    int
    operator()(const Branches::Ptr  &branches_,
               const fs::path       &searchpath_,
               const fs::path       &filterpath_,
               std::vector<Branch*> &output_) const
    {
      // Per-branch accept/reject patterns are applied here, once, rather than
      // in each of the ~18 create policies. Filtering the branch list before
      // the policy runs means every policy honours them automatically and
      // none of them needed changing.
      //
      // Only Create is filtered. Patterns govern PLACEMENT of new files; Action
      // and Search operate on files that already exist, and filtering those
      // would make data already living on a branch unreachable.
      //
      // The scan is skipped entirely unless some branch actually declares a
      // filter, so a pool without patterns pays one bool per branch.
      bool filtered = false;
      for(const auto &b : *branches_)
        {
          if(b.has_filters()) { filtered = true; break; }
        }

      if(!filtered)
        return (*impl)(branches_,searchpath_,output_);

      // Branches::Impl is non-copyable, so build the subset by construction
      // rather than copy-and-erase. Branch itself is copyable.
      const std::string path = filterpath_.string();
      auto subset = std::make_shared<Branches::Impl>(&branches_->minfreespace());
      for(const auto &b : *branches_)
        {
          if(b.accepts(path))
            subset->push_back(b);
        }

      // Every branch rejected this path: report it as "no space here" rather
      // than inventing a placement that violates the admin's rules.
      if(subset->empty())
        return -ENOSPC;

      std::vector<Branch*> selected;

      int rv = (*impl)(subset,searchpath_,selected);
      if(rv < 0)
        return rv;

      // `selected` points into `subset`, which is destroyed when this function
      // returns. Callers keep using the returned Branch* well past that, so
      // translate each pick back to the canonical Branch owned by branches_.
      // Branch paths are unique within a pool, so path is a safe key.
      for(const auto *sel : selected)
        {
          for(auto &b : *branches_)
            {
              if(b.path == sel->path)
                {
                  output_.push_back(&b);
                  break;
                }
            }
        }

      return 0;
    }

    operator bool() const
    {
      return (bool)impl;
    }

  private:
    CreateImpl *impl;
  };

  class SearchImpl
  {
  public:
    SearchImpl(const std::string &name_)
      : name(name_)
    {
    }

  public:
    std::string name;
    virtual int operator()(const Branches::Ptr&,
                           const fs::path&,
                           std::vector<Branch*>&) const = 0;
  };

  class Search
  {
  public:
    Search(SearchImpl *impl_)
      : impl(impl_)
    {}

    Search&
    operator=(SearchImpl *impl_)
    {
      impl = impl_;
      return *this;
    }

    const
    std::string&
    name(void) const
    {
      return impl->name;
    }

    int
    operator()(const Branches::Ptr  &branches_,
               const fs::path       &fusepath_,
               std::vector<Branch*> &output_) const
    {
      return (*impl)(branches_,fusepath_,output_);
    }

    operator bool() const
    {
      return (bool)impl;
    }

  private:
    SearchImpl *impl;
  };
}
