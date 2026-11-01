// SPDX-License-Identifier: GPL-2.0
/*
 * CPU hotplug PMU writes for Exynos850, as in the mainline series "soc: samsung: exynos-pmu:
 * add Exynos850 CPU hotplug support" (Alexey Klimov, Linaro, 2026). The vendor 5.10
 * exynos-cpupm leaves CLUSTERx_CPUy_INT_EN bit 3 alone (those lines are commented out in
 * flexpmu_cal_cpu_s5e3830.h) and re-enables the core only once it already runs, so a core
 * that went through PSCI CPU_OFF never re-enters the kernel (docs 11.9).
 *   offline (on the dying CPU, before CPU_OFF): INFORM = C2, mask its GRP2 wake-up,
 *                                               clear GRP1 pending, INT_EN bit 3 = 1
 *   online  (on the boot CPU, before CPU_ON):   INFORM = 0, unmask GRP2, clear GRP2 pending,
 *                                               INT_EN bit 3 = 0
 */
#include <linux/module.h>
#include <linux/cpuhotplug.h>
#include <linux/io.h>
#include <linux/spinlock.h>

#define PMU_ALIVE		0x11860000
#define PMU_INTR_GEN		0x11870000
#define CPU_INFORM(cpu)		(0x0860 + (cpu) * 4)
#define CPU_INT_EN(cpu)		(0x1000 + ((cpu) / 4) * 0x400 + ((cpu) % 4) * 0x80 + 0x44)
#define CPU_STATUS(cpu)		(0x1000 + ((cpu) / 4) * 0x400 + ((cpu) % 4) * 0x80 + 0x04)
#define GRP1_UPEND		0x108
#define GRP1_CLEAR		0x10c
#define GRP2_ENABLE		0x200
#define GRP2_UPEND		0x208
#define GRP2_CLEAR		0x20c
#define INFORM_C2		1

static void __iomem *alive, *intr;
static DEFINE_RAW_SPINLOCK(lock);
static int st_prepare, st_online;

static void dump(const char *when, unsigned int cpu)
{
	pr_info("e850-cpuhp: cpu%u %s: inform %x int_en %x status %x grp2_en %x\n", cpu, when,
		readl(alive + CPU_INFORM(cpu)), readl(alive + CPU_INT_EN(cpu)),
		readl(alive + CPU_STATUS(cpu)), readl(intr + GRP2_ENABLE));
}

static int e850_cpu_offline(unsigned int cpu)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&lock, flags);
	writel(INFORM_C2, alive + CPU_INFORM(cpu));
	writel(readl(intr + GRP2_ENABLE) | BIT(cpu), intr + GRP2_ENABLE);
	writel(readl(intr + GRP1_UPEND) & BIT(cpu), intr + GRP1_CLEAR);
	writel(readl(intr + GRP1_UPEND) & BIT(cpu + 8), intr + GRP1_CLEAR);
	writel(readl(alive + CPU_INT_EN(cpu)) | BIT(3), alive + CPU_INT_EN(cpu));
	raw_spin_unlock_irqrestore(&lock, flags);
	dump("offline", cpu);
	return 0;
}

static int e850_cpu_prepare(unsigned int cpu)
{
	unsigned long flags;

	dump("before on", cpu);
	raw_spin_lock_irqsave(&lock, flags);
	writel(0, alive + CPU_INFORM(cpu));
	writel(readl(intr + GRP2_ENABLE) & ~BIT(cpu), intr + GRP2_ENABLE);
	writel(readl(intr + GRP2_UPEND) & BIT(cpu), intr + GRP2_CLEAR);
	writel(readl(alive + CPU_INT_EN(cpu)) & ~BIT(3), alive + CPU_INT_EN(cpu));
	raw_spin_unlock_irqrestore(&lock, flags);
	return 0;
}

static int __init e850_cpuhp_init(void)
{
	unsigned int cpu;

	alive = ioremap(PMU_ALIVE, 0x2000);
	intr = ioremap(PMU_INTR_GEN, 0x1000);
	if (!alive || !intr)
		return -ENOMEM;
	for_each_possible_cpu(cpu)
		dump("load", cpu);
	st_prepare = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN, "e850-cpuhp:prepare",
					       e850_cpu_prepare, NULL);
	st_online = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "e850-cpuhp:online",
					      NULL, e850_cpu_offline);
	pr_info("e850-cpuhp: states %d %d\n", st_prepare, st_online);
	return 0;
}

static void __exit e850_cpuhp_exit(void)
{
	if (st_online > 0)
		cpuhp_remove_state_nocalls(st_online);
	if (st_prepare > 0)
		cpuhp_remove_state_nocalls(st_prepare);
	iounmap(intr);
	iounmap(alive);
}

module_init(e850_cpuhp_init);
module_exit(e850_cpuhp_exit);
MODULE_LICENSE("GPL");
