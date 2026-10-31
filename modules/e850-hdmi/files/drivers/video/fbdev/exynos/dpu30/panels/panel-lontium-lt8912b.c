// SPDX-License-Identifier: GPL-2.0
/*
 * E850-96 HDMI: "panel" entry for the Lontium LT8912B MIPI-DSI -> HDMI bridge.
 *
 * The E850-96 has no DSI panel; DSIM0 (4 lanes, video mode) feeds an LT8912B
 * on i2c@13840000 (0x48/0x49), and GPG1[2] switches the MIPI lanes to it.
 * The DPU30 panel framework needs an exynos_panel_ops entry whose id matches
 * panel_0's ddi_id, otherwise exynos_panel_register_ops() walks NULL entries
 * of panel_list. LK detects "panel" 0xff on this board and writes that into
 * panel_0's ddi_id before Linux starts, so 0xff is the id that has to match.
 * A bridge takes no DSI commands, so every op is a no-op; the
 * bridge itself is programmed over I2C from user space once DSI is running
 * (tools/linux510/lt8912b-hdmi.sh), which keeps the sequence easy to change.
 */
#include "exynos_panel_drv.h"

static int lt8912b_nop(struct exynos_panel_device *panel)
{
	return 0;
}

static int lt8912b_mres(struct exynos_panel_device *panel, int mres_idx)
{
	return 0;
}

static int lt8912b_cabc_mode(struct exynos_panel_device *panel, int mode)
{
	return 0;
}

struct exynos_panel_ops panel_lt8912b_ops = {
	.id		= {0x000000ff, 0x008912b0, 0xffffff},	/* 0xff: the id LK writes (see DT) */
	.suspend	= lt8912b_nop,
	.displayon	= lt8912b_nop,
	.mres		= lt8912b_mres,
	.doze		= lt8912b_nop,
	.doze_suspend	= lt8912b_nop,
	.dump		= lt8912b_nop,
	.read_state	= lt8912b_nop,
	.set_cabc_mode	= lt8912b_cabc_mode,
};
