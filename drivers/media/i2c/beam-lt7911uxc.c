// SPDX-License-Identifier: GPL-2.0-only
/*
 * Beam LT7911UXC FW 1.a DP-to-CSI bridge for mainline V4L2.
 * Register protocol: Rockchip lt7911uxc.c, develop-6.12, and Beam captures.
 * No firmware flashing and no dependency on private RK camera ioctls.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/v4l2-dv-timings.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

struct beam_lt {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler controls;
	struct regulator_bulk_data supplies[4];
	struct gpio_desc *reset;
	struct typec_mux_dev *mode_switch;
	struct mutex lock;
	struct delayed_work poll;
	struct v4l2_dv_timings timings;
	struct v4l2_mbus_config_mipi_csi2 bus;
	s64 link_frequency;
	u32 htotal, vtotal, audio_rate;
	u8 raw_timings[16];
	bool signal, streaming;
};

/* Beam FW 1.a exposes runtime timing/stream registers without E0:EE writes,
 * as verified against the running 4.19 driver and direct register reads.
 * Keep gating available for comparisons; snapshot once per operation so
 * a parameter update cannot leave an open/close pair inconsistent.
 */
static bool runtime_i2c_gate;
module_param(runtime_i2c_gate, bool, 0644);
MODULE_PARM_DESC(runtime_i2c_gate, "Gate runtime timing/stream accesses through E0:EE (diagnostic, default off for Beam FW 1.a)");

static struct beam_lt *to_beam_lt(struct v4l2_subdev *sd)
{
	return container_of(sd, struct beam_lt, sd);
}

/* Bank selection and read are one I2C transfer, with real errors propagated. */
static int beam_lt_read(struct beam_lt *lt, u16 reg, u8 *data, unsigned int len)
{
	u8 bank[] = {0xff, reg >> 8};
	u8 address = reg;
	struct i2c_msg messages[] = {
		{.addr = lt->client->addr, .len = 2, .buf = bank},
		{.addr = lt->client->addr, .len = 1, .buf = &address},
		{.addr = lt->client->addr, .flags = I2C_M_RD, .len = len, .buf = data},
	};
	int ret = i2c_transfer(lt->client->adapter, messages, ARRAY_SIZE(messages));

	return ret == ARRAY_SIZE(messages) ? 0 : ret < 0 ? ret : -EIO;
}

static int beam_lt_write(struct beam_lt *lt, u16 reg, u8 value)
{
	u8 bank[] = {0xff, reg >> 8};
	u8 data[] = {reg & 0xff, value};
	struct i2c_msg messages[] = {
		{.addr = lt->client->addr, .len = 2, .buf = bank},
		{.addr = lt->client->addr, .len = 2, .buf = data},
	};
	int ret = i2c_transfer(lt->client->adapter, messages, ARRAY_SIZE(messages));

	return ret == ARRAY_SIZE(messages) ? 0 : ret < 0 ? ret : -EIO;
}

/* The running Beam 1.a driver drives receiver HPD through E0:B7. */
static int beam_lt_set_mode(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
	struct beam_lt *lt = typec_mux_get_drvdata(mux);
	bool enabled;
	int ret;

	switch (state->mode) {
	case TYPEC_STATE_SAFE:
	case TYPEC_STATE_USB:
		enabled = false;
		break;
	case TYPEC_DP_STATE_C:
	case TYPEC_DP_STATE_E:
		enabled = true;
		break;
	default:
		return -EOPNOTSUPP;
	}
	mutex_lock(&lt->lock);
	ret = beam_lt_write(lt, 0xe0b7, enabled);
	mutex_unlock(&lt->lock);
	if (ret)
		dev_err_ratelimited(&lt->client->dev, "receiver HPD failed: %d\n", ret);
	else
		dev_dbg(&lt->client->dev, "receiver HPD=%u mode=%lu\n", enabled, state->mode);
	return ret;
}

