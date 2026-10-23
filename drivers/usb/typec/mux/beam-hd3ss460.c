// SPDX-License-Identifier: GPL-2.0-only
/* HD3SS460 GPIO control for the two initial XREAL Beam USB-C ports. */
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_mux.h>

struct beam_hd3ss460 {
	struct gpio_desc *enable, *polarity, *amsel, *hpd;
	struct typec_switch_dev *sw;
	struct typec_mux_dev *mux;
	struct mutex lock;
	enum typec_orientation orientation;
	unsigned long mode;
};

static void beam_hd3ss_apply(struct beam_hd3ss460 *chip)
{
	bool four_lane = chip->mode == TYPEC_DP_STATE_C || chip->mode == TYPEC_DP_STATE_E;
	bool dp = four_lane || chip->mode == TYPEC_DP_STATE_D || chip->mode == TYPEC_DP_STATE_F;
	bool enable = chip->orientation != TYPEC_ORIENTATION_NONE && dp;

	/* TI truth table: AMSEL=1 for C/E, AMSEL=0 for D/F. */
	gpiod_set_value_cansleep(chip->enable, 0);
	gpiod_set_value_cansleep(chip->polarity, chip->orientation == TYPEC_ORIENTATION_REVERSE);
	gpiod_set_value_cansleep(chip->amsel, four_lane);
	gpiod_set_value_cansleep(chip->enable, enable);
	if (!enable)
		gpiod_set_value_cansleep(chip->hpd, 0);
}

static int beam_hd3ss_switch(struct typec_switch_dev *sw, enum typec_orientation orientation)
{
	struct beam_hd3ss460 *chip = typec_switch_get_drvdata(sw);

	mutex_lock(&chip->lock);
	chip->orientation = orientation;
	beam_hd3ss_apply(chip);
	mutex_unlock(&chip->lock);
	return 0;
}

static int beam_hd3ss_mux(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
	struct beam_hd3ss460 *chip = typec_mux_get_drvdata(mux);
	struct typec_displayport_data *dp = state->data;

	switch (state->mode) {
	case TYPEC_STATE_SAFE:
	case TYPEC_STATE_USB:
	case TYPEC_DP_STATE_C:
	case TYPEC_DP_STATE_D:
	case TYPEC_DP_STATE_E:
	case TYPEC_DP_STATE_F:
		break;
	default:
		return -EOPNOTSUPP;
	}
	mutex_lock(&chip->lock);
	chip->mode = state->mode;
	beam_hd3ss_apply(chip);
	if (state->alt && state->alt->svid == USB_TYPEC_DP_SID && dp &&
	    chip->orientation != TYPEC_ORIENTATION_NONE && state->mode >= TYPEC_STATE_MODAL)
		gpiod_set_value_cansleep(chip->hpd, !!(dp->status & DP_STATUS_HPD_STATE));
	mutex_unlock(&chip->lock);
	return 0;
}

static int beam_hd3ss_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct typec_switch_desc sw_desc = {};
	struct typec_mux_desc mux_desc = {};
	struct beam_hd3ss460 *chip;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	mutex_init(&chip->lock);
	chip->mode = TYPEC_STATE_SAFE;
	chip->enable = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(chip->enable))
		return dev_err_probe(dev, PTR_ERR(chip->enable), "enable GPIO\n");
	chip->polarity = devm_gpiod_get(dev, "polarity", GPIOD_OUT_LOW);
	if (IS_ERR(chip->polarity))
		return dev_err_probe(dev, PTR_ERR(chip->polarity), "polarity GPIO\n");
	chip->amsel = devm_gpiod_get(dev, "amsel", GPIOD_OUT_LOW);
	if (IS_ERR(chip->amsel))
		return dev_err_probe(dev, PTR_ERR(chip->amsel), "AMSEL GPIO\n");
	chip->hpd = devm_gpiod_get_optional(dev, "hpd", GPIOD_OUT_LOW);
	if (IS_ERR(chip->hpd))
		return dev_err_probe(dev, PTR_ERR(chip->hpd), "HPD GPIO\n");
	sw_desc.drvdata = chip;
	sw_desc.fwnode = dev_fwnode(dev);
	sw_desc.set = beam_hd3ss_switch;
	chip->sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(chip->sw))
		return dev_err_probe(dev, PTR_ERR(chip->sw), "Type-C switch\n");
	mux_desc.drvdata = chip;
	mux_desc.fwnode = dev_fwnode(dev);
	mux_desc.set = beam_hd3ss_mux;
	chip->mux = typec_mux_register(dev, &mux_desc);
	if (IS_ERR(chip->mux)) {
		typec_switch_unregister(chip->sw);
		return dev_err_probe(dev, PTR_ERR(chip->mux), "Type-C mux\n");
	}
	platform_set_drvdata(pdev, chip);
	return 0;
}

static void beam_hd3ss_remove(struct platform_device *pdev)
{
	struct beam_hd3ss460 *chip = platform_get_drvdata(pdev);

	typec_mux_unregister(chip->mux);
	typec_switch_unregister(chip->sw);
	gpiod_set_value_cansleep(chip->enable, 0);
	gpiod_set_value_cansleep(chip->hpd, 0);
}

static const struct of_device_id beam_hd3ss_match[] = {
	{ .compatible = "xreal,beam-hd3ss460" },
	{}
};
MODULE_DEVICE_TABLE(of, beam_hd3ss_match);

static struct platform_driver beam_hd3ss_driver = {
	.probe = beam_hd3ss_probe,
	.remove = beam_hd3ss_remove,
	.driver = {
		.name = "beam-hd3ss460",
		.of_match_table = beam_hd3ss_match,
	},
};
module_platform_driver(beam_hd3ss_driver);
MODULE_DESCRIPTION("XREAL Beam HD3SS460 Type-C DisplayPort switch");
MODULE_LICENSE("GPL");
