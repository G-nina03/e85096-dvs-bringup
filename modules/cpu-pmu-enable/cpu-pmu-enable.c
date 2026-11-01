// SPDX-License-Identifier: GPL-2.0
/*
 * Unmask the flexpmu wake-up of CPUs that were offline when exynos-cpupm probed.
 * exynos-cpupm only calls cal_cpu_enable() for CPUs online at probe (maxcpus=1 -> CPU0),
 * so with this loaded, `echo 1 > /sys/devices/system/cpu/cpuN/online` can power CPUN.
 */
#include <linux/module.h>
#include <linux/cpumask.h>
#include <soc/samsung/cal-if.h>

static unsigned int mask = 0xfc;	/* CPU2-7 */
module_param(mask, uint, 0444);

static int __init cpu_pmu_enable_init(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		if (!(mask & BIT(cpu)) || cpu_online(cpu))
			continue;
		pr_info("cpu-pmu-enable: cpu%u status %d", cpu, cal_cpu_status(cpu));
		pr_cont(" -> enable %d", cal_cpu_enable(cpu));
		pr_cont(" -> status %d\n", cal_cpu_status(cpu));
	}
	return 0;
}

static void __exit cpu_pmu_enable_exit(void) {}

module_init(cpu_pmu_enable_init);
module_exit(cpu_pmu_enable_exit);
MODULE_LICENSE("GPL");