static int beam_lt_refresh(struct beam_lt *lt)
{
	u8 values[16];
	u32 width, height, htotal, vtotal, pixel_khz, byte_khz;
	int ret, close_ret;
	bool gated = READ_ONCE(runtime_i2c_gate);

	if (gated) {
		ret = beam_lt_write(lt, 0xe0ee, 1);
		if (ret)
			return ret;
	}
	ret = beam_lt_read(lt, 0xe085, values, sizeof(values));
	close_ret = gated ? beam_lt_write(lt, 0xe0ee, 0) : 0;
	if (ret || close_ret)
		return ret ?: close_ret;
	memcpy(lt->raw_timings, values, sizeof(values));
	pixel_khz = (values[0] << 16) | (values[1] << 8) | values[2];
	htotal = (values[3] << 8) | values[4];
	vtotal = (values[5] << 8) | values[6];
	width = (values[7] << 8) | values[8];
	height = (values[9] << 8) | values[10];
	byte_khz = (values[13] << 16) | (values[14] << 8) | values[15];
	lt->signal = width >= 64 && width <= 1920 && height >= 64 && height <= 1080 &&
		!(width & 1) && !(height & 1) && htotal >= width && vtotal >= height &&
		pixel_khz >= 1000 && byte_khz >= 1000;
	if (!lt->signal)
		return -ENOLINK;
	lt->timings.type = V4L2_DV_BT_656_1120;
	lt->timings.bt = (struct v4l2_bt_timings) {
		.width = width, .height = height, .pixelclock = (u64)pixel_khz * 1000,
		/* FW exposes aggregate blanking, not its individual sync phases. */
		.hfrontporch = htotal - width, .vfrontporch = vtotal - height,
	};
	lt->htotal = htotal;
	lt->vtotal = vtotal;
	lt->audio_rate = (values[11] << 8) | values[12];
	/* FW's byte-clock measurement is kHz; D-PHY clock is four times it. */
	lt->link_frequency = (s64)byte_khz * 4000;
	return 0;
}

static void beam_lt_poll(struct work_struct *work)
{
	struct beam_lt *lt = container_of(to_delayed_work(work), struct beam_lt, poll);
	struct v4l2_event event = {.type = V4L2_EVENT_SOURCE_CHANGE};
	u32 width, height;
	bool signal;
	int ret;

	mutex_lock(&lt->lock);
	width = lt->timings.bt.width;
	height = lt->timings.bt.height;
	signal = lt->signal;
	ret = beam_lt_refresh(lt);
	if (ret && ret != -ENOLINK) {
		lt->signal = false;
		dev_err_ratelimited(&lt->client->dev, "timing read failed: %d\n", ret);
	}
	if (signal != lt->signal || width != lt->timings.bt.width || height != lt->timings.bt.height) {
		event.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;
		v4l2_subdev_notify_event(&lt->sd, &event);
		dev_info(&lt->client->dev, "signal=%u %ux%u link=%lldHz\n", lt->signal,
			 lt->timings.bt.width, lt->timings.bt.height, lt->link_frequency);
	}
	mutex_unlock(&lt->lock);
	mod_delayed_work(system_freezable_wq, &lt->poll, msecs_to_jiffies(250));
}

static int beam_lt_get_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			    struct v4l2_subdev_format *format)
{
	struct beam_lt *lt = to_beam_lt(sd);

	if (format->pad)
		return -EINVAL;
	mutex_lock(&lt->lock);
	format->format = (struct v4l2_mbus_framefmt) {
		.code = MEDIA_BUS_FMT_UYVY8_1X16,
		.width = lt->timings.bt.width, .height = lt->timings.bt.height,
		.field = V4L2_FIELD_NONE, .colorspace = V4L2_COLORSPACE_REC709,
	};
	mutex_unlock(&lt->lock);
	return 0;
}

static int beam_lt_enum_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_UYVY8_1X16;
	return 0;
}

static int beam_lt_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
			       struct v4l2_mbus_config *config)
{
	struct beam_lt *lt = to_beam_lt(sd);

	if (pad)
		return -EINVAL;
	config->type = V4L2_MBUS_CSI2_DPHY;
	config->bus.mipi_csi2 = lt->bus;
	return 0;
}

static int beam_lt_query_timings(struct v4l2_subdev *sd, unsigned int pad,
				 struct v4l2_dv_timings *timings)
{
	struct beam_lt *lt = to_beam_lt(sd);
	int ret;

	if (pad)
		return -EINVAL;
	mutex_lock(&lt->lock);
	ret = beam_lt_refresh(lt);
	if (!ret)
		*timings = lt->timings;
	mutex_unlock(&lt->lock);
	return ret;
}

static int beam_lt_stream(struct v4l2_subdev *sd, int enable)
{
	struct beam_lt *lt = to_beam_lt(sd);
	int ret, close_ret;
	bool gated = READ_ONCE(runtime_i2c_gate);

	mutex_lock(&lt->lock);
	ret = gated ? beam_lt_write(lt, 0xe0ee, 1) : 0;
	if (!ret) {
		ret = beam_lt_write(lt, 0xe0b0, enable ? 1 : 0);
		close_ret = gated ? beam_lt_write(lt, 0xe0ee, 0) : 0;
		ret = ret ?: close_ret;
	}
	if (!ret)
		lt->streaming = !!enable;
	mutex_unlock(&lt->lock);
	return ret;
}

