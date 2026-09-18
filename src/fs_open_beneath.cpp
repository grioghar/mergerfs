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

#include "fs_open_beneath.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>


namespace
{
  // Splits into components, rejecting anything that could escape or
  // that a tree walk would never produce.
  int
  _split(const std::string        &relpath_,
         std::vector<std::string> *out_)
  {
    std::size_t start = 0;

    while(start <= relpath_.size())
      {
        const std::size_t slash = relpath_.find('/',start);
        const std::string comp  = relpath_.substr(start,
                                                  ((slash == std::string::npos)
                                                   ? std::string::npos
                                                   : (slash - start)));

        if(comp.empty() || (comp == ".") || (comp == ".."))
          return -EINVAL;

        out_->push_back(comp);

        if(slash == std::string::npos)
          break;

        start = (slash + 1);
      }

    return (out_->empty() ? -EINVAL : 0);
  }

  int
  _step(const int          curfd_,
        const std::string &comp_)
  {
    const int fd = ::openat(curfd_,comp_.c_str(),
                            O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);

    // A symlink where a directory was expected is the case this whole
    // file exists for. Report it as such rather than as a generic
    // failure so a caller can distinguish "gone" from "tampered".
    if((fd < 0) && (errno == ENOTDIR))
      {
        struct stat st;

        if((::fstatat(curfd_,comp_.c_str(),&st,AT_SYMLINK_NOFOLLOW) == 0) &&
           S_ISLNK(st.st_mode))
          return -ELOOP;
      }

    return ((fd < 0) ? -errno : fd);
  }
}

int
fs::open_parent_beneath(const int          rootfd_,
                        const std::string &relpath_,
                        std::string       *base_)
{
  std::vector<std::string> comps;

  int rv = ::_split(relpath_,&comps);
  if(rv < 0)
    return rv;

  int cur = ::dup(rootfd_);
  if(cur < 0)
    return -errno;

  for(std::size_t i = 0; (i + 1) < comps.size(); i++)
    {
      const int next = ::_step(cur,comps[i]);

      ::close(cur);

      if(next < 0)
        return next;

      cur = next;
    }

  *base_ = comps.back();

  return cur;
}

int
fs::mkdir_parent_beneath(const int          srcrootfd_,
                         const int          dstrootfd_,
                         const std::string &relpath_,
                         std::string       *base_)
{
  std::vector<std::string> comps;

  int rv = ::_split(relpath_,&comps);
  if(rv < 0)
    return rv;

  int src = ::dup(srcrootfd_);
  int dst = ::dup(dstrootfd_);

  if((src < 0) || (dst < 0))
    {
      const int err = errno;
      if(src >= 0) ::close(src);
      if(dst >= 0) ::close(dst);
      return -err;
    }

  for(std::size_t i = 0; (i + 1) < comps.size(); i++)
    {
      const std::string &comp = comps[i];

      // The source side is only ever read, and pinned the same way.
      const int nsrc = ::_step(src,comp);
      ::close(src);
      if(nsrc < 0)
        {
          ::close(dst);
          return nsrc;
        }
      src = nsrc;

      // Created restrictively; the real mode is applied after the
      // directory is known to be ours and a real directory.
      const bool created = (::mkdirat(dst,comp.c_str(),0700) == 0);
      if(!created && (errno != EEXIST))
        {
          const int err = errno;
          ::close(src);
          ::close(dst);
          return -err;
        }

      const int ndst = ::_step(dst,comp);
      ::close(dst);
      if(ndst < 0)
        {
          ::close(src);
          return ndst;
        }
      dst = ndst;

      if(created)
        {
          struct stat st;

          // O_PATH fds cannot be fchmod'ed, so reopen for the metadata
          // copy. Still O_NOFOLLOW; still pinned to what mkdirat made.
          const int mdst = ::openat(dst,".",O_RDONLY | O_DIRECTORY | O_CLOEXEC);

          if((mdst >= 0) && (::fstat(src,&st) == 0))
            {
              ::fchown(mdst,st.st_uid,st.st_gid);
              ::fchmod(mdst,(st.st_mode & 07777));

              struct timespec ts[2] = {st.st_atim,st.st_mtim};
              ::futimens(mdst,ts);
            }

          if(mdst >= 0)
            ::close(mdst);
        }
    }

  ::close(src);

  *base_ = comps.back();

  return dst;
}
