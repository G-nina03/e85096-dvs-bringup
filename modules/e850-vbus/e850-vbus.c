// SPDX-License-Identifier: GPL-2.0
/*
 * e850-vbus: tell the Exynos DWC3 glue that VBUS is present.
 *
 * The vendor 5.10 kernel targets the ERD3830 reference board, where the
 * S2MU106 charger/PDIC reports cable attach through usb_notifier ->
 * dwc3_exynos_vbus_event(). The E850-96 has no S2MU106 (all its registers
 * read 0xff), so that event never comes: the controller stays runtime
 * suspended with clocks off (current_dr_role = 255), and touching a gadget
 * endpoint then hangs the bus. Loading this module sends "VBUS on" once;
 * unloading sends "VBUS off".
 *
 * The glue probe runs asynchronously and starts the OTG state machine
 * (dwc3_otg_start) before it binds the gadget to it (otg_set_peripheral).
 * VBUS in between ends in "dwc3_otg_start_gadget does not have any gadget",
 * nothing retries, and the ACM console never comes up (docs 11.31). So wait
 * for the gadget first.
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/usb/otg-fsm.h>
#include "core.h"
#include "dwc3-exynos.h"
#include "exynos-otg.h"

int dwc3_exynos_vbus_event(struct device *dev, bool vbus_active);

static struct platform_device *pdev;

static int __init e850_vbus_init(void)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "samsung,exynos-dwusb");
	int ret, ms;

	if (!np)
		return -ENODEV;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -EPROBE_DEFER;
	for (ms = 0; ms < 5000; ms += 10) {
		struct dwc3_exynos *exynos = dev_get_drvdata(&pdev->dev);
		struct dwc3_otg *dotg = exynos ? READ_ONCE(exynos->dotg) : NULL;

		if (dotg && READ_ONCE(dotg->otg.gadget))
			break;
		msleep(10);
	}
	ret = dwc3_exynos_vbus_event(&pdev->dev, true);
	pr_info("e850-vbus: VBUS on for %s after %d ms: %d\n", dev_name(&pdev->dev), ms, ret);
	if (ret)
		put_device(&pdev->dev);
	return ret;
}

static void __exit e850_vbus_exit(void)
{
	dwc3_exynos_vbus_event(&pdev->dev, false);
	put_device(&pdev->dev);
}

module_init(e850_vbus_init);
module_exit(e850_vbus_exit);
MODULE_DESCRIPTION("E850-96: report VBUS to the DWC3 glue (no S2MU106 PDIC on this board)");
MODULE_LICENSE("GPL");