static int beam_lt_subscribe(struct v4l2_subdev *sd, struct v4l2_fh *fh,
			     struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_SOURCE_CHANGE)
		return v4l2_src_change_event_subdev_subscribe(sd, fh, sub);
	return v4l2_ctrl_subdev_subscribe_event(sd, fh, sub);
}

static const struct v4l2_subdev_core_ops beam_lt_core_ops = {
	.subscribe_event = beam_lt_subscribe,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};
static const struct v4l2_subdev_video_ops beam_lt_video_ops = {
	.s_stream = beam_lt_stream,
};
static const struct v4l2_subdev_pad_ops beam_lt_pad_ops = {
	.enum_mbus_code = beam_lt_enum_code,
	.get_fmt = beam_lt_get_fmt,
	.set_fmt = beam_lt_get_fmt,
	.get_mbus_config = beam_lt_mbus_config,
	.query_dv_timings = beam_lt_query_timings,
};
static const struct v4l2_subdev_ops beam_lt_ops = {
	.core = &beam_lt_core_ops, .video = &beam_lt_video_ops, .pad = &beam_lt_pad_ops,
};

static ssize_t beam_timings_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct beam_lt *lt = to_beam_lt(sd);
	ssize_t size;
	int ret;

	mutex_lock(&lt->lock);
	ret = beam_lt_refresh(lt);
	if (ret)
		size = ret;
	else
		size = sysfs_emit(buf, "{\"width\":%u,\"height\":%u,\"htotal\":%u,\"vtotal\":%u,"
				  "\"pixel_clock_khz\":%llu,\"link_frequency_hz\":%lld,\"audio_rate\":%u}\n",
				  lt->timings.bt.width, lt->timings.bt.height, lt->htotal, lt->vtotal,
				  lt->timings.bt.pixelclock / 1000, lt->link_frequency, lt->audio_rate);
	mutex_unlock(&lt->lock);
	return size;
}
static DEVICE_ATTR_RO(beam_timings);
static ssize_t beam_raw_timings_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct beam_lt *lt = to_beam_lt(sd);
	ssize_t size;
	int ret;

	mutex_lock(&lt->lock);
	ret = beam_lt_refresh(lt);
	size = ret && ret != -ENOLINK ? ret :
		sysfs_emit(buf, "status=%d raw=%*ph\n", ret, 16, lt->raw_timings);
	mutex_unlock(&lt->lock);
	return size;
}
static DEVICE_ATTR_RO(beam_raw_timings);
static struct attribute *beam_lt_attrs[] = {
	&dev_attr_beam_timings.attr, &dev_attr_beam_raw_timings.attr, NULL
};
static const struct attribute_group beam_lt_group = {.attrs = beam_lt_attrs};

static void beam_lt_power_off(void *data)
{
	struct beam_lt *lt = data;

	regulator_bulk_disable(ARRAY_SIZE(lt->supplies), lt->supplies);
}

