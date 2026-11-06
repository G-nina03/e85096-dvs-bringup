/*
 * Samsung S5KRC1S DVS CIS bring-up driver for Exynos Pablo
 *
 * Minimal first-stage implementation:
 *  - CCI/I2C probe
 *  - known transport registers
 *  - 2-lane selection
 *  - standby / stream control via 0x3000
 *
 * Conventional exposure/gain controls are intentionally not implemented.
 */

#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <media/v4l2-subdev.h>

#include <exynos-is-sensor.h>
#include "is-core.h"
#include "is-device-sensor.h"
#include "is-device-sensor-peri.h"
#include "is-cis-s5krc1s.h"
#include "is-cis-s5krc1s-setA.h"
#include "is-helper-i2c.h"

#define SENSOR_NAME "S5KRC1S"

static const struct v4l2_subdev_ops subdev_ops;

/*
 * Applied at each mode change, after the setfile; 0 = setfile value. In a sparse scene the sensor sends
 * one packet per flush timeout, so a MIPI frame (= one VC0 buffer) takes packets x flush_us (11.25).
 * CSIS takes the short frame: the VRESOL check is off (csis-v1_2-err-clear.patch).
 */
static uint packets;
module_param(packets, uint, 0644);
MODULE_PARM_DESC(packets, "MIPI_NUM_OF_PACKETS 0x391A/B per frame, 1..127; CSIS needs it = the DT mode height (11.25)");

/* MIPI packets per frame = VC0 lines of each DT mode (module_s5krc1s.dtsi mode0, mode1); one setfile for all */
static const u16 sensor_s5krc1s_mode_lines[] = { S5KRC1S_BRINGUP_PACKET_COUNT, 32 };
static uint flush_us;
module_param(flush_us, uint, 0644);
MODULE_PARM_DESC(flush_us, "MIPI FIFO flush timeout 0x3912/13 in us (fine count 0x3911 = 1 us), 1..1023");

static int sensor_s5krc1s_write8(struct is_cis *cis, u16 reg, u8 value)
{
	if (!cis || !cis->client)
		return -EINVAL;

	return is_sensor_write8(cis->client, reg, value);
}

static int sensor_s5krc1s_read8(struct is_cis *cis, u16 reg, u8 *value)
{
	if (!cis || !cis->client || !value)
		return -EINVAL;

	return is_sensor_read8(cis->client, reg, value);
}

static int sensor_s5krc1s_cis_init(struct v4l2_subdev *subdev)
{
	struct is_cis *cis;
	u8 mode;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	memset(cis->cis_data, 0, sizeof(cis_shared_data));

	cis->cis_data->cur_width = SENSOR_S5KRC1S_MAX_WIDTH;
	cis->cis_data->cur_height = SENSOR_S5KRC1S_MAX_HEIGHT;
	cis->cis_data->max_fps = 30;
	cis->cis_data->stream_on = false;
	cis->need_mode_change = false;

	/*
	 * The manual has no chip ID register; a SEL_MODE read only shows the sensor answers.
	 * As in the OV5647 driver, only log: the power sequence may not have run yet.
	 */
	if (sensor_s5krc1s_read8(cis, S5KRC1S_REG_SEL_MODE, &mode))
		err("%s: no I2C response at 0x%02x\n", __func__, cis->client->addr);
	else
		probe_info("%s: S5KRC1S 960x720 DVS, SEL_MODE(0x3000)=0x%02x\n", __func__, mode);
	return 0;
}

static int sensor_s5krc1s_cis_log_status(struct v4l2_subdev *subdev)
{
	struct is_cis *cis;
	u8 val = 0;
	int ret = 0;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);

	if (!sensor_s5krc1s_read8(cis, S5KRC1S_REG_CCI_ADDRESS, &val))
		pr_info("[S5KRC1S] CCI_ADDRESS(0x3024)=0x%02x\n", val);

	if (!sensor_s5krc1s_read8(cis, S5KRC1S_REG_SEL_MODE, &val))
		pr_info("[S5KRC1S] SEL_MODE(0x3000)=0x%02x\n", val);

	if (!sensor_s5krc1s_read8(cis, S5KRC1S_REG_CSI2_DI, &val))
		pr_info("[S5KRC1S] CSI2_DI(0x3901)=0x%02x\n", val);

	if (!sensor_s5krc1s_read8(cis, S5KRC1S_REG_CSI2_WC_LOW, &val))
		pr_info("[S5KRC1S] WC_LOW(0x3902)=0x%02x\n", val);

	if (!sensor_s5krc1s_read8(cis, S5KRC1S_REG_CSI2_WC_HIGH, &val))
		pr_info("[S5KRC1S] WC_HIGH(0x3903)=0x%02x\n", val);

	if (!sensor_s5krc1s_read8(cis, S5KRC1S_REG_CSI2_EMBD_DI, &val))
		pr_info("[S5KRC1S] EMBD_DI(0x3906)=0x%02x\n", val);

	if (!sensor_s5krc1s_read8(cis, S5KRC1S_REG_MIPI_TIMEOUT, &val))
		pr_info("[S5KRC1S] MIPI_TIMEOUT(0x3910)=0x%02x\n", val);

	ret = sensor_s5krc1s_read8(cis, S5KRC1S_REG_MIPI_MODE, &val);
	if (!ret)
		pr_info("[S5KRC1S] MIPI_MODE(0x3915)=0x%02x\n", val);

	if (!sensor_s5krc1s_read8(cis, S5KRC1S_REG_PACKET_NUM_LOW, &val))
		pr_info("[S5KRC1S] PACKET_NUM_LOW(0x391A)=0x%02x\n", val);

	if (!sensor_s5krc1s_read8(cis, S5KRC1S_REG_PACKET_NUM_HIGH, &val))
		pr_info("[S5KRC1S] PACKET_NUM_HIGH(0x391B)=0x%02x\n", val);

	if (!sensor_s5krc1s_read8(cis, S5KRC1S_REG_GROUP_EVENT, &val))
		pr_info("[S5KRC1S] GROUP_EVENT(0x392C)=0x%02x\n", val);

	return ret;
}

