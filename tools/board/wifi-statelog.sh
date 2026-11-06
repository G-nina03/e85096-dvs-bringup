#!/bin/sh
# wifi-statelog.sh OUT: every 0.2 s "<unix time> <operstate> <wpa_state> <bssid> <ipv4>" (run as root)
while :; do
  s=$(wpa_cli -i wlan0 status 2>/dev/null)
  echo "$(date +%s.%N) $(cat /sys/class/net/wlan0/operstate 2>/dev/null) $(echo "$s" | sed -n 's/^wpa_state=//p') $(echo "$s" | sed -n 's/^bssid=//p') $(echo "$s" | sed -n 's/^ip_address=//p')"
  sleep 0.2
done >> "$1"