static int beam_lt_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct v4l2_fwnode_endpoint endpoint = {.bus_type = V4L2_MBUS_CSI2_DPHY};
	struct fwnode_handle *ep;
	struct v4l2_ctrl *ctrl;
	struct beam_lt *lt;
	u8 id[2];
	int ret;

	lt = devm_kzalloc(dev, sizeof(*lt), GFP_KERNEL);
	if (!lt)
		return -ENOMEM;
	lt->client = client;
	mutex_init(&lt->lock);
	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0, 0);
	if (!ep)
		return -EINVAL;
	ret = v4l2_fwnode_endpoint_parse(ep, &endpoint);
	fwnode_handle_put(ep);
	if (ret)
		return ret;
	if (endpoint.bus.mipi_csi2.num_data_lanes != 4)
		return -EINVAL;
	lt->bus = endpoint.bus.mipi_csi2;
	lt->supplies[0].supply = "vdd11";
	lt->supplies[1].supply = "vdd33";
	/* Beam's CSI receiver analogue rails were owned by the vendor bridge. */
	lt->supplies[2].supply = "image-0v9";
	lt->supplies[3].supply = "image-1v8";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(lt->supplies), lt->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "LT7911UXC supplies\n");
	lt->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(lt->reset))
		return dev_err_probe(dev, PTR_ERR(lt->reset), "LT7911UXC reset\n");
	ret = regulator_bulk_enable(ARRAY_SIZE(lt->supplies), lt->supplies);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, beam_lt_power_off, lt);
	if (ret)
		return ret;
	msleep(100);
	/* Preserve the bootloader's running MCU when it already responds. */
	ret = beam_lt_write(lt, 0xe0ee, 1);
	if (!ret)
		ret = beam_lt_read(lt, 0xe100, id, sizeof(id));
	beam_lt_write(lt, 0xe0ee, 0);
	if (ret || id[0] != 0x19 || id[1] != 0x01) {
		if (lt->reset) {
			gpiod_direction_output(lt->reset, 1);
			msleep(25);
			gpiod_set_value_cansleep(lt->reset, 0);
			msleep(100);
		}
		ret = beam_lt_write(lt, 0xe0ee, 1);
		if (!ret)
			ret = beam_lt_read(lt, 0xe100, id, sizeof(id));
		beam_lt_write(lt, 0xe0ee, 0);
		if (ret)
			return dev_err_probe(dev, ret, "LT7911UXC chip ID read\n");
		if (id[0] != 0x19 || id[1] != 0x01)
			return dev_err_probe(dev, -ENODEV, "unexpected chip ID %02x%02x\n", id[1], id[0]);
	}
	lt->timings = (struct v4l2_dv_timings)V4L2_DV_BT_CEA_1920X1080P60;
	lt->link_frequency = 400000000;
	v4l2_i2c_subdev_init(&lt->sd, client, &beam_lt_ops);
	lt->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	lt->pad.flags = MEDIA_PAD_FL_SOURCE;
	lt->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	v4l2_ctrl_handler_init(&lt->controls, 1);
	lt->controls.lock = &lt->lock;
	ctrl = v4l2_ctrl_new_int_menu(&lt->controls, NULL, V4L2_CID_LINK_FREQ,
				     0, 0, &lt->link_frequency);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	if (lt->controls.error) {
		ret = lt->controls.error;
		goto free_controls;
	}
	lt->sd.ctrl_handler = &lt->controls;
	ret = media_entity_pads_init(&lt->sd.entity, 1, &lt->pad);
	if (ret)
		goto free_controls;
	ret = v4l2_subdev_init_finalize(&lt->sd);
	if (ret)
		goto clean_entity;
	ret = v4l2_async_register_subdev(&lt->sd);
	if (ret)
		goto clean_subdev;
	ret = devm_device_add_group(dev, &beam_lt_group);
	if (ret)
		goto unregister_subdev;
	if (device_property_read_bool(dev, "mode-switch")) {
		struct typec_mux_desc desc = {
			.fwnode = dev_fwnode(dev),
			.set = beam_lt_set_mode,
			.drvdata = lt,
		};

		lt->mode_switch = typec_mux_register(dev, &desc);
		if (IS_ERR(lt->mode_switch)) {
			ret = PTR_ERR(lt->mode_switch);
			goto unregister_subdev;
		}
	}
	INIT_DELAYED_WORK(&lt->poll, beam_lt_poll);
	mod_delayed_work(system_freezable_wq, &lt->poll, 0);
	dev_info(dev, "Beam DP receiver, chip 0x0119, four CSI-2 lanes\n");
	return 0;
unregister_subdev:
	v4l2_async_unregister_subdev(&lt->sd);
clean_subdev:
	v4l2_subdev_cleanup(&lt->sd);
clean_entity:
	media_entity_cleanup(&lt->sd.entity);
free_controls:
	v4l2_ctrl_handler_free(&lt->controls);
	return ret;
}

static void beam_lt_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct beam_lt *lt = to_beam_lt(sd);

	if (lt->mode_switch)
		typec_mux_unregister(lt->mode_switch);
	cancel_delayed_work_sync(&lt->poll);
	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&lt->controls);
}

static const struct of_device_id beam_lt_match[] = {
	{.compatible = "xreal,beam-lt7911uxc"}, {}
};
MODULE_DEVICE_TABLE(of, beam_lt_match);
static struct i2c_driver beam_lt_driver = {
	.probe = beam_lt_probe, .remove = beam_lt_remove,
	.driver = {.name = "beam-lt7911uxc", .of_match_table = beam_lt_match},
};
module_i2c_driver(beam_lt_driver);
MODULE_DESCRIPTION("XREAL Beam LT7911UXC DP to CSI-2 bridge");
MODULE_LICENSE("GPL");
