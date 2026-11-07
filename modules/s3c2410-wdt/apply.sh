#!/bin/sh
# Quiet the vendor watchdog driver: every ping (busybox watchdog, 200 ms, both clusters) logged
# "keepalive!" and read the PMIC RTC, which logged again: 20 journal lines/s (docs 11.32).
#   sh modules/s3c2410-wdt/apply.sh /work/kernel   (run again safely), then rebuild modules and the
#   boot image: s3c2410_wdt.ko is a stage-1 module in the initramfs.
set -eu
K=${1:?usage: apply.sh /path/to/kernel}
HERE=$(cd "$(dirname "$0")" && pwd)
patch -d "$K" -p1 -N -r - < "$HERE/quiet-keepalive.patch" ||
    patch -d "$K" -p1 -R --dry-run < "$HERE/quiet-keepalive.patch" >/dev/null
