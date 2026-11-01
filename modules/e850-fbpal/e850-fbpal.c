// SPDX-License-Identifier: GPL-2.0
/*
 * Load the VT palette (vt.default_red/grn/blu, white text from the boot command line) into
 * fb0's pseudo-palette. The DPU driver's decon_setcolreg() drops palette writes while DECON
 * is off or in hibernation, which is the case when fbcon sets the palette at boot, so the
 * HDMI text console drew white as dark blue (0x0000aa). The palette is in ANSI order; fbcon's
 * pseudo-palette is indexed in VGA order (1 = blue, 4 = red), as fbcon_set_palette() maps it.
 * The driver also reports fix.visual = MONO01 at boot, so fbcon binds in mono mode and every
 * colour comes out as index 1: set TRUECOLOR here, then rebind fbcon (e850-hdmi.service).
 */
#include <linux/module.h>
#include <linux/fb.h>

extern unsigned char default_red[], default_grn[], default_blu[];

static const unsigned char vga_to_ansi[16] = { 0, 4, 2, 6, 1, 5, 3, 7, 8, 12, 10, 14, 9, 13, 11, 15 };

static u32 field(unsigned char c, const struct fb_bitfield *bf)
{
	return ((u32)c >> (8 - bf->length)) << bf->offset;
}

static int __init e850_fbpal_init(void)
{
	struct fb_info *info = registered_fb[0];
	u32 *pal;
	int i;

	if (!info || !info->pseudo_palette || info->var.bits_per_pixel != 32 ||
	    info->var.red.length != 8)
		return -ENODEV;
	/* the DPU driver reports MONO01 until something sets a mode; fbcon then binds as mono */
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	pal = info->pseudo_palette;
	for (i = 0; i < 16; i++) {
		int a = vga_to_ansi[i];

		pal[i] = field(default_red[a], &info->var.red) |
			 field(default_grn[a], &info->var.green) |
			 field(default_blu[a], &info->var.blue);
	}
	pr_info("e850-fbpal: fb0 palette loaded (white = %08x)\n", pal[7]);
	return 0;
}

static void __exit e850_fbpal_exit(void) {}

module_init(e850_fbpal_init);
module_exit(e850_fbpal_exit);
MODULE_LICENSE("GPL");
