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
# fbcon (text console on HDMI) draws through fb_ops fillrect/copyarea/imageblit, which the
# Android DECON driver leaves NULL: "CFI failure (target: 0x0)" in fbcon_cursor. Use the
# generic cfb_* helpers (CONFIG_FB_CFB_FILLRECT/COPYAREA/IMAGEBLIT); DECON vmaps its buffer
# into fbi->screen_base, which is what they draw into.
F=$K/drivers/video/fbdev/exynos/dpu30/decon_core.c
grep -q 'cfb_imageblit' $F ||
    sed -i 's|^\t\.fb_mmap\t= decon_mmap,|&\n\t.fb_fillrect\t= cfb_fillrect,\n\t.fb_copyarea\t= cfb_copyarea,\n\t.fb_imageblit\t= cfb_imageblit,|' $F
grep -n 'cfb_' $F
# decon_fb_alloc_memory() vmaps the fb buffer, clears it, then vunmaps it and sets
# fbi->screen_base = NULL (Android draws only through mmap). fbcon draws from the kernel:
# keep the mapping (the buffer is from the "system-uncached" heap, so no cache maintenance)
sed -i '/^static int decon_fb_alloc_memory(/,/^#if defined(CONFIG_FB_TEST)/ {
    /^\tdma_buf_vunmap(buf, vaddr);$/d
    /^\tfbi->screen_base = NULL;$/d
}' $F
sed -n '/^static int decon_fb_alloc_memory(/,/^#if defined(CONFIG_FB_TEST)/p' $F | grep -n 'screen_base\|vunmap'
# decon_release() runs on every close of fb0 (and on fbcon unbind) and turns DECON/DSIM off
# each time, even while fb-hold.py or fbcon still use it: count the opens, disable on the last
grep -q 'decon_fb_users' $F || {
    sed -i 's|^int decon_release(struct fb_info \*info, int user)$|/* fb_release runs on every close: count opens, disable DECON only on the last one */\nstatic atomic_t decon_fb_users = ATOMIC_INIT(0);\n\nstatic int decon_open(struct fb_info *info, int user)\n{\n\tatomic_inc(\&decon_fb_users);\n\treturn 0;\n}\n\n&|' $F
    sed -i '/^int decon_release(struct fb_info \*info, int user)$/,/^}$/ s|^\tdecon_info("%s + : %d\\n", __func__, decon->id);$|&\n\tif (atomic_dec_return(\&decon_fb_users) > 0)\n\t\treturn 0;|' $F
    sed -i 's|^\t\.fb_release\t= decon_release,|\t.fb_open\t= decon_open,\n&|' $F
}
grep -n 'decon_fb_users\|fb_open' $F
# fbcon colours: the fb reported MONO01 until decon_set_par() ran (it returns early while DECON
# is off, which it is when fbcon binds), so fbcon bound in mono; and decon_setcolreg() dropped
# palette writes while DECON was off. The pseudo-palette is memory only: set TRUECOLOR at window
# create and always store it (replaces modules/e850-fbpal + fbcon rebind). Drop its debug printk.
grep -q 'fbinfo->fix.visual = FB_VISUAL_TRUECOLOR;' $F ||
    sed -i 's|^\tfbinfo->pseudo_palette  = &win->pseudo_palette;$|&\n\tfbinfo->fix.visual = FB_VISUAL_TRUECOLOR;|' $F
grep -n 'fbinfo->fix.visual = FB_VISUAL_TRUECOLOR;' $F
F=$K/drivers/video/fbdev/exynos/dpu30/decon_dsi.c
sed -i '/^int decon_setcolreg(unsigned regno,$/,/^EXPORT_SYMBOL(decon_setcolreg);$/ {
    /^\tprintk("@@@@ %s\\n", __func__);$/d
    /^\tif (IS_DECON_OFF_STATE(decon))$/ { N; N; /\n\t\treturn 0;\n$/d; }
    /^\tstruct decon_device \*decon = win->decon;$/d
}' $F
sed -n '/^int decon_setcolreg(unsigned regno,$/,/^EXPORT_SYMBOL(decon_setcolreg);$/p' $F | grep -n '@@@@\|IS_DECON_OFF_STATE' || echo 'decon_setcolreg: always stores the pseudo-palette'
# FB_CFB_* are select-only symbols (olddefconfig drops them when set by hand): select them here
KC=$K/drivers/video/fbdev/exynos/dpu30/Kconfig
grep -q 'select FB_CFB_IMAGEBLIT' $KC ||
    sed -i '/^config EXYNOS_DPU30$/,/^config / s|^\tdepends on \(.*\)$|\tdepends on \1\n\tselect FB_CFB_FILLRECT\n\tselect FB_CFB_COPYAREA\n\tselect FB_CFB_IMAGEBLIT|' $KC
grep -n -A6 '^config EXYNOS_DPU30$' $KC
# include the DT
D=$K/arch/arm64/boot/dts/exynos/board/erd/s5e3830-erd3830.dts
grep -q s5e3830-e850-hdmi.dtsi $D ||
    sed -i 's|^#include "camera/s5e3830-e850-ov5647-camera.dtsi"|&\n#include "s5e3830-e850-hdmi.dtsi"|' $D
grep -n "lt8912b" $K/drivers/video/fbdev/exynos/dpu30/Makefile $P/exynos_panel_drv.h $P/exynos_panel_drv.c
grep -n "e850-hdmi" $D
