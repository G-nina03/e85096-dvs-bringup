#!/bin/sh
# Add the E850-96 HDMI pieces (LT8912B panel entry + DT) to the vendor 5.10 kernel tree.
#   sh modules/e850-hdmi/apply.sh /work/kernel
set -eu
K=$1
H=$(cd "$(dirname "$0")" && pwd)
P=$K/drivers/video/fbdev/exynos/dpu30/panels
cp $H/files/drivers/video/fbdev/exynos/dpu30/panels/panel-lontium-lt8912b.c $P/
cp $H/files/arch/arm64/boot/dts/exynos/board/erd/s5e3830-e850-hdmi.dtsi $K/arch/arm64/boot/dts/exynos/board/erd/
# build it into dpu.ko
grep -q panel-lontium-lt8912b $K/drivers/video/fbdev/exynos/dpu30/Makefile ||
    sed -i 's|\./panels/panel-samsung-td4150.o|./panels/panel-samsung-td4150.o\\\n\t./panels/panel-lontium-lt8912b.o|' \
        $K/drivers/video/fbdev/exynos/dpu30/Makefile
grep -q panel_lt8912b_ops $P/exynos_panel_drv.h ||
    sed -i 's|^extern struct exynos_panel_ops panel_td4150_ops;|&\nextern struct exynos_panel_ops panel_lt8912b_ops;|' $P/exynos_panel_drv.h
grep -q panel_lt8912b_ops $P/exynos_panel_drv.c ||
    sed -i 's|^\tpanel_list\[1\] = &panel_td4150_ops;|&\n\tpanel_list[2] = \&panel_lt8912b_ops;|' $P/exynos_panel_drv.c
# LK leaves DSIM running, so an underrun irq can arrive before dsim_register_panel() has set
# dsim->panel: guard the dereference in dsim_irq_handler (it oopsed at probe otherwise)
F=$K/drivers/video/fbdev/exynos/dpu30/dsim_drv.c
grep -q 'if (dsim->panel && dsim->panel->lcd_info.mode == DECON_VIDEO_MODE)' $F ||
    sed -i 's|^\t\tif (dsim->panel->lcd_info.mode == DECON_VIDEO_MODE) {|\t\tif (dsim->panel \&\& dsim->panel->lcd_info.mode == DECON_VIDEO_MODE) {|' $F
grep -n 'if (dsim->panel && dsim->panel->lcd_info.mode == DECON_VIDEO_MODE)' $F
# include the DT
D=$K/arch/arm64/boot/dts/exynos/board/erd/s5e3830-erd3830.dts
grep -q s5e3830-e850-hdmi.dtsi $D ||
    sed -i 's|^#include "camera/s5e3830-e850-ov5647-camera.dtsi"|&\n#include "s5e3830-e850-hdmi.dtsi"|' $D
grep -n "lt8912b" $K/drivers/video/fbdev/exynos/dpu30/Makefile $P/exynos_panel_drv.h $P/exynos_panel_drv.c
grep -n "e850-hdmi" $D
