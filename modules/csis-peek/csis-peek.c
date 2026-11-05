// SPDX-License-Identifier: GPL-2.0
/*
 * One-shot CSIS register dump (no /dev/mem on this kernel). Load only while the CSIS is
 * streaming: its clock is gated otherwise and the read can hang the bus. init prints and
 * returns -EAGAIN, so nothing stays loaded.
 *   insmod csis-peek.ko [base=0x14410000]   (CSIS1 = CN4)
 * PHY_STATUS(0x700): [0] STOPSTATECLK, [1] ULPSCLK, [7:4] STOPSTATEDAT, [11:8] ULPSDAT
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/delay.h>

static ulong base = 0x14410000;
module_param(base, ulong, 0444);

static const u16 regs[] = { 0x0, 0x4, 0x8, 0x10, 0x14, 0x18, 0x1c, 0x40, 0x44, 0x500, 0x700, 0x704 };

static int __init csis_peek_init(void)
{
	void __iomem *r = ioremap(base, 0x1000);
	u32 and = ~0u, or = 0, v;
	int i;

	if (!r)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		pr_info("csis-peek %lx+%03x = %08x\n", base, regs[i], readl(r + regs[i]));
	/* PHY_STATUS over ~10 ms: bits that ever cleared (and) or ever set (or) */
	for (i = 0; i < 10000; i++) {
		v = readl(r + 0x700);
		and &= v;
		or |= v;
		ndelay(1000);
	}
	pr_info("csis-peek PHY_STATUS and=%08x or=%08x\n", and, or);
	iounmap(r);
	return -EAGAIN;
}

module_init(csis_peek_init);
MODULE_LICENSE("GPL");
