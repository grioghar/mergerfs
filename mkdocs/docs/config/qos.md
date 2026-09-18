# qos

* type: `BOOL`
* default: `false`
* example: `qos=true`

Per-client quality of service: attribute each read and write to the
process that asked for it, match it against a ruleset, and give the
worker thread that class's I/O priority, nice value and bandwidth
allowance for the duration of the request.


## Why this exists

mergerfs is one daemon serving every process that touches the pool. To
the kernel, all pool I/O is issued by one set of threads in one cgroup,
so block layer priority cannot tell a media player apart from a torrent
client -- `ionice`, systemd's `IOSchedulingClass=` and cgroup
`io.weight` are all applied to the *daemon*, not to whoever is behind
the request. Priority is flattened.

This restores the distinction inside the daemon, where the caller is
still known.

[proxy-ioprio](proxy-ioprio.md) addresses the same problem from the
other end: it copies whatever ioprio the caller happens to have. That
is simpler, but it only works if every client is correctly `ionice`d by
whoever started it -- which is awkward for programs that respawn
workers under a single service, and impossible for clients inside a
container the daemon does not control. With `qos` the policy lives in
one file and is written in terms of *who is asking* and *what they are
touching*.

The two are independent and there is no reason to enable both.


## Options

| option | default | meaning |
|---|---|---|
| `qos` | `false` | master switch |
| `qos.rules` | none | path to a rules file; assigning it loads it |
| `qos.ruleset` | - | read only: the ruleset as parsed |
| `qos.stats` | - | counters; assign `reset` to zero them |
| `qos.max-sleepers` | `-1` | threads that may sleep in a throttle at once; `-1` is half the process thread pool |
| `qos.max-sleep-ms` | `50` | longest any one request may be delayed |
| `qos.distress-ms` | `50` | service time below which a protected class is never considered to be suffering |
| `qos.distress-factor` | `3.0` | multiple of a resource's quiet latency that counts as distress |
| `qos.stats.json` | - | read only: all of the above, plus the governor, mover, GPU and calibration state, as one JSON document |
| `qos.calibrate` | - | assign `<seconds>[,write]` to measure what each branch delivers; read for progress |
| `qos.govern` | `0` | seconds between sweeps applying `govern` classes to client processes; `0` is off |
| `qos.gpu` | `0` | `<percent>[,<floor>]`: hold pressure at `floor` while the GPU is at least this busy; `0` is off |
| `qos.mover` | `policy=off` | background relocation of existing files; see [The mover](#the-mover) |

Every one of these is readable and writable at runtime through the
[runtime interface](../runtime_interface.md):

```sh
getfattr -n user.mergerfs.qos.stats --only-values /mnt/pool/.mergerfs
setfattr -n user.mergerfs.qos.rules -v /etc/mergerfs/qos.rules /mnt/pool/.mergerfs
```

Assigning `qos.rules` is also how a ruleset is reloaded. A file that
fails to parse is rejected with the offending line number logged to
syslog, and the running ruleset is left untouched.


## The rules file

A single mount option pointing at a file, rather than a value crammed
into `/etc/fstab`, because rules contain commas and fstab does not
forgive that.

```
# Everything after # is a comment.

capacity /mnt/disk1 180M          # measured throughput of a resource
capacity default    120M          # fallback for unlisted resources

class <name> [protect] [ioprio=…] [nice=…] [rate=…] [burst=…] [yield=…] [floor=…]

match <field> <op> <pattern> [<field> <op> <pattern> …] -> <class>

default <class>
```

Rules are evaluated in order and the first whose conditions *all* match
wins. A class must be defined before a rule names it.

### Class attributes

| attribute | meaning |
|---|---|
| `ioprio=` | `rt:0`-`rt:7`, `be:0`-`be:7`, `idle`, `none` |
| `nice=` | `-20` to `19` |
| `rate=` | absolute (`8M`) or a share of the resource's capacity (`10%`) |
| `burst=` | bucket depth; defaults to one second of `rate` |
| `protect` | never throttled, and its latency drives the governor |
| `critical` | never throttled, and *not* a control signal |
| `yield=` | `0`-`100`: how hard this class gives way under pressure |
| `floor=` | never back off below this; absolute or a percentage |
| `govern` | also apply this class's values to the client process itself; see [The process governor](#the-process-governor) |
| `govern-ioprio=`, `govern-nice=` | what the governor applies to the *process*, when that should differ from what its pool I/O gets; default to `ioprio` / `nice` |

### Match fields

| field | matched against |
|---|---|
| `cgroup` | `/proc/<pid>/cgroup` -- identifies the container |
| `comm` | `/proc/<pid>/comm` -- identifies the program |
| `cmdline` | `/proc/<pid>/cmdline`, arguments joined by spaces |
| `uid`, `gid` | the caller's credentials |
| `path` | the path within the pool |
| `op` | `read` or `write` |

Operators are `=` for exact match and `~` for an
[fnmatch(3)](https://man7.org/linux/man-pages/man3/fnmatch.3.html)
glob. `FNM_PATHNAME` is not set, so `path ~ /TV/*` covers everything
beneath `/TV`.

`uid`, `gid` and `op` accept only `=`.

Wrap a pattern in single or double quotes to keep spaces in it. This is
not decoration -- the names worth matching include `"Plex Transcoder"`
and `"Plex Media Scanner"`, and a command line is nothing but spaces.


### Why `cmdline` exists

Some programs do very different jobs under one name. Plex runs credits
and intro detection using the *same* `Plex Transcoder` binary that
serves playback, reading the *same* media file. Neither `comm` nor
`path` can tell those apart. The command line can: a detection job
writes into `.../Transcode/Detection/`, a playback transcode feeds a
session.

```
match comm = "Plex Transcoder" cmdline ~ */Transcode/Detection/* -> analysis
match comm = "Plex Transcoder"                                   -> playback
```

Measured on an otherwise idle pool, with `analysis` capped at 8 MiB/s:
the detection job ran at 8.1 MiB/s and the playback job, same binary and
same file, at 730 MiB/s.


### Why `critical` exists

`critical` marks a class that playback *synchronously waits on*, as
opposed to playback itself. Plex's EasyAudioEncoder is the canonical
case: transcoding EAC3, TrueHD or DTS audio runs
`Plex Transcoder -codec:1 eac3_eae`, which hands the audio to EAE and
blocks until it answers.

Such a helper looks like background work -- it is not the stream, it
does not appear in a session -- but slowing it slows the stream stalled
behind it. That is a priority inversion, and it is an easy one to
create by accident: a governor that suspends "background" processes to
protect playback will happily suspend the one service playback is
waiting for, and hang the very thing it was protecting.

A `critical` class is never delayed, whatever else the ruleset says
about it, and its latency is not used as a control signal.

```
class eae critical
match comm ~ EasyAudioEncode* -> eae
match comm = "Plex EAE Service" -> eae
```


## Rates are per resource

A resource is a branch -- in practice, a disk. Each class gets its own
token bucket per branch, so a class limited to 10% gets a tenth of
*each* disk rather than a tenth of the pool split across all of them.
Two players reading two different disks never compete for one
allowance.

Percentages need a capacity to resolve against, and mergerfs works it
out for itself. There are three sources, in order of authority:

1. **A `capacity` line in the rules file.** An explicit statement of
   what a disk is for. Nothing overrules it.
2. **An active probe.** `qos.calibrate` saturates each branch with
   O_DIRECT reads and records a real ceiling.
3. **Passive observation.** Always running and free: the best rate seen
   in a one second window that carried enough requests to mean
   anything. It rises immediately, decays slowly, and does not decay at
   all while the pool is idle -- so an overnight quiet spell cannot
   erase what the pool managed under load.

Observation can only ever see as much as something asked for, so on a
pool that has never been driven hard it reads low. That is what the
probe is for:

```sh
# three seconds per branch, reads only
setfattr -n user.mergerfs.qos.calibrate -v 3 /mnt/pool/.mergerfs

# ...and measure writes too, via a temporary file on each branch
setfattr -n user.mergerfs.qos.calibrate -v 3,write /mnt/pool/.mergerfs

getfattr -n user.mergerfs.qos.calibrate --only-values /mnt/pool/.mergerfs
```

It runs in the background -- the assignment returns immediately -- one
branch at a time, at `nice 19` and idle I/O priority, reading with
O_DIRECT so it neither measures the page cache nor evicts anything the
running services still want. Results are installed per branch as they
are measured.

Because it yields rather than competes, a figure measured while the
pool is busy is a floor rather than a ceiling. Calibrate when it is
quiet.

A percentage with no capacity known for its resource is left unlimited
rather than throttled against a guess.


## The adaptive governor

A fixed cap is a blunt instrument: it slows downloads even when nothing
is playing. Marking a class `protect` turns on a feedback loop instead.

mergerfs times that class's requests per resource and compares the
smoothed figure against the quietest that resource has managed. When it
is worse by `qos.distress-factor` -- and worse than `qos.distress-ms`
in absolute terms -- *and* a yielding class is active on that same
resource, pressure rises. Every yielding class's allowance is then
scaled by `1 - (pressure × yield / 100)`, bounded below by its `floor`.

When the latency recovers, or playback stops, pressure decays back to
zero and the allowances return to whatever they were configured for --
which for downloads is normally no limit at all.

The result: bulk traffic runs flat out on a disk nobody is streaming
from, gives way gradually on one that is, and never stops entirely.

Pressure rises multiplicatively and falls additively. A stutter has
already been heard by the time it is measured, so the loop backs off
hard and returns gently.

`qos.distress-ms` is the knob that matters and it is device dependent.
The `50` default suits spinning disks, where 50ms is a genuine stall.
On an SSD or NVMe pool it will never be reached and the governor will
never engage -- use single-digit milliseconds there. Setting it to `0`
removes noise suppression entirely and is not recommended.


## The process governor

Everything above governs I/O that flows *through* mergerfs. A media
server also reads and writes outside the pool -- its own database, its
metadata store, its transcode scratch directory -- and it burns CPU.
Neither is flattened by FUSE, and both compete with playback.

Marking a class `govern` applies its values to the client process
itself, not only to the worker thread serving its pool I/O:

```
class downloads govern yield=100 floor=5% ioprio=idle nice=19
```

The two are different axes, and a class may set them separately with
`govern-ioprio=` and `govern-nice=`. ffprobe is the canonical case: its
pool reads are a synchronous dependency of starting playback and must
never be throttled, while its CPU is pure library analysis:

```
class probe critical govern govern-ioprio=idle govern-nice=19
match comm ~ ffprobe* -> probe
```

```sh
# sweep every 10 seconds
setfattr -n user.mergerfs.qos.govern -v 10 /mnt/pool/.mergerfs
getfattr -n user.mergerfs.qos.govern --only-values /mnt/pool/.mergerfs
```

This replaces the shell loop calling `renice` and `ionice` that pools
of this kind usually end up with. Doing it here means one ruleset
describes both halves of the problem instead of two that drift apart,
and it avoids the trap that `pgrep` walks into: `comm` is capped at
fifteen characters and frequently does not resemble the command line,
so `pgrep -x` silently matches nothing for a name any longer than that.
The matching here already understands full command lines.

Details that matter:

* **Every thread of a matched process is set**, not just its main
  thread. `nice` and `ioprio` are per-thread values on Linux, and a
  server that does its scanning on a worker thread is the normal case.
* **Steady state costs nothing.** A process is only touched when its
  class changes or it has spawned threads since the last sweep.
* **mergerfs never governs itself.** Its threads are set per request;
  a sweep would fight that every interval.
* `path` and `op` conditions cannot be evaluated outside a request, so
  rules using them never match here and classification falls through
  to the next rule. Do not mark such a class `govern` and expect it to
  work.
* Lowering another process's `nice` needs privilege. Threads that
  could not be set are counted in `failed`.
* The default class is never applied by the sweep, whatever it says:
  applied host-wide it would renice init, sshd and your shell.
* Values are applied, never restored. A process left at `idle` when
  the mount goes away stays there, exactly as it would after a
  `renice` from a shell. Restart it, or let its own supervisor.
* `qos.govern` given as a mount option takes effect once the daemon is
  serving requests (mergerfs daemonises after parsing its options), so
  the first sweep lands a few seconds after mount.


## The GPU signal

On a media server the video engine is busy exactly when hardware
transcodes are running -- which is to say, when somebody is watching
something. That is evidence the disk-side signals cannot see on their
own:

* A player with a full buffer issues no reads at all for seconds at a
  time. The contention window reads that silence as "nobody is
  streaming", which is precisely when a download would be let back up
  to full speed and the next buffer refill would stutter.
* A saturated video engine means further transcodes fall back to
  software, changing both the CPU picture and the read pattern.

```sh
# while the GPU is >=70% busy, hold pressure at no less than 0.5
setfattr -n user.mergerfs.qos.gpu -v 70,0.5 /mnt/pool/.mergerfs
```

This contributes a *floor* under the governor's pressure rather than a
term in its feedback: while the engine is busy, yielding classes never
return to full speed however quiet the disks look. The latency loop
still runs on top and can push pressure higher. The floor applies only
to branches that were streamed from in the last minute, so a disk
nobody has touched is never throttled on account of a transcode
elsewhere.

The source is the amdgpu sysfs busy counter. There is no driver,
library or link time dependency in any configuration -- it opens a file
in `/sys` and parses an integer. Intel i915 and NVIDIA do not publish
an equivalent single percentage there; on those, and on a host with no
GPU, `qos.gpu` reports unsupported and assigning a non-zero threshold
fails with `EOPNOTSUPP` rather than silently doing nothing.

**The GPU is not used to make scheduling decisions, and should not be.**
Classifying a request is a string match costing a few hundred
nanoseconds; a dispatch to a device costs tens of microseconds before
any work begins. On a code path whose whole purpose is protecting
playback latency that trade is backwards. The GPU here is an input,
never a processor.


## The mover

The `balance` option steers new creates towards the branches that are
behind, which levels a pool over time but only as fast as new data
arrives -- a disk that is already full stays full. The mover relocates
data that is already written. The two are complementary and there is
no reason not to run both.

```sh
setfattr -n user.mergerfs.qos.mover \
  -v 'policy=percent-full,high=90,low=85,interval=300' /mnt/pool/.mergerfs

getfattr -n user.mergerfs.qos.mover --only-values /mnt/pool/.mergerfs
```

| key | default | meaning |
|---|---|---|
| `policy` | `off` | `off`, `percent-full` or `time-based` |
| `interval` | `60` | seconds between passes |
| `high` | `90` | percent full at which a branch starts shedding |
| `low` | `85` | the level the pool is being levelled towards |
| `age` | `90` | `time-based`: days since last access |
| `from`, `to` | - | `time-based`: source and destination branch paths |
| `pressure` | `0.05` | pause while either end is under more backoff than this |
| `rate` | `0` | cap, e.g. `50M`; `0` is unlimited |
| `max-files` | `0` | per pass; `0` is unlimited |

Keys not given keep their current values, so one can be adjusted
without restating the rest.

`percent-full` moves the largest files off the fullest branch onto the
emptiest one that has room, stopping when the source reaches `low` *or*
the destination rises to `low`, whichever comes first. `time-based`
moves files not accessed in `age` days from `from` to `to`, coldest
first -- cold media off the fast disk, or off a disk about to be
retired.

### Why this belongs in the daemon

A mover is a bulk sequential reader and writer hitting two disks at
once, which is the single most disruptive thing that can happen to a
pool somebody is streaming from. An external script has no way to know
when that is, so it gets scheduled for 3am and hoped for.

The daemon already knows. The mover consults the same per-branch
pressure signal the governor controls against, and it announces its own
traffic as yielding -- necessary, because it reads and writes the
branches directly and never passes through the FUSE path, so nothing
would otherwise tell the governor there was anything here willing to
give way. The result is a mover that runs flat out when the pool is
quiet and gets out of the way within a second or two of somebody
pressing play, with no schedule and no playback detection of its own.

It also runs at `nice 19` and idle I/O priority, and `rate` is there
for the case where the destination is fast enough that nothing ever
registers as contended.

### Safety

This is the only part of mergerfs that deletes data, so:

* Copy, verify, rename, and only then unlink the source. A source that
  changed underneath the copy is re-copied rather than trusted.
* **A file with more than one link is never moved.** Copying it would
  silently split the hardlink into two independent files, and nothing
  afterwards can put that back together.
* **A path that already exists on the destination is skipped**, never
  overwritten. Resolving a duplicate by picking a winner is not a
  decision this makes silently. These are counted in `skipped`, not
  `errors`.
* Never onto a branch that is RO or NC, or that lacks room for the file
  plus its `minfreespace`.
* A process holding the file open keeps reading the copy it already
  has, by ordinary unlink semantics. It sees no error.

If `moved` stays at zero, read `skipped` and `last-error`. The usual
cause is [minfreespace](minfreespace.md), which defaults to 4GiB: a
destination branch smaller than that never accepts anything.


## Example

```
capacity default 150M

# Playback is what we protect. Never throttled; its latency is the
# signal everything else is governed by.
class playback  protect ioprio=rt:0 nice=-5

# Services playback blocks on. Never throttled, never a signal.
class helpers   critical

# A media server's own background work -- scans, thumbnails, chapter
# and credits detection -- yields, but gently.
class scanning  yield=60  floor=10% ioprio=be:6 nice=10

# Downloads yield first and hardest, but never stop. `govern` also
# pins the downloader's own process to the bottom of both schedulers,
# which covers the I/O it does outside the pool.
class downloads govern yield=100 floor=5%  ioprio=idle nice=19

# Audio helpers first: a transcode blocks on these.
match comm ~ EasyAudioEncode*                     -> helpers
match comm = "Plex EAE Service"                   -> helpers

# A media server's analysis work runs under the same binary as
# playback; only the command line separates them.
match comm = "Plex Transcoder" cmdline ~ */Transcode/Detection/* -> scanning
match comm = "Plex Media Scanner"                 -> scanning
match comm ~ ffdetect                             -> scanning

# Real transcodes and direct play, across every server.
match comm = "Plex Transcoder"                    -> playback
match comm ~ *ffmpeg*                             -> playback
match comm ~ jellyfin                             -> playback
match comm ~ EmbyServer                           -> playback

# Anything else those containers do.
match cgroup ~ *lxc/311[123]*                     -> scanning

# Downloaders.
match comm ~ qbittorrent*                         -> downloads
match comm ~ sabnzbd*                             -> downloads

default scanning
```

With that loaded, a working configuration for a media pool is:

```sh
ctl=/mnt/pool/.mergerfs

setfattr -n user.mergerfs.qos.rules    -v /etc/mergerfs/qos.rules "$ctl"
setfattr -n user.mergerfs.qos.calibrate -v 3 "$ctl"   # once, while quiet
setfattr -n user.mergerfs.qos.govern   -v 10 "$ctl"
setfattr -n user.mergerfs.qos.gpu      -v 70,0.5 "$ctl"
setfattr -n user.mergerfs.qos.mover    -v 'policy=percent-full,interval=300' "$ctl"
```

A client that reads the pool over NFS or SMB -- Kodi on another
machine, typically -- arrives as `nfsd` or `smbd`, not as itself.
Match those, or match on `path`, since the process behind the export is
not the one you care about.


## Testing it

A ruleset that looks right is not the same as playback that does not
stutter. `mergerfs.qos-playback-test` models a player: it reads a real
file out of the pool at a fixed bitrate through a jitter buffer, starts
and stops competing load partway through, and counts the moments the
buffer ran dry.

```sh
mergerfs.qos-playback-test /mnt/pool/Movies/film.mkv \
    --bulk-file /mnt/pool/TV/something.mkv \
    -b 25M --buffer 5 -d 60 --bulk-start 10 --bulk-stop 45 \
    -m /mnt/pool
```

It exits non-zero if the buffer ever emptied. The `pressure` column
shows the governor engaging and releasing.


## Limitations

**Buffered writes are attributed to the kernel, not the writer.** A
write that lands in the page cache is flushed later by kernel writeback
threads, which are not the calling process and carry none of its
identity. Those requests fall to the `default` class. Rate limiting a
writer works because the limit is applied when the write *enters*
mergerfs; ioprio on the worker thread does not, for the same reason it
does not for any other buffered write.

**Throttling is bounded, and therefore best effort.** mergerfs hands
requests from its read threads to a bounded process thread queue. A
sleeping process thread is one not serving anyone, and once that queue
backs up, every class queues behind it. So no more than
`qos.max-sleepers` threads sleep at once and no request is delayed
longer than `qos.max-sleep-ms`; anything that would exceed either is let
through and counted in the `passed` stat. A class generating requests
far faster than the limiter can absorb will exceed its rate. If
`passed` badly outweighs `throttled`, raise `qos.max-sleepers` --
accepting that the queue is then likelier to stall.

**Classification is cached per pid for ten seconds.** A pid recycled
onto a different class inside that window is briefly misclassified.

**Only reads and writes are governed.** Metadata operations are not,
being neither large nor slow enough to be worth the syscalls.

**Direct access to a branch is still invisible.** Anything reading or
writing `/mnt/disk1` rather than the pool never reaches mergerfs and
cannot be classified per request. Two things now narrow this:
[the process governor](#the-process-governor) sets the offending
process's own `nice` and `ioprio`, which is what the kernel *can* act
on for I/O it issues directly; and [the mover](#the-mover), the usual
reason a rebalance script existed, is now inside the daemon and
announces its traffic to the governor. A third party tool writing to a
branch behind mergerfs's back remains outside all of this.


## Who may change these

The control file is `0664`, owned by the daemon's uid and gid, and the
kernel's permission check on that mode is what keeps other users away
from every `mergerfs` runtime key. The `qos.*` keys are additionally
gated inside the daemon: assigning one requires the caller to be root
or the daemon's own uid, and a member of the daemon's group who can
write every other key is refused these with `EPERM`. Reading is not
restricted.

The distinction exists because these keys are not configuration in the
way `cache.attr` is. `qos.govern` renices other users' processes,
`qos.calibrate` with `write` creates files on every branch, and
`qos.mover` relocates data -- all with the daemon's privilege.

The check identifies the caller from the request's pid when the kernel
does not put credentials in the request header, which recent kernels do
not for `setxattr`. A caller it cannot identify is refused.


## Build options

Every piece of this can be compiled out. `USE_QOS=0` removes the
subsystem outright -- the read and write paths lose their `qos::Apply`,
throttle and timing calls entirely rather than branching past them, so
a build that does not want any of it pays nothing for it.

| flag | default | removes |
|---|---|---|
| `USE_QOS=0` | `1` | the whole subsystem, including the hot path |
| `USE_QOS_GOVERN=0` | `1` | the client process governor |
| `USE_QOS_CALIBRATE=0` | `1` | the active capacity probe (passive measurement stays) |
| `USE_QOS_MOVER=0` | `1` | the background file mover |
| `USE_QOS_GPU=0` | `1` | the GPU signal |

```sh
make USE_QOS_GPU=0 USE_QOS_MOVER=0
```

The `qos.*` runtime keys stay registered in every configuration, so a
caller gets a clear "unsupported" rather than an unknown-key error that
looks like a typo. Every sub-feature is also off at runtime by default,
so enabling them is always a deliberate act.


## Supported platforms

Linux only. `ioprio_set` and `/proc/<pid>/cgroup` have no equivalent
elsewhere; on other platforms the option parses and does nothing.


## Performance impact

A ruleset that cannot change anything is detected at parse time and
skipped entirely. Otherwise classification costs one small `/proc` read
per process per ten seconds, cached per worker thread, and only for the
fields some rule actually uses. Applying a class costs one
`ioprio_set` and one `setpriority` per *change*, not per request.
