#!/bin/sh
# S5KRC1S live view on HDMI, settings page on :8080 (CTL_PORT): dvslive.sh [ov5647-cap.py args, e.g. --boxes FILE]
# MIF/INT/CAM devfreq to max (else CSIS overflows, 11.22), fimc-is with the error stop off (11.24) and the CRC
# keep on (CRC_KEEP=0 for the vendor abort, 11.28), CIS with a 50 us MIPI flush (FLUSH_US; 4x the buffers of
# 250 us, 11.28). Log: /tmp/dvslive.log.
set -u
for x in mif int cam; do f=$(ls -d /sys/class/devfreq/*_$x); cat $f/max_freq > $f/min_freq; done
lsmod | grep -q '^fimc_is ' || modprobe fimc-is
lsmod | grep -q '^is_cis_s5krc1s ' || modprobe is-cis-s5krc1s
echo 0 > /sys/module/fimc_is/parameters/e850_csi_err_stop
echo "${CRC_KEEP:-1}" > /sys/module/fimc_is/parameters/e850_csi_crc_keep
echo "${FLUSH_US:-50}" > /sys/module/is_cis_s5krc1s/parameters/flush_us
# a second open of /dev/video102 fails while the old process still holds it
while pkill -TERM -f 'ov5647-cap.py --live'; do sleep 2; done
setsid nohup ov5647-cap.py --live --raw8 -W 8192 -H 127 -n 0 -b 8 -B 16 -t 3 --ctl "${CTL_PORT:-8080}" "$@" \
    > /tmp/dvslive.log 2>&1 < /dev/null &
