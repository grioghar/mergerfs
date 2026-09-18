#!/bin/bash
# Functional test of the mover's `queue` policy: named trees moved whole
# from one branch to another, in order, emptied directories pruned, a
# hardlinked file left behind with its entry not done, and the
# destination's accept gate honoured.
#
#   docker run --rm --privileged -v $PWD:/src -w /src mergerfs-test \
#     bash tests/qos/test-queue.sh
set -u
MFS=${MFS_BIN:-build/mergerfs}
T=$(mktemp -d /tmp/qos-queue.XXXXXX)
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
mount -t tmpfs -o size=64M tmpfs $T/br2

mk(){ mkdir -p "$(dirname "$1")"; dd if=/dev/urandom of="$1" bs=1M count=$2 status=none; }
mk "$T/br1/TV/Show A/Season 1/e1.mkv" 2
mk "$T/br1/TV/Show A/Season 1/e2.mkv" 2
mk "$T/br1/TV/Show A/Season 2/e1.mkv" 1
mk "$T/br1/TV/Show B/e1.mkv" 1
mk "$T/br1/TV/Show B/linked.mkv" 1
ln "$T/br1/TV/Show B/linked.mkv" "$T/br1/TV/Show B/linked-twin.mkv"
mk "$T/br1/Movies/Film C/film.mkv" 1
mk "$T/br1/TV/Show D/e1.mkv" 1
mkdir -p $T/br2/TV
SUM_BEFORE=$(cd $T/br1 && find TV Movies -type f -print0 | sort -z | xargs -0 md5sum | md5sum)

cat > $T/queue.txt <<Q
# comment line
$T/br1|$T/br2|TV|Show A
$T/br1|$T/br2|TV|Show B
$T/br1|$T/br2|Movies|Film C
$T/br1|$T/nowhere|TV|Show D
bad line without bars
Q
cat > $T/rules.txt <<RULES
class other
default other
RULES

# br2 accepts only TV: Film C must be refused by the gate, not moved.
$MFS -f -o qos=true,qos.rules=$T/rules.txt,cache.files=off,category.create=mfs,minfreespace=1M "$T/br1:$T/br2=RW,accept=/TV/*" $T/pool &
for i in $(seq 40); do mountpoint -q $T/pool && break; sleep 0.25; done
mountpoint -q $T/pool || { echo "MOUNT FAILED"; exit 1; }
CTL=$T/pool/.mergerfs
get(){ getfattr --only-values -n "user.mergerfs.$1" "$CTL" 2>/dev/null; }
set_(){ setfattr -n "user.mergerfs.$1" -v "$2" "$CTL" 2>&1; }

echo "== configure =="
set_ qos.mover "policy=queue" >/dev/null 2>&1 && bad "queue without file" "accepted" || ok "queue without file refused"
set_ qos.mover "policy=queue,queue=/nonexistent" >/dev/null 2>&1 && bad "missing file" "accepted" || ok "missing queue file refused"
set_ qos.mover "policy=queue,queue=$T/queue.txt,interval=1,pressure=1.0" >/dev/null && ok "configured" || bad "configure" "rejected"

for i in $(seq 40); do
  MS=$(get qos.mover)
  echo "$MS" | grep -q "passes=[2-9]" && echo "$MS" | grep -q "state=idle" && break
  sleep 1
done
echo "$MS" | sed 's/^/  /'

echo "== results =="
[ -d "$T/br1/TV/Show A" ] && bad "Show A source" "still on br1" || ok "Show A source tree pruned"
[ -f "$T/br2/TV/Show A/Season 1/e2.mkv" ] && [ -f "$T/br2/TV/Show A/Season 2/e1.mkv" ] && ok "Show A wholly on br2" || bad "Show A dest" "incomplete"
[ -f "$T/br2/TV/Show B/e1.mkv" ] && ok "Show B plain file moved" || bad "Show B e1" "not moved"
[ -f "$T/br1/TV/Show B/linked.mkv" ] && [ -f "$T/br1/TV/Show B/linked-twin.mkv" ] && ok "hardlinked pair left on br1" || bad "hardlink" "moved or lost"
[ ! -e "$T/br2/TV/Show B/linked.mkv" ] && ok "hardlink not split" || bad "hardlink" "copied to br2"
[ -f "$T/br1/Movies/Film C/film.mkv" ] && [ ! -e "$T/br2/Movies" ] && ok "Film C refused by accept gate" || bad "accept gate" "Film C moved"
[ -f "$T/br1/TV/Show D/e1.mkv" ] && ok "Show D with bogus destination untouched" || bad "Show D" "moved somewhere"
echo "$MS" | grep -q "done=1/4" && ok "queue reports done=1/4" || bad "done count" "$(echo "$MS" | grep -o 'done=[0-9/]*')"
# last-error holds the most recent problem; the bogus destination is
# reported after the bad line, so either text proves the report works.
echo "$MS" | grep -q -E "bad line|not a branch" && ok "problems reported in last-error" || bad "last-error" "$(echo "$MS" | grep last-error)"
echo "$MS" | grep -q "skipped=[1-9]" && ok "refused entries counted as skipped" || bad "skipped" "0"
SUM_AFTER=$(cd $T/pool && find TV Movies -type f -print0 | sort -z | xargs -0 md5sum | md5sum)
[ "$SUM_BEFORE" = "$SUM_AFTER" ] && ok "every file byte-identical through the pool" || bad "content" "checksums differ"
get qos.stats.json | grep -o '"mover": {[^}]*}' | grep -o '"queue_total[^,]*, "queue_done[^,]*, "current[^,]*'

echo "== policy=off interrupts a running pass =="
set_ qos.mover "policy=off" >/dev/null
mk "$T/br1/TV/Show E/big.mkv" 48
printf '%s\n' "$T/br1|$T/br2|TV|Show E" > $T/queue2.txt
set_ qos.mover "policy=queue,queue=$T/queue2.txt,interval=1,pressure=1.0,rate=4M" >/dev/null
sleep 3
get qos.mover | grep -q "state=moving" && ok "pass running (48MiB at 4MiB/s)" || bad "pass" "not running: $(get qos.mover | grep state)"
set_ qos.mover "policy=off" >/dev/null
for i in 1 2 3 4 5 6; do get qos.mover | grep -q "state=idle" && break; sleep 0.5; done
get qos.mover | grep -q "state=idle" && ok "pass stopped within 3s of policy=off" || bad "interrupt" "$(get qos.mover | grep state)"
[ -f "$T/br1/TV/Show E/big.mkv" ] && ok "in-flight source left intact" || bad "source" "gone"
[ -z "$(find $T/br2 -name '.mergerfs-mover.*')" ] && ok "no temp file left on destination" || bad "temp" "$(find $T/br2 -name '.mergerfs-mover.*')"
echo
[ $FAILED = 0 ] && echo "ALL OK" || { echo "FAILURES"; exit 1; }
