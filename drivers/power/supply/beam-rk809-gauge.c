// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only RK809 battery telemetry for initial XREAL Beam mainline boots.
 * ADC conversion follows Linux rk817_charger.c (BSP-derived coefficients).
 * Leave gauge setup, learned capacity and NVRAM untouched for 4.19 rollback.
 */
#include <linux/devm-helpers.h>
#include <linux/i2c.h>
#include <linux/mfd/rk808.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

struct beam_gauge {
	struct regmap *map;
	struct power_supply *psy;
	struct power_supply *charger;
	struct delayed_work work;
	u32 design_uah, sense_uohm;
};

static int beam_gauge_read16(struct beam_gauge *g, unsigned int reg, int *value)
{
	u8 data[2];
	int ret = regmap_bulk_read(g->map, reg, data, sizeof(data));

	if (!ret)
		*value = get_unaligned_be16(data);
	return ret;
}

static int beam_gauge_charge(struct beam_gauge *g, int *value)
{
	u8 data[4];
	s32 raw;
	int ret = regmap_bulk_read(g->map, RK817_GAS_GAUGE_Q_PRES_H3, data, sizeof(data));

	if (ret)
		return ret;
	raw = get_unaligned_be32(data);
	*value = clamp_t(s64, div_s64((s64)raw * 172 * 10000, (s64)3600 * g->sense_uohm),
			0, g->design_uah);
	return 0;
}

static int beam_gauge_get(struct power_supply *psy, enum power_supply_property prop,
			  union power_supply_propval *val)
{
	struct beam_gauge *g = power_supply_get_drvdata(psy);
	int raw, low, high, k, b, ret;

	switch (prop) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_HEALTH:
		return power_supply_get_property(g->charger, prop, val);
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = g->design_uah;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
	case POWER_SUPPLY_PROP_CHARGE_NOW:
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = beam_gauge_charge(g, &raw);
		if (ret)
			return ret;
		val->intval = prop == POWER_SUPPLY_PROP_CAPACITY ?
			div_u64((u64)raw * 100, g->design_uah) : raw;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = beam_gauge_read16(g, RK817_GAS_GAUGE_BAT_CUR_H, &raw);
		if (ret)
			return ret;
		val->intval = div_s64((s64)(s16)raw * 172 * 10000, g->sense_uohm);
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = beam_gauge_read16(g, RK817_GAS_GAUGE_VCALIB0_H, &low);
		if (ret)
			return ret;
		ret = beam_gauge_read16(g, RK817_GAS_GAUGE_VCALIB1_H, &high);
		if (ret)
			return ret;
		if (high <= low)
			return -ERANGE;
		k = 1725000 / (high - low);
		b = 4025 - k * high / 1000;
		ret = beam_gauge_read16(g, RK817_GAS_GAUGE_BAT_VOL_H, &raw);
		if (ret)
			return ret;
		val->intval = k * raw + b * 1000;
		return 0;
	default:
		return -EINVAL;
	}
}

static const enum power_supply_property beam_gauge_properties[] = {
	POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT, POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY, POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CHARGE_NOW, POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW, POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static const struct power_supply_desc beam_gauge_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = beam_gauge_properties,
	.num_properties = ARRAY_SIZE(beam_gauge_properties),
	.get_property = beam_gauge_get,
};

static void beam_gauge_poll(struct work_struct *work)
{
	struct beam_gauge *g = container_of(to_delayed_work(work), struct beam_gauge, work);

	power_supply_changed(g->psy);
	mod_delayed_work(system_freezable_wq, &g->work, msecs_to_jiffies(5000));
}

static int beam_gauge_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config config = {};
	struct device_node *pmic;
	struct i2c_client *client;
	struct beam_gauge *g;
	int ret;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;
	ret = of_property_read_u32(dev->of_node, "charge-full-design-microamp-hours", &g->design_uah);
	if (ret || !g->design_uah || g->design_uah > 10000000)
		return -EINVAL;
	ret = of_property_read_u32(dev->of_node, "sense-resistor-micro-ohms", &g->sense_uohm);
	if (ret || (g->sense_uohm != 10000 && g->sense_uohm != 20000))
		return -EINVAL;
	pmic = of_parse_phandle(dev->of_node, "pmic", 0);
	if (!pmic)
		return -EINVAL;
	client = of_find_i2c_device_by_node(pmic);
	of_node_put(pmic);
	if (!client)
		return -EPROBE_DEFER;
	g->map = dev_get_regmap(&client->dev, NULL);
	if (g->map && !device_link_add(dev, &client->dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(&client->dev);
		return -ENOMEM;
	}
	put_device(&client->dev);
	if (!g->map)
		return -EPROBE_DEFER;
	g->charger = devm_power_supply_get_by_reference(dev, "charge-controller");
	if (IS_ERR(g->charger))
		return PTR_ERR(g->charger);
	if (!g->charger)
		return -EPROBE_DEFER;
	config.drv_data = g;
	config.fwnode = dev_fwnode(dev);
	g->psy = devm_power_supply_register(dev, &beam_gauge_desc, &config);
	if (IS_ERR(g->psy))
		return PTR_ERR(g->psy);
	ret = devm_delayed_work_autocancel(dev, &g->work, beam_gauge_poll);
	if (ret)
		return ret;
	mod_delayed_work(system_freezable_wq, &g->work, msecs_to_jiffies(5000));
	return 0;
}

static const struct of_device_id beam_gauge_of_match[] = {
	{ .compatible = "xreal,beam-rk809-gauge" },
	{}
};
MODULE_DEVICE_TABLE(of, beam_gauge_of_match);
static struct platform_driver beam_gauge_driver = {
	.probe = beam_gauge_probe,
	.driver = {
		.name = "beam-rk809-gauge",
		.of_match_table = beam_gauge_of_match,
	},
};
module_platform_driver(beam_gauge_driver);
MODULE_DESCRIPTION("XREAL Beam RK809 read-only battery gauge");
MODULE_LICENSE("GPL");
