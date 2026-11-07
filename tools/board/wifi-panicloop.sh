#!/bin/sh
# wifi-panicloop.sh N: N forced WLBT FW panics (run as root, needs disable_recovery_handling=0).
# After each: ms until mx_recovery_count went up and wlan0 is COMPLETED with an IPv4 address again.
# At the end: the kernel WARNING count since the start (docs 11.33).
P=/proc/driver/mxman_ctrl0
ms() { echo $(( $(date +%s%N) / 1000000 )); }
s=$(date +%s)
for i in $(seq 1 "$1"); do
    c0=$(cat $P/mx_recovery_count); t0=$(ms)
    echo 1 > $P/mx_panic
    r="NOT BACK in 40 s"
    while [ $(( $(ms) - t0 )) -lt 40000 ]; do
        [ "$(cat $P/mx_recovery_count)" -gt "$c0" ] &&
            wpa_cli -i wlan0 status 2>/dev/null | grep -qx wpa_state=COMPLETED &&
            ip -4 addr show wlan0 | grep -q inet && { r="back after $(( $(ms) - t0 )) ms"; break; }
        sleep 0.2
    done
    echo "panic $i: count $(cat $P/mx_recovery_count), $(tr -d '\n' < $P/mx_lastpanic), $r"
    sleep 8
done
echo "WARNING lines since start: $(journalctl -k --since=@$s --no-pager -o cat | grep -c 'WARNING:')"
journalctl -k --since=@$s --no-pager -o cat | grep -E 'WARNING:|vsnprintf' | head -3