static int sensor_s5krc1s_cis_set_global_setting(struct v4l2_subdev *subdev)
{
	int ret;

	FIMC_BUG(!subdev);

	/* the RPi5 init tool waits 0.5 s after RSTN high before writing the setfile */
	msleep(500);

	ret = sensor_cis_set_registers(
		subdev,
		sensor_s5krc1s_setfile_A_Global,
		ARRAY_SIZE(sensor_s5krc1s_setfile_A_Global));
	if (ret < 0)
		err("%s: global setfile write failed (%d)\n", __func__, ret);
	else
		probe_info("%s: Delta_10_2000FPS written\n", __func__);

	return ret;
}

static int sensor_s5krc1s_set_two_lane(struct is_cis *cis)
{
	u8 value;
	int ret;

	ret = sensor_s5krc1s_read8(cis, S5KRC1S_REG_MIPI_MODE, &value);
	if (ret < 0)
		return ret;

	/*
	 * 0x3915: [5:4] mipi_ck_mode, [3] SOL/EOL, [2] SOF/EOF, [1] 2-lane, [0] 1-lane.
	 * Only the lane bits change: the setfile's 0x24 (clock always on, SOF/EOF) -> 0x26.
	 */
	value &= ~0x03;
	value |= 0x02;

	ret = sensor_s5krc1s_write8(cis, S5KRC1S_REG_MIPI_MODE, value);
	if (!ret)
		probe_info("%s: 0x3915=0x%02x (2-lane)\n", __func__, value);

	return ret;
}

static int sensor_s5krc1s_cis_mode_change(struct v4l2_subdev *subdev, u32 mode)
{
	struct is_cis *cis;
	int ret;
	u32 lines;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	if (mode >= ARRAY_SIZE(sensor_s5krc1s_mode_lines))
		return -EINVAL;

	ret = sensor_s5krc1s_write8(cis,
				    S5KRC1S_REG_SEL_MODE,
				    S5KRC1S_MODE_STANDBY);
	if (ret < 0)
		return ret;

	ret = sensor_cis_set_registers(
		subdev,
		sensor_s5krc1s_setfiles_A[0],
		sensor_s5krc1s_setfile_A_sizes[0]);
	if (ret < 0)
		return ret;

	ret = sensor_s5krc1s_set_two_lane(cis);
	if (ret < 0)
		return ret;

	lines = packets >= 1 && packets <= S5KRC1S_BRINGUP_PACKET_COUNT ? packets : sensor_s5krc1s_mode_lines[mode];
	ret = sensor_s5krc1s_write8(cis, 0x391A, lines & 0xff);
	ret = ret ?: sensor_s5krc1s_write8(cis, 0x391B, lines >> 8);
	if (ret < 0)
		return ret;
	if (flush_us >= 1 && flush_us <= 1023) {
		ret = sensor_s5krc1s_write8(cis, 0x3912, flush_us & 0xff);
		ret = ret ?: sensor_s5krc1s_write8(cis, 0x3913, flush_us >> 8);
		if (ret < 0)
			return ret;
	}

	/* Bring-up readback: verify programmed CSI-2 transport registers. */
	sensor_s5krc1s_cis_log_status(subdev);

	cis->cis_data->cur_width = 8192;
	cis->cis_data->cur_height = sensor_s5krc1s_mode_lines[mode];

	probe_info("%s: mode %u, RAW8 8192x%u transport, %u packets\n",
		   __func__, mode, sensor_s5krc1s_mode_lines[mode], lines);

	return 0;
}

static int sensor_s5krc1s_cis_stream_on(struct v4l2_subdev *subdev)
{
	struct is_cis *cis;
	int ret;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	ret = sensor_s5krc1s_write8(cis,
				    S5KRC1S_REG_SEL_MODE,
				    S5KRC1S_MODE_STREAM);
	if (ret < 0)
		return ret;

	cis->cis_data->stream_on = true;
	probe_info("%s: stream ON (0x3000=0x02)\n", __func__);

	/* Bring-up readback: confirm stream state after 0x3000 write. */
	sensor_s5krc1s_cis_log_status(subdev);

	return 0;
}

