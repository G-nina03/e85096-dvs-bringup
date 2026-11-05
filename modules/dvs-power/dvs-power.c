// SPDX-License-Identifier: GPL-2.0
/*
 * Power the DVS (S5KRC1S) on CN4 for I2C tests, without any camera driver:
 * load = CAM_VDD_33 (vdd_ldo32) on, RSTN (CN4 pin 12 = GPC0[2]) low -> high.
 * unload = RSTN low, vdd_ldo32 off. MCLK is not driven (the sensor uses its internal clock).
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>

static unsigned int rst_gpio = 110;	/* gpiochip26 (gpc0) base 108 + 2 */
module_param(rst_gpio, uint, 0444);
static unsigned int rst_ms = 10;
module_param(rst_ms, uint, 0444);

static struct regulator *vdd;

static int __init dvs_power_init(void)
{
	int ret;

	vdd = regulator_get(NULL, "vdd_ldo32");
	if (IS_ERR(vdd))
		return PTR_ERR(vdd);
	ret = gpio_request_one(rst_gpio, GPIOF_OUT_INIT_LOW, "dvs-rstn");
	if (ret)
		goto put;
	ret = regulator_enable(vdd);
	if (ret)
		goto free;
	msleep(rst_ms);
	gpio_set_value(rst_gpio, 1);
	msleep(rst_ms);
	pr_info("dvs-power: vdd_ldo32 on (%d uV), rstn gpio %u high\n",
		regulator_get_voltage(vdd), rst_gpio);
	return 0;
free:
	gpio_free(rst_gpio);
put:
	regulator_put(vdd);
	return ret;
}

static void __exit dvs_power_exit(void)
{
	gpio_set_value(rst_gpio, 0);
	regulator_disable(vdd);
	gpio_free(rst_gpio);
	regulator_put(vdd);
	pr_info("dvs-power: rstn low, vdd_ldo32 off\n");
}

module_init(dvs_power_init);
module_exit(dvs_power_exit);
MODULE_LICENSE("GPL");
