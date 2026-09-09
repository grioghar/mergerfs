# qos: per-client quality of service

## The problem

mergerfs is one daemon serving every process that touches the pool. To the
kernel, all pool I/O is issued by one set of threads in one cgroup, so block
layer priority cannot tell a media player apart from a torrent client:
`ionice`, systemd's `IOSchedulingClass=` and cgroup `io.weight` all apply to
the *daemon*, not to whoever is behind the request. Priority is flattened.

`proxy_ioprio` addresses this from the other end by copying whatever ioprio the
caller happens to have. That works when every client is correctly `ionice`d by
whoever started it, which is awkward for programs that respawn workers under a
single service and impossible for clients inside a container the daemon does
not control.

This adds an alternative: put the policy in one file, written in terms of who
is asking and what they are touching.

## What it does

Off by default (`qos=false`), and a ruleset that cannot change anything is
detected at parse time and skipped entirely, so an unused build pays nothing.

**Classification.** Each read and write is matched against an ordered ruleset;
first rule whose conditions *all* match wins. Conditions test `cgroup`, `comm`,
`cmdline`, `uid`, `gid`, `path` (within the pool) and `op` (read/write), with
`=` for exact match and `~` for an fnmatch glob. Patterns may be quoted to
contain spaces.

**Enforcement.** A matched class supplies an `ioprio` and `nice` for the worker
thread, and optionally a bandwidth allowance -- absolute (`8M`) or a share of
the serving resource's measured capacity (`10%`).

**Rates are per resource.** Each class holds one token bucket per branch, so a
class limited to 10% gets a tenth of *each* disk rather than a tenth of the
pool divided across all of them. Two players reading two different disks never
compete for one allowance.

**An adaptive governor.** A fixed cap is blunt: it slows downloads even when
nothing is playing. Marking a class `protect` instead makes its service time
the control signal. When it degrades past a threshold on a resource *and* a
yielding class is active on that same resource, pressure rises and every
yielding class's allowance is scaled by `1 - (pressure x yield/100)`, bounded
below by its `floor`. When latency recovers or playback stops, pressure decays
and allowances return to full. Pressure rises multiplicatively and falls
additively: a stutter has already been heard by the time it is measured.

So bulk traffic runs flat out on a disk nobody is streaming from, gives way
gradually on one that is, and never stops.

## Two things that came out of running this against a real media server

**`cmdline` matching.** Plex runs credits detection using the same
`Plex Transcoder` binary that serves playback, reading the same file. Neither
`comm` nor `path` separates them; the command line does, since detection writes
into `.../Transcode/Detection/`. Measured on an idle pool with the analysis
class capped at 8 MiB/s: detection ran at 8.1 MiB/s, playback -- same binary,
same file -- at 730 MiB/s.

**A `critical` class flag.** Some services playback *synchronously waits on*
look exactly like background work. Plex's EasyAudioEncoder is the case:
transcoding EAC3/TrueHD/DTS runs `Plex Transcoder -codec:1 eac3_eae`, which
hands audio to EAE and blocks. Throttling or suspending such a helper stalls
the stream behind it -- a priority inversion, and an easy one to create by
accident. A `critical` class is never delayed and is not used as a signal.

## Tools

`mergerfs.qos-bench` measures per-branch throughput so percentage rates have
something to resolve against. Reads only, O_DIRECT (so it neither reads stale
numbers from the page cache nor evicts anything), idle priority, short samples;
it discovers branches from a live mount and emits the `capacity` lines a
ruleset needs.

`mergerfs.qos-playback-test` answers the question a config file cannot: does it
still stutter? It models a player -- reads a real file at a fixed bitrate
through a jitter buffer, starts and stops competing load partway through, and
counts the moments the buffer ran dry. Exits non-zero if it ever did.

## Testing

21 unit tests covering rule parsing, matching semantics, capacity/percentage
resolution and per-resource bucket isolation. Full suite passes (196 tests).

Measured on a 7-disk pool and on a scratch mount:

- classification exact -- 513 requests attributed to each of two classes with
  no leakage
- rate limiting accurate -- 64 MiB through an 8 MiB/s class with an 8 MiB burst
  took 7.016s against a predicted 7.0s
- per-resource isolation -- the same `dd` binary got 4.6 MB/s inside a matched
  cgroup and 324 MB/s outside it
