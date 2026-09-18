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

#include "fuse_setxattr.hpp"

#include "procfs.hpp"

#include "config.hpp"
#include "errno.hpp"
#include "fs_glob.hpp"
#include "fs_lsetxattr.hpp"
#include "fs_path.hpp"
#include "fs_statvfs_cache.hpp"
#include "num.hpp"
#include "policy_rv.hpp"
#include "str.hpp"
#include "syslog.hpp"

#include "fuse.h"

#include <cstring>
#include <string>
#include <vector>

static const char SECURITY_CAPABILITY[] = "security.capability";


static
int
_setxattr_cmd_xattr(const std::string_view &attrname_,
                    const std::string_view &attrval_)
{
  std::string_view cmd;

  cmd = Config::prune_cmd_xattr(attrname_);

  SysLog::info("command requested: {}={}",cmd,attrval_);

  if(cmd == "gc")
    return (fuse_gc(),0);
  if(cmd == "gc1")
    return (fuse_gc1(),0);
  if(cmd == "invalidate-all-nodes")
    return (fuse_invalidate_all_nodes(),0);

  return -ENOATTR;
}

static
bool
_is_attrname_security_capability(const char *attrname_)
{
  return str::eq(attrname_,SECURITY_CAPABILITY);
}

// The control file is synthesised as mode 0664 owned by the daemon's
// uid, and it is the kernel's permission check on that mode -- active
// only with kernel-permissions-check=true -- that normally keeps other
// users out. The qos.* keys are different in kind from the rest of the
// configuration: assigning one can renice arbitrary processes, write
// probe files onto every branch, or set the mover relocating data, all
// with the daemon's privilege. Those are gated here explicitly so that
// disabling the kernel check for some unrelated reason does not quietly
// hand them to every local user.
//
// Recent kernels issue FUSE_SETXATTR without credentials in the request
// header -- uid and gid arrive as (uint32_t)-1 -- even though the
// FUSE_CREATE from the same shell carries them (observed on 7.0). The
// pid is still filled in, so when the header says nothing the caller is
// looked up through /proc instead. Failing that, the write is refused:
// an unidentifiable caller is not a privileged one.
static
bool
_privileged_key(const std::string    &key_,
                const fuse_req_ctx_t *ctx_)
{
  static const uid_t self = ::getuid();

  if(key_.rfind("qos",0) != 0)
    return true;

  uid_t caller = ctx_->uid;

  if(caller == static_cast<uid_t>(-1))
    caller = procfs::get_fsuid(ctx_->pid);

  if(caller == static_cast<uid_t>(-1))
    return false;

  return ((caller == 0) || (caller == self));
}

static
int
_setxattr_ctrl_file(const fuse_req_ctx_t *ctx_,
                    const char           *attrname_,
                    const char           *attrval_,
                    size_t                attrvalsize_,
                    const int             flags_)
{
  int rv;
  std::string key;

  if(!Config::is_mergerfs_xattr(attrname_))
    return -ENOATTR;

  if(Config::is_cmd_xattr(attrname_))
    return ::_setxattr_cmd_xattr(attrname_,
                                 std::string_view{attrval_,attrvalsize_});

  key = Config::prune_ctrl_xattr(attrname_);

  if(cfg.has_key(key) == false)
    return -ENOATTR;

  if(!::_privileged_key(key,ctx_))
    return -EPERM;

  if((flags_ & XATTR_CREATE) == XATTR_CREATE)
    return -EEXIST;

  rv = cfg.set(key,std::string{attrval_,attrvalsize_});
  if(rv < 0)
    return rv;

  fs::statvfs_cache_timeout(cfg.cache_statfs);

  return rv;
}

static
void
_setxattr_loop_core(const fs::path &basepath_,
                    const fs::path &fusepath_,
                    const char     *attrname_,
                    const char     *attrval_,
                    const size_t    attrvalsize_,
                    const int       flags_,
                    PolicyRV       *prv_)
{
  int rv;
  fs::path fullpath;

  fullpath = basepath_ / fusepath_;

  rv = fs::lsetxattr(fullpath,attrname_,attrval_,attrvalsize_,flags_);

  prv_->insert(rv,basepath_);
}

static
void
_setxattr_loop(const std::vector<Branch*> &branches_,
               const fs::path             &fusepath_,
               const char                 *attrname_,
               const char                 *attrval_,
               const size_t                attrvalsize_,
               const int                   flags_,
               PolicyRV                   *prv_)
{
  for(auto &branch : branches_)
    {
      ::_setxattr_loop_core(branch->path,
                            fusepath_,
                            attrname_,
                            attrval_,attrvalsize_,
                            flags_,
                            prv_);
    }
}

static
int
_setxattr(const Policy::Action &setxattrPolicy_,
          const Policy::Search &getxattrPolicy_,
          const Branches::Ptr   branches_,
          const fs::path       &fusepath_,
          const char           *attrname_,
          const char           *attrval_,
          const size_t          attrvalsize_,
          const int             flags_)
{
  int rv;
  PolicyRV prv;
  std::vector<Branch*> branches;

  rv = setxattrPolicy_(branches_,fusepath_,branches);
  if(rv < 0)
    return rv;
  if(branches.empty())
    return -ENOENT;

  ::_setxattr_loop(branches,fusepath_,attrname_,attrval_,attrvalsize_,flags_,&prv);
  if(prv.errors.empty())
    return 0;
  if(prv.successes.empty())
    return prv.errors[0].rv;

  branches.clear();
  rv = getxattrPolicy_(branches_,fusepath_,branches);
  if(rv < 0)
    return rv;
  if(branches.empty())
    return -ENOENT;

  return prv.get_error(branches[0]->path);
}

static
int
_setxattr(const fuse_req_ctx_t *ctx_,
          const fs::path       &fusepath_,
          const char           *attrname_,
          const char           *attrval_,
          size_t                attrvalsize_,
          int                   flags_)
{
  if((cfg.security_capability == false) &&
     ::_is_attrname_security_capability(attrname_))
    return -ENOATTR;

  if(cfg.xattr.to_int())
    return -cfg.xattr.to_int();

  return ::_setxattr(cfg.func.setxattr.policy,
                     cfg.func.getxattr.policy,
                     cfg.branches,
                     fusepath_,
                     attrname_,
                     attrval_,
                     attrvalsize_,
                     flags_);
}


int
FUSE::setxattr(const fuse_req_ctx_t *ctx_,
               const char           *fusepath_,
               const char           *attrname_,
               const char           *attrval_,
               size_t                attrvalsize_,
               int                   flags_)
{
  const fs::path fusepath{fusepath_};

  if(Config::is_ctrl_file(fusepath))
    return ::_setxattr_ctrl_file(ctx_,attrname_,
                                 attrval_,
                                 attrvalsize_,
                                 flags_);

  return ::_setxattr(ctx_,
                     fusepath,
                     attrname_,
                     attrval_,
                     attrvalsize_,
                     flags_);
}
