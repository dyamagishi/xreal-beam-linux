// SPDX-License-Identifier: GPL-2.0-only
/* First-generation XREAL Beam multiplexed NTC sensors. */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/hwmon.h>
#include <linux/iio/consumer.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include "beam-ntc-conversion.h"

struct beam_ntc;
struct beam_ntc_sensor {
	struct beam_ntc *ntc;
	int index;
};
struct beam_ntc {
	struct gpio_desc *enable, *a, *b;
	struct iio_channel *adc;
	struct mutex lock;
	unsigned long next_update;
	bool sampled, valid, filtered_valid;
	int raw[4], temp[5], filtered[3];
	struct beam_ntc_sensor sensor[6];
};

static int beam_ntc_update(struct beam_ntc *n)
{
	static const int select[4][2] = {
		{ 1, 1 }, { 0, 1 }, { 1, 0 }, { 0, 0 }
	};
	int i, ret = 0, value[4];

	if (n->sampled && time_before(jiffies, n->next_update))
		return n->valid ? 0 : -EIO;
	n->sampled = true;
	n->next_update = jiffies + HZ;
	for (i = 0; i < 4; i++) {
		gpiod_set_value_cansleep(n->a, select[i][0]);
		gpiod_set_value_cansleep(n->b, select[i][1]);
		usleep_range(1000, 1500);
		ret = iio_read_channel_raw(n->adc, &n->raw[i]);
		if (ret < 0)
			break;
		ret = beam_ntc_convert(n->raw[i], i == 3, &value[i]);
		if (ret)
			break;
	}
	/* Restore the default selector, including on read failure. */
	gpiod_set_value_cansleep(n->a, 1);
	gpiod_set_value_cansleep(n->b, 1);
	n->valid = !ret;
	if (ret) {
		n->filtered_valid = false;
		return ret;
	}
	for (i = 0; i < 4; i++)
		n->temp[i] = value[i];
	for (i = 0; i < 3; i++)
		n->filtered[i] =
			n->filtered_valid ?
				(n->filtered[i] * 95 + value[i] * 5) / 100 :
				value[i];
	n->filtered_valid = true;
	n->temp[4] = beam_ntc_outside(n->filtered);
	return 0;
}

static umode_t beam_ntc_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	return 0444;
}

static int beam_ntc_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *value)
{
	struct beam_ntc *n = dev_get_drvdata(dev);
	int ret;
	mutex_lock(&n->lock);
	ret = beam_ntc_update(n);
	if (attr == hwmon_temp_fault) {
		*value = !!ret;
		ret = 0;
	} else if (!ret) {
		*value = n->temp[channel];
	}
	mutex_unlock(&n->lock);
	return ret;
}

static int beam_ntc_read_string(struct device *dev,
				enum hwmon_sensor_types type, u32 attr,
				int channel, const char **str)
{
	static const char *const names[] = { "ntc1", "ntc2", "ntc3", "battery",
					     "outside" };
	*str = names[channel];
	return 0;
}

static const struct hwmon_ops beam_ntc_hwmon_ops = {
	.is_visible = beam_ntc_visible,
	.read = beam_ntc_read,
	.read_string = beam_ntc_read_string,
};
static const struct hwmon_channel_info *const beam_ntc_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_FAULT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_FAULT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_FAULT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_FAULT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_FAULT | HWMON_T_LABEL),
	NULL,
};
static const struct hwmon_chip_info beam_ntc_chip = {
	.ops = &beam_ntc_hwmon_ops,
	.info = beam_ntc_info,
};

static int beam_ntc_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct beam_ntc_sensor *s = thermal_zone_device_priv(tz);
	struct beam_ntc *n = s->ntc;
	int ret;
	mutex_lock(&n->lock);
	ret = beam_ntc_update(n);
	if (!ret)
		*temp = s->index == 5 ? -n->temp[3] : n->temp[s->index];
	else {
		/* Fail-safe thermal demand, below emergency power-off trips. hwmon
   * reports EIO/fault instead of displaying these as real measurements. */
		static const int fail_temp[] = { 79000, 79000, 79000,
						 60000, 60000, 1000 };
		*temp = fail_temp[s->index];
	}
	mutex_unlock(&n->lock);
	return 0;
}
static const struct thermal_zone_device_ops beam_ntc_thermal_ops = {
	.get_temp = beam_ntc_get_temp,
};

static int beam_ntc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev, *hwmon;
	struct thermal_zone_device *tz;
	struct beam_ntc *n;
	int i, ret;
	n = devm_kzalloc(dev, sizeof(*n), GFP_KERNEL);
	if (!n)
		return -ENOMEM;
	mutex_init(&n->lock);
	n->enable = devm_gpiod_get(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(n->enable))
		return dev_err_probe(dev, PTR_ERR(n->enable),
				     "NTC enable GPIO\n");
	n->a = devm_gpiod_get(dev, "select-a", GPIOD_OUT_HIGH);
	if (IS_ERR(n->a))
		return dev_err_probe(dev, PTR_ERR(n->a), "NTC selector A\n");
	n->b = devm_gpiod_get(dev, "select-b", GPIOD_OUT_HIGH);
	if (IS_ERR(n->b))
		return dev_err_probe(dev, PTR_ERR(n->b), "NTC selector B\n");
	n->adc = devm_iio_channel_get(dev, "ntc");
	if (IS_ERR(n->adc))
		return dev_err_probe(dev, PTR_ERR(n->adc), "NTC ADC\n");
	platform_set_drvdata(pdev, n);
	ret = beam_ntc_update(n);
	if (ret)
		dev_warn(
			dev,
			"NTC initial sample invalid; thermal demand is fail-safe\n");
	hwmon = devm_hwmon_device_register_with_info(dev, "beam_ntc", n,
						     &beam_ntc_chip, NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);
	for (i = 0; i < 6; i++) {
		n->sensor[i].ntc = n;
		n->sensor[i].index = i;
		tz = devm_thermal_of_zone_register(dev, i, &n->sensor[i],
						   &beam_ntc_thermal_ops);
		if (IS_ERR(tz) && PTR_ERR(tz) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(tz),
					     "NTC thermal zone %d\n", i);
	}
	return 0;
}
static const struct of_device_id beam_ntc_match[] = {
	{ .compatible = "xreal,beam-ntc" },
	{}
};
MODULE_DEVICE_TABLE(of, beam_ntc_match);
static struct platform_driver beam_ntc_driver = {
	.probe = beam_ntc_probe,
	.driver = { .name = "beam-ntc", .of_match_table = beam_ntc_match },
};
module_platform_driver(beam_ntc_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("XREAL Beam multiplexed NTC sensors");
MODULE_IMPORT_NS("IIO_CONSUMER");
