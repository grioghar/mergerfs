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

/*
  Symlink-proof path resolution beneath a directory.

  A path-based open of `<branch>/<relpath>` follows every symlink in
  every component. For a daemon running as root against a branch that
  unprivileged users can write to through the pool, that is an
  arbitrary-file primitive: between a scan that saw a regular file and
  the operation on it, a directory in the chain can be swapped for a
  symlink to anywhere. O_NOFOLLOW guards only the final component.

  This walks the relative path one component at a time from a root
  directory fd, refusing to follow a symlink at any step and refusing
  to leave the root through `..`. What comes back is an fd for the
  final component's parent, so the caller can openat/unlinkat/renameat
  the leaf by name with the chain already pinned.
 */

#pragma once

#include <string>


namespace fs
{
  // Returns an O_PATH directory fd for the parent of the last component
  // of `relpath`, or a negative errno. `base` receives the last
  // component. The caller closes the fd.
  //
  // Rejects empty components, `.` and `..` outright: a relative path
  // produced by walking a tree never contains them, so one that does
  // is not something this should be asked to resolve.
  int open_parent_beneath(const int          rootfd,
                          const std::string &relpath,
                          std::string       *base);

  // Same walk, but creates each missing directory of `relpath` beneath
  // `dstrootfd`, cloning ownership, mode and timestamps from the
  // corresponding directory beneath `srcrootfd`. Returns an O_PATH fd
  // for the deepest directory, or a negative errno -- including -ELOOP
  // if any existing component turns out not to be a real directory.
  int mkdir_parent_beneath(const int          srcrootfd,
                           const int          dstrootfd,
                           const std::string &relpath,
                           std::string       *base);
}