- adaptive loop -- pressure 0.00 at baseline, ~1.00 under contention (bulk
  cut from 364 MB/s to 10-52 MB/s), decaying to 0.00 within ~6s of the load
  stopping; playback held its bitrate with zero buffer underruns throughout

The playback harness found two real bugs during development: adaptive classes
were skipping `throttle()` entirely because they carry no static rate, and
pressure rose with no competing load present.

## Limitations, documented

- **Buffered writes are attributed to the kernel, not the writer.** Writeback
  threads carry none of the calling process's identity, so those requests fall
  to the default class. Rate limiting a writer still works, because the limit
  applies as the write enters mergerfs; ioprio on the worker does not, for the
  same reason it does not for any buffered write.
- **Throttling is bounded, therefore best effort.** Requests move from read
  threads to a bounded process thread queue, so a sleeping process thread is
  one not serving anyone and a backed-up queue delays every class. No more than
  `qos.max-sleepers` threads sleep at once (default: half the process pool) and
  no request is delayed beyond `qos.max-sleep-ms`; anything exceeding either is
  let through and counted in a `passed` stat.
- **Classification is cached per pid for ten seconds**, so a recycled pid can
  be briefly misclassified.
- Only reads and writes are governed; metadata operations are not.
- Direct access to a branch bypasses mergerfs entirely and cannot be classified.
- Linux only -- `ioprio_set` and `/proc/<pid>/cgroup` have no equivalent
  elsewhere. Elsewhere the option parses and does nothing.

`qos.distress-ms` (default 50) is the knob that matters and is device
dependent: 50ms is a genuine stall on a spinning disk and unreachable on NVMe,
where the governor would never engage.

## What I have and have not verified

I would rather state this plainly than have you discover it in review.

**Environment.** Everything was built and measured on one machine: Debian
(Proxmox), kernel 7.0.14-16-pve, gcc with `-std=c++20`, on a 7-branch pool of
USB-attached spinning disks plus an NVMe-backed scratch mount. One kernel, one
compiler, one pool layout.

**Not run against upstream CI.** I did not run the project's CI or its
functional test scripts under `tests/` -- only `make` and `make tests` (196
unit tests, all passing).

**Not built on FreeBSD.** The feature is Linux-only by nature and is guarded,
but I have not compiled it on FreeBSD or macOS, so I cannot promise the
non-Linux build is clean.

**The governor's distress detection was never triggered by genuine latency
degradation.** This is the caveat I would want to know about. On the NVMe
scratch mount, three greedy readers saturating the same branch moved playback
read latency from 0.2ms to 1.5ms -- nowhere near the 50ms default, so the
governor correctly stayed dormant. To exercise the control loop at all I had to
set `qos.distress-ms=0` and `qos.distress-factor=1.5`. The loop's *mechanics*
are therefore well tested (pressure rises under contention, decays to zero on
recovery, respects `floor`, and cut bulk from 364 MB/s to 10-52 MB/s while
playback held its bitrate with zero underruns). What is untested is the
*threshold* actually firing on real degradation on real spinning disks under
real playback. The defaults are reasoned, not measured.

**Rates are not hard guarantees.** The fail-open bounds are load-bearing: under
a request rate the limiter cannot absorb, traffic is let through rather than
delayed, and the `passed` stat reflects it. In one NVMe test with three greedy
readers, `passed` outnumbered `throttled` by 280:1 before the `max-sleepers`
auto-sizing fix. Enforcement quality depends on `qos.max-sleepers`, and raising
it trades enforcement against the risk of stalling the shared request queue.

**Defaults are opinionated.** `qos.distress-ms=50`, `qos.distress-factor=3.0`,
the ten-second classification cache, the 2s contention window, the AIMD
constants and the 2s debt ceiling are all judgement calls that seemed sensible
on one pool. I would not be surprised to be wrong about any of them and am
happy to change them.

**Attribution.** This was written with Claude (Anthropic), which is recorded in
the commit trailers. The design, the measurements and the bugs it flushed out
are real, but you should review it as machine-assisted work.

## Note

This is a large feature and I would understand wanting it smaller. It splits
cleanly at the two commits: classification plus static ioprio/nice/rate first,
the adaptive governor and its tools second. Happy to resubmit that way, or to
change any of the naming or defaults.
