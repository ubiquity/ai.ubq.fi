#!/bin/bash
# Runs ops/rust-worker-build.sh in the isolated checkout with a disk guard (>=3 GiB free) and peak sampling.
set -u
T=/home/codex/rust-worker-prep
LOG=$T/build.log
ls -la ~/.cache/worker-build $T/cache/worker-build > $T/worker-build-cache.before 2>&1
cd $T/checkout
start=$(date +%s); echo "start $(date -u +%FT%TZ) sha=$(git rev-parse HEAD)" > $T/build.meta
env -i HOME="$HOME" bash -c ". $T/worker-build-env.sh; exec timeout 3600 nice -n 19 ionice -c3 /bin/bash ops/rust-worker-build.sh" > $LOG 2>&1 &
pid=$!
minfree=999999999; maxtarget=0
while kill -0 $pid 2>/dev/null; do
  free=$(df --output=avail -k / | tail -1); [ "$free" -lt "$minfree" ] && minfree=$free
  tg=$(du -sk $T/target 2>/dev/null | cut -f1); [ "${tg:-0}" -gt "$maxtarget" ] && maxtarget=$tg
  if [ "$free" -lt 3145728 ]; then echo "DISK GUARD: avail ${free}k < 3GiB, killing build" >> $T/build.meta; pkill -TERM -P $pid; kill -TERM $pid; fi
  sleep 10
done
wait $pid; rc=$?
end=$(date +%s)
echo "end $(date -u +%FT%TZ) rc=$rc secs=$((end-start)) min_avail_kib=$minfree peak_target_kib=$maxtarget" >> $T/build.meta
ls -la ~/.cache/worker-build $T/cache/worker-build > $T/worker-build-cache.after 2>&1
echo DONE >> $T/build.meta
