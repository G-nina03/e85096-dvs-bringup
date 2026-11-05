#!/bin/sh
# One S5KRC1S capture with the CSIS ISR counted: dvscap.sh [speed_mbps] [settle]   (0 = DT value)
# Raises MIF/INT/CAM devfreq to max (else CSIS overflows, 11.22), loads fimc-is + CIS if needed,
# sets fimc_is.e850_csi_speed/settle, captures 3 VC0 frames to /tmp/ov5647_N.raw.
set -u
P=/sys/module/fimc_is/parameters
for x in mif int cam; do f=$(ls -d /sys/class/devfreq/*_$x); cat $f/max_freq > $f/min_freq; done
lsmod | grep -q '^fimc_is ' || modprobe fimc-is
lsmod | grep -q '^is_cis_s5krc1s ' || modprobe is-cis-s5krc1s
echo "${1:-0}" > $P/e850_csi_speed
echo "${2:-0}" > $P/e850_csi_settle
rm -f /tmp/ov5647_*.raw
insmod /root/csis-irqstat.ko
ov5647-cap.py --raw8 -W 8192 -H 127 -n 3 -b 4 -B 8 -t 3 > /tmp/cap.log 2>&1
rmmod csis-irqstat
grep -E 'settle\(' /tmp/cap.log; dmesg | grep -E '\[CSI1\] settle' | tail -1
grep -E 'vc0 frame|stopped' /tmp/cap.log
dmesg | grep csis-irqstat | tail -6