static int sensor_s5krc1s_cis_stream_off(struct v4l2_subdev *subdev)
{
	struct is_cis *cis;
	int ret;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	ret = sensor_s5krc1s_write8(cis,
				    S5KRC1S_REG_SEL_MODE,
				    S5KRC1S_MODE_STANDBY);
	if (ret < 0)
		return ret;

	cis->cis_data->stream_on = false;
	probe_info("%s: stream OFF (0x3000=0x00)\n", __func__);

	return 0;
}

static struct is_cis_ops cis_ops = {
	.cis_init = sensor_s5krc1s_cis_init,
	.cis_log_status = sensor_s5krc1s_cis_log_status,
	.cis_set_global_setting = sensor_s5krc1s_cis_set_global_setting,
	.cis_mode_change = sensor_s5krc1s_cis_mode_change,
	.cis_stream_on = sensor_s5krc1s_cis_stream_on,
	.cis_stream_off = sensor_s5krc1s_cis_stream_off,
};

/* Not __init: as a module, probe can run again after -EPROBE_DEFER. */
static int cis_s5krc1s_probe(struct i2c_client *client,
				    const struct i2c_device_id *id)
{
	struct is_core *core;
	struct is_device_sensor *device;
	struct is_device_sensor_peri *sensor_peri;
	struct v4l2_subdev *subdev_cis;
	struct is_cis *cis;
	struct device_node *dnode;
	u32 sensor_instance;
	const char *setfile = NULL;
	int ret;

	FIMC_BUG(!client);
	FIMC_BUG(!is_dev);

	core = (struct is_core *)dev_get_drvdata(is_dev);
	if (!core) {
		probe_info("%s: core is not yet probed\n", __func__);
		return -EPROBE_DEFER;
	}

	dnode = client->dev.of_node;

	ret = of_property_read_u32(dnode, "id", &sensor_instance);
	if (ret) {
		err("%s: id property read failed (%d)\n", __func__, ret);
		return ret;
	}

	device = &core->sensor[sensor_instance];

	sensor_peri = find_peri_by_cis_id(device, SENSOR_NAME_S5KRC1S);
	if (!sensor_peri) {
		probe_info("%s: sensor peri is not yet probed\n", __func__);
		return -EPROBE_DEFER;
	}

	cis = &sensor_peri->cis;

	subdev_cis = kzalloc(sizeof(struct v4l2_subdev), GFP_KERNEL);
	if (!subdev_cis)
		return -ENOMEM;

	sensor_peri->subdev_cis = subdev_cis;

	cis->id = SENSOR_NAME_S5KRC1S;
	cis->subdev = subdev_cis;
	cis->device = sensor_instance;
	cis->client = client;
	sensor_peri->module->client = client;
	cis->ctrl_delay = N_PLUS_ONE_FRAME;

	cis->cis_data = kzalloc(sizeof(cis_shared_data), GFP_KERNEL);
	if (!cis->cis_data) {
		kfree(subdev_cis);
		return -ENOMEM;
	}

	cis->cis_ops = &cis_ops;

	/* Framework ABI field only; S5KRC1S DVS data is not Bayer. */
	cis->bayer_order = OTF_INPUT_ORDER_BAYER_GR_BG;
	cis->use_dgain = false;
	cis->hdr_ctrl_by_again = false;

	if (!of_property_read_string(dnode, "setfile", &setfile))
		probe_info("%s: setfile=%s\n", __func__, setfile);

	v4l2_i2c_subdev_init(subdev_cis, client, &subdev_ops);
	v4l2_set_subdevdata(subdev_cis, cis);
	v4l2_set_subdev_hostdata(subdev_cis, device);
	snprintf(subdev_cis->name, V4L2_SUBDEV_NAME_SIZE,
		 "cis-subdev.%d", cis->id);

	probe_info("%s: S5KRC1S bound to sensor instance %u, addr=0x%02x\n",
		   __func__, sensor_instance, client->addr);

	return 0;
}

static const struct of_device_id sensor_cis_s5krc1s_match[] = {
	{
		.compatible = "samsung,exynos-is-cis-s5krc1s",
	},
	{},
};
MODULE_DEVICE_TABLE(of, sensor_cis_s5krc1s_match);

static const struct i2c_device_id sensor_cis_s5krc1s_idt[] = {
	{ SENSOR_NAME, 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, sensor_cis_s5krc1s_idt);

static struct i2c_driver sensor_cis_s5krc1s_driver = {
	.probe = cis_s5krc1s_probe,
	.driver = {
		.name = SENSOR_NAME,
		.owner = THIS_MODULE,
		.of_match_table = sensor_cis_s5krc1s_match,
		.suppress_bind_attrs = true,
	},
	.id_table = sensor_cis_s5krc1s_idt,
};

module_i2c_driver(sensor_cis_s5krc1s_driver);

MODULE_DESCRIPTION("Samsung S5KRC1S DVS CIS bring-up driver");
MODULE_LICENSE("GPL");
