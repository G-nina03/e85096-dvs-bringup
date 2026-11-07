#!/bin/sh
# Two vendor bugs in the Maxwell Wi-Fi drivers (docs 11.33):
#  - scsc_wlan dev.c slsi_add_log_to_system_error_buffer: the size was subtracted twice, so once the
#    system error log passed 2 KB it went negative (vsnprintf WARN, taint W, after the 4th recovery
#    in 11.31); the message was also used as the format string
#  - scsc_mx mxproc.c: /proc/driver/mxman_ctrl0/mx_panic was 0666, and even a read forces a FW panic
#   sh modules/scsc-wifi/apply.sh /work/kernel   (run again safely), then rebuild modules and copy
#   scsc_mx.ko, scsc_wlan.ko to the rootfs (e850-wifi.service loads them from there).
set -eu
K=${1:?usage: apply.sh /path/to/kernel}
HERE=$(cd "$(dirname "$0")" && pwd)
patch -d "$K" -p1 -N -r - < "$HERE/vendor-fixes.patch" ||
    patch -d "$K" -p1 -R --dry-run < "$HERE/vendor-fixes.patch" >/dev/null
