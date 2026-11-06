#!/bin/sh
# Put the S5KRC1S (DVS) Pablo CIS driver and E850-96 CN4 overlay into the Linaro exynos-5.10 tree.
#
# usage: apply.sh /path/to/kernel   (run again safely)
#
# The tree already has SENSOR_NAME_S5KRC1S (42), CAMERA_CIS_S5KRC1S_OBJ and its Makefile line.
# The board dts includes one CN4 overlay; this switches it from OV5647 back to S5KRC1S.
set -eu
K=${1:?usage: apply.sh /path/to/kernel}
HERE=$(cd "$(dirname "$0")" && pwd)
CIS=drivers/media/platform/exynos/camera/sensor/module_framework/cis
BOARD=arch/arm64/boot/dts/exynos/board/erd/s5e3830-erd3830.dts

cp -v "$HERE"/files/$CIS/is-cis-s5krc1s* "$K/$CIS/"
cp -v "$HERE"/files/arch/arm64/boot/dts/exynos/board/erd/camera/*s5krc1s* \
      "$K/arch/arm64/boot/dts/exynos/board/erd/camera/"
grep -q SENSOR_NAME_S5KRC1S "$K/include/dt-bindings/camera/exynos_is_dt.h"
grep -q CAMERA_CIS_S5KRC1S_OBJ "$K/$CIS/Kconfig"

sed -i 's|#include "camera/s5e3830-e850-ov5647-camera.dtsi"|#include "camera/s5e3830-e850-s5krc1s-camera.dtsi"|' "$K/$BOARD"
grep -q 's5e3830-e850-s5krc1s-camera.dtsi' "$K/$BOARD"

# A CSIS VC overflow panics the kernel on v8_20_0 (vendor debug option; v8_10_0/v8_1_0 #undef it).
# With panic=0 the board then hangs (11.20). Log the error instead: needs a fimc-is.ko rebuild.
CFG=drivers/media/platform/exynos/camera/ischain/is-v8_20_0/is-config.h
sed -i 's|^#define OVERFLOW_PANIC_ENABLE_CSIS$|#undef OVERFLOW_PANIC_ENABLE_CSIS\t/* e850: log CSIS overflow, no panic */|' "$K/$CFG"
grep -n 'OVERFLOW_PANIC_ENABLE_CSIS' "$K/$CFG"

# CSIS v1.2: write back the per-VC error status (it kept the link IRQ asserted) and do not treat a
# VRESOL mismatch as an error (a DVS frame length is not fixed). 11.22. Needs a fimc-is.ko rebuild.
patch -d "$K" -p1 -N -r - < "$HERE/patches/csis-v1_2-err-clear.patch" ||
    patch -d "$K" -p1 -R --dry-run < "$HERE/patches/csis-v1_2-err-clear.patch" >/dev/null
# fimc_is.e850_csi_speed / e850_csi_settle: override the DT lane speed and settle at stream on
# (/sys/module/fimc_is/parameters, 0 = DT) for the CRC experiments without a new DTB. 11.22.
patch -d "$K" -p1 -N -r - < "$HERE/patches/csi-speed-param.patch" ||
    patch -d "$K" -p1 -R --dry-run < "$HERE/patches/csi-speed-param.patch" >/dev/null
# fimc_is.e850_csi_err_stop: CSIS gives up (free_irq, DMA off) after this many error frames; vendor 10,
# 0 = never. Every S5KRC1S frame has CRC errors, so live view needs 0. 11.24.
patch -d "$K" -p1 -N -r - < "$HERE/patches/csi-err-stop-param.patch" ||
    patch -d "$K" -p1 -R --dry-run < "$HERE/patches/csi-err-stop-param.patch" >/dev/null
grep -n 'camera/' "$K/$BOARD"
echo "applied to $K"
