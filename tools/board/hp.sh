#!/bin/sh
# hp.sh [rounds] [random toggles]: CPU1-7 hotplug soak (needs e850-cpuhp.ko loaded)
# hotplug soak: N rounds of all CPU1-7 off then on, then random single toggles
C=/sys/devices/system/cpu
fail=0
for r in $(seq 1 ${1:-20}); do
  for c in 1 2 3 4 5 6 7; do echo 0 > $C/cpu$c/online || fail=$((fail+1)); done
  [ "$(cat $C/online)" = 0 ] || fail=$((fail+1))
  for c in 1 2 3 4 5 6 7; do echo 1 > $C/cpu$c/online || fail=$((fail+1)); done
  [ "$(cat $C/online)" = 0-7 ] || { fail=$((fail+1)); echo "round $r: online=$(cat $C/online)"; }
done
for i in $(seq 1 ${2:-200}); do
  c=$(( $(od -An -N1 -tu1 /dev/urandom) % 7 + 1 ))
  v=$(( 1 - $(cat $C/cpu$c/online) ))
  echo $v > $C/cpu$c/online || fail=$((fail+1))
done
for c in 1 2 3 4 5 6 7; do echo 1 > $C/cpu$c/online 2>/dev/null; done
echo "HP DONE fail=$fail online=$(cat $C/online)"
