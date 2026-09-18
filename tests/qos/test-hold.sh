#!/bin/bash
# Functional test of `hold=`: a class held at its floor while a
# protected class has recently read the same resource, and the mover
# paced at hold-rate under the same condition.
#
# Runs inside the test container (privileged, fuse3, attr):
#   docker run --rm --privileged -v $PWD:/src -w /src mergerfs-test \
#     bash tests/qos/test-hold.sh
set -u
MFS=${MFS_BIN:-build/mergerfs}
T=$(mktemp -d /tmp/qos-hold.XXXXXX)
FAILED=0
ok(){ printf '  ok   %s\n' "$1"; }
bad(){ printf '  FAIL %s -- %s\n' "$1" "$2"; FAILED=1; }
cleanup(){
  mountpoint -q $T/pool 2>/dev/null && fusermount3 -uz $T/pool
  sleep 0.5
  for m in $T/br1 $T/br2; do mountpoint -q $m 2>/dev/null && umount -l $m; done
  rm -rf $T
}
trap cleanup EXIT
mkdir -p $T/br1 $T/br2 $T/pool
mount -t tmpfs -o size=64M tmpfs $T/br1
mount -t tmpfs -o size=256M tmpfs $T/br2
mkdir -p $T/br1/media

# comm is the executable's name, so each role gets its own copy of dd.
cp "$(command -v dd)" $T/qos-playback
cp "$(command -v dd)" $T/qos-scan

dd if=/dev/urandom of=$T/br1/media/film.bin bs=1M count=4 status=none
dd if=/dev/urandom of=$T/br1/media/scan.bin bs=1M count=8 status=none

CTL=$T/pool/.mergerfs
get(){ getfattr --only-values -n "user.mergerfs.$1" "$CTL" 2>/dev/null; }
set_(){ setfattr -n "user.mergerfs.$1" -v "$2" "$CTL" 2>&1; }

echo "== parser =="
mk(){ printf '%s\n' "$@" > $T/r.txt; }
mk "class a yield=0 hold=5s" "default a"
$MFS -o qos=true,qos.rules=$T/r.txt $T/br1 $T/pool 2>/dev/null && { fusermount3 -uz $T/pool; bad "hold without floor" "accepted"; } || ok "hold without floor refused"
mk "class a protect hold=5s floor=1M" "default a"
$MFS -o qos=true,qos.rules=$T/r.txt $T/br1 $T/pool 2>/dev/null && { fusermount3 -uz $T/pool; bad "hold on protect" "accepted"; } || ok "hold on protect refused"

cat > $T/rules.txt <<RULES
capacity default 200M
class playback protect ioprio=be:0
class scan     yield=0 floor=1M hold=3s ioprio=be:6
class other
match comm = qos-playback -> playback
match comm = qos-scan     -> scan
default other
RULES

$MFS -f -o qos=true,qos.rules=$T/rules.txt,cache.files=off,category.create=mfs,minfreespace=1M $T/br1:$T/br2 $T/pool &
for i in $(seq 40); do mountpoint -q $T/pool && break; sleep 0.25; done
mountpoint -q $T/pool || { echo "MOUNT FAILED"; exit 1; }
get qos.ruleset | grep -q "hold=3s" && ok "ruleset echoes hold=3s" || bad "ruleset" "hold not echoed: $(get qos.ruleset | head -3)"

scan_secs(){ local s=$(date +%s.%N); $T/qos-scan if=$T/pool/media/scan.bin of=/dev/null bs=64K status=none; awk "BEGIN{print $(date +%s.%N) - $s}"; }

echo "== class hold =="
BASE=$(scan_secs)
echo "  baseline scan of 8MiB: ${BASE}s"
# One protected read marks the branch; the hold then lasts 3s.
$T/qos-playback if=$T/pool/media/film.bin of=/dev/null bs=1M status=none
HELD=$(scan_secs)
echo "  scan right after playback: ${HELD}s"
STATS=$(get qos.stats)
echo "$STATS" | grep "^scan:"
if [ "$(awk "BEGIN{print ($HELD > 2.5)}")" = 1 ]; then ok "scan held at floor (${HELD}s vs ${BASE}s)"; else bad "hold" "scan not slowed: ${HELD}s"; fi
echo "$STATS" | grep -q "^scan:.*held=[1-9]" && ok "held counter advanced" || bad "held counter" "$(echo "$STATS" | grep ^scan:)"
sleep 4
AFTER=$(scan_secs)
echo "  scan after hold lapsed: ${AFTER}s"
if [ "$(awk "BEGIN{print ($AFTER < 1.0)}")" = 1 ]; then ok "hold lapsed after window"; else bad "lapse" "still slow: ${AFTER}s"; fi
get qos.stats.json | grep -o '"name": "[^"]*", "contended": [a-z]*, [^}]*protected_age_ms": [0-9nul]*' | head -3

echo "== mover hold =="
# Fill br1 past `high` so percent-full moves scan.bin off it; hold it
# by playing film.bin (also on br1) throughout.
dd if=/dev/urandom of=$T/br1/media/fill.bin bs=1M count=40 status=none
echo "  br1 used $(df --output=pcent $T/br1 | tail -1 | tr -d ' ')"
( END=$((SECONDS+25)); while [ $SECONDS -lt $END ]; do $T/qos-playback if=$T/pool/media/film.bin of=/dev/null bs=1M status=none; sleep 0.5; done ) &
PB=$!
S=$(date +%s)
set_ qos.mover "policy=percent-full,interval=1,high=50,low=10,pressure=1.0,hold=5s,hold-rate=1M,max-files=1" >/dev/null && ok "mover configured" || bad "mover" "configure rejected"
for i in $(seq 60); do
  MS=$(get qos.mover)
  echo "$MS" | grep -q "moved=[1-9]" && break
  sleep 1
done
E=$(( $(date +%s) - S ))
kill $PB 2>/dev/null; wait $PB 2>/dev/null
echo "$MS"
echo "$MS" | grep -q "moved=[1-9]" && ok "mover moved a file in ${E}s" || bad "mover" "nothing moved"
echo "$MS" | grep -q "holds=[1-9]" && ok "mover counted a hold" || bad "mover holds" "holds=0"
# The 8MiB file at 1MiB/s (the 40MiB fill exceeds the destination budget):
# >= 7s means the copy was really paced.
[ "$E" -ge 7 ] && ok "copy was paced (${E}s)" || bad "pace" "too fast for hold-rate=1M: ${E}s"
get qos.stats.json | grep -o '"mover": {[^}]*}' | sed 's/"last_error.*//'

set_ qos.mover "policy=off" >/dev/null
echo
[ $FAILED = 0 ] && echo "ALL OK" || { echo "FAILURES"; exit 1; }
