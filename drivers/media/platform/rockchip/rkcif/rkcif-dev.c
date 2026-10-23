// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Camera Interface (CIF) Driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2020 Maxime Chevallier <maxime.chevallier@bootlin.com>
 * Copyright (C) 2023 Mehdi Djait <mehdi.djait@bootlin.com>
 * Copyright (C) 2025 Michael Riesch <michael.riesch@wolfvision.net>
 * Copyright (C) 2025 Collabora, Ltd.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/sysfs.h>
#include <linux/reset.h>

#include <media/v4l2-fwnode.h>
#include <media/v4l2-mc.h>

#include "rkcif-capture-dvp.h"
#include "rkcif-capture-mipi.h"
#include "rkcif-common.h"

static const char *const px30_vip_clks[] = {
	"aclk",
	"hclk",
	"pclk",
};

static const struct rkcif_match_data px30_vip_match_data = {
	.clks = px30_vip_clks,
	.clks_num = ARRAY_SIZE(px30_vip_clks),
	.dvp = &rkcif_px30_vip_dvp_match_data,
};

static const char *const rk3568_vicap_clks[] = {
	"aclk",
	"hclk",
	"dclk",
	"iclk",
};

static const struct rkcif_match_data rk3568_vicap_match_data = {
	.clks = rk3568_vicap_clks,
	.clks_num = ARRAY_SIZE(rk3568_vicap_clks),
	.dvp = &rkcif_rk3568_vicap_dvp_match_data,
	.mipi = &rkcif_rk3568_vicap_mipi_match_data,
};

static const char *const rk3588_vicap_clks[] = {
	"aclk",
	"hclk",
	"dclk",
	"iclk",
	"iclk1",
};

static const struct rkcif_match_data rk3588_vicap_match_data = {
	.clks = rk3588_vicap_clks,
	.clks_num = ARRAY_SIZE(rk3588_vicap_clks),
	.mipi = &rkcif_rk3588_vicap_mipi_match_data,
};

static const struct of_device_id rkcif_plat_of_match[] = {
	{
		.compatible = "rockchip,px30-vip",
		.data = &px30_vip_match_data,
	},
	{
		.compatible = "rockchip,rk3568-vicap",
		.data = &rk3568_vicap_match_data,
	},
	{
		.compatible = "rockchip,rk3588-vicap",
		.data = &rk3588_vicap_match_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, rkcif_plat_of_match);

static int rkcif_register(struct rkcif_device *rkcif)
{
	int ret;

	ret = rkcif_dvp_register(rkcif);
	if (ret && ret != -ENODEV)
		goto err;

	ret = rkcif_mipi_register(rkcif);
	if (ret && ret != -ENODEV)
		goto err_dvp_unregister;

	return 0;

err_dvp_unregister:
	rkcif_dvp_unregister(rkcif);
err:
	return ret;
}

static void rkcif_unregister(struct rkcif_device *rkcif)
{
	rkcif_mipi_unregister(rkcif);
	rkcif_dvp_unregister(rkcif);
}

static int rkcif_notifier_bound(struct v4l2_async_notifier *notifier,
				struct v4l2_subdev *sd,
				struct v4l2_async_connection *asd)
{
	struct rkcif_device *rkcif =
		container_of(notifier, struct rkcif_device, notifier);
	struct rkcif_remote *remote =
		container_of(asd, struct rkcif_remote, async_conn);
	struct media_pad *sink_pad =
		&remote->interface->pads[RKCIF_IF_PAD_SINK];
	int ret;

	ret = v4l2_create_fwnode_links_to_pad(sd, sink_pad,
					      MEDIA_LNK_FL_ENABLED);
	if (ret) {
		dev_err(rkcif->dev, "failed to link source pad of %s\n",
			sd->name);
		return ret;
	}

	remote->sd = sd;

	return 0;
}

static int rkcif_notifier_complete(struct v4l2_async_notifier *notifier)
{
	struct rkcif_device *rkcif =
		container_of(notifier, struct rkcif_device, notifier);

	return v4l2_device_register_subdev_nodes(&rkcif->v4l2_dev);
}

static const struct v4l2_async_notifier_operations rkcif_notifier_ops = {
	.bound = rkcif_notifier_bound,
	.complete = rkcif_notifier_complete,
};

static irqreturn_t rkcif_isr(int irq, void *ctx)
{
	struct device *dev = ctx;
	struct rkcif_device *rkcif = dev_get_drvdata(dev);
	irqreturn_t ret = IRQ_NONE;

	atomic64_inc(&rkcif->irq_count);
	/* Access registers only while clocks and the controller IRQ are ready. */
	if (!smp_load_acquire(&rkcif->irq_enabled)) {
		atomic64_inc(&rkcif->inactive_hw_irqs);
		return IRQ_NONE;
	}
	if (rkcif_dvp_isr(irq, ctx) == IRQ_HANDLED)
		ret = IRQ_HANDLED;
	if (rkcif_mipi_isr(irq, ctx) == IRQ_HANDLED)
		ret = IRQ_HANDLED;
	return ret;
}

static ssize_t beam_irq_counts_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct rkcif_device *rkcif = dev_get_drvdata(dev);
	struct rkcif_interface *dvp = &rkcif->interfaces[RKCIF_DVP];
	struct irq_data *data = irq_get_irq_data(rkcif->irq);

	return sysfs_emit(buf,
		"irq=%lld inactive_hw=%lld inactive_dvp=%lld masked_mipi=%lld bad_buffers=%lld dvp_status=%u dvp_streams=%u irq_on=%u core_disabled=%u\n",
		(long long)atomic64_read(&rkcif->irq_count),
		(long long)atomic64_read(&rkcif->inactive_hw_irqs),
		(long long)atomic64_read(&rkcif->inactive_dvp_irqs),
		(long long)atomic64_read(&rkcif->masked_mipi_irqs),
		(long long)atomic64_read(&rkcif->bad_buffers),
		READ_ONCE(dvp->status), READ_ONCE(dvp->streams_num),
		smp_load_acquire(&rkcif->irq_enabled),
		data ? irqd_irq_disabled(data) : true);
}
static DEVICE_ATTR_RO(beam_irq_counts);
static struct attribute *rkcif_beam_attrs[] = {
	&dev_attr_beam_irq_counts.attr,
	NULL,
};
static const struct attribute_group rkcif_beam_group = {
	.attrs = rkcif_beam_attrs,
};

static int rkcif_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rkcif_device *rkcif;
	int ret, irq;

	rkcif = devm_kzalloc(dev, sizeof(*rkcif), GFP_KERNEL);
	if (!rkcif)
		return -ENOMEM;

	rkcif->match_data = of_device_get_match_data(dev);
	if (!rkcif->match_data)
		return -ENODEV;

	dev_set_drvdata(dev, rkcif);
	rkcif->dev = dev;
	atomic64_set(&rkcif->irq_count, 0);
	atomic64_set(&rkcif->inactive_hw_irqs, 0);
	atomic64_set(&rkcif->inactive_dvp_irqs, 0);
	atomic64_set(&rkcif->masked_mipi_irqs, 0);
	atomic64_set(&rkcif->bad_buffers, 0);

	rkcif->base_addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rkcif->base_addr))
		return PTR_ERR(rkcif->base_addr);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	rkcif->irq = irq;
	ret = devm_request_irq(dev, irq, rkcif_isr, IRQF_NO_AUTOEN,
			       dev_driver_string(dev), dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	if (rkcif->match_data->clks_num > RKCIF_CLK_MAX)
		return dev_err_probe(dev, -EINVAL, "invalid number of clocks\n");

	rkcif->clks_num = rkcif->match_data->clks_num;
	for (unsigned int i = 0; i < rkcif->clks_num; i++)
		rkcif->clks[i].id = rkcif->match_data->clks[i];
	ret = devm_clk_bulk_get(dev, rkcif->clks_num, rkcif->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	rkcif->reset = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(rkcif->reset))
		return PTR_ERR(rkcif->reset);

	rkcif->grf = syscon_regmap_lookup_by_phandle(dev->of_node,
						     "rockchip,grf");
	if (IS_ERR(rkcif->grf))
		rkcif->grf = NULL;

	pm_runtime_enable(&pdev->dev);

	rkcif->media_dev.dev = dev;
	strscpy(rkcif->media_dev.model, RKCIF_DRIVER_NAME,
		sizeof(rkcif->media_dev.model));
	media_device_init(&rkcif->media_dev);

	rkcif->v4l2_dev.mdev = &rkcif->media_dev;
	ret = v4l2_device_register(dev, &rkcif->v4l2_dev);
	if (ret)
		goto err_media_dev_cleanup;

	ret = media_device_register(&rkcif->media_dev);
	if (ret < 0) {
		dev_err(dev, "failed to register media device: %d\n", ret);
		goto err_v4l2_dev_unregister;
	}

	v4l2_async_nf_init(&rkcif->notifier, &rkcif->v4l2_dev);
	rkcif->notifier.ops = &rkcif_notifier_ops;

	ret = rkcif_register(rkcif);
	if (ret) {
		dev_err(dev, "failed to register media entities: %d\n", ret);
		goto err_notifier_cleanup;
	}

	ret = devm_device_add_group(dev, &rkcif_beam_group);
	if (ret)
		goto err_rkcif_unregister;

	ret = v4l2_async_nf_register(&rkcif->notifier);
	if (ret)
		goto err_rkcif_unregister;

	return 0;

err_rkcif_unregister:
	rkcif_unregister(rkcif);
err_notifier_cleanup:
	v4l2_async_nf_cleanup(&rkcif->notifier);
	media_device_unregister(&rkcif->media_dev);
err_v4l2_dev_unregister:
	v4l2_device_unregister(&rkcif->v4l2_dev);
err_media_dev_cleanup:
	media_device_cleanup(&rkcif->media_dev);
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static void rkcif_irq_disable(struct rkcif_device *rkcif)
{
	if (READ_ONCE(rkcif->irq_enabled)) {
		smp_store_release(&rkcif->irq_enabled, false);
		disable_irq(rkcif->irq);
	}
}

static void rkcif_remove(struct platform_device *pdev)
{
	struct rkcif_device *rkcif = platform_get_drvdata(pdev);

	rkcif_irq_disable(rkcif);
	v4l2_async_nf_unregister(&rkcif->notifier);
	rkcif_unregister(rkcif);
	v4l2_async_nf_cleanup(&rkcif->notifier);
	media_device_unregister(&rkcif->media_dev);
	v4l2_device_unregister(&rkcif->v4l2_dev);
	media_device_cleanup(&rkcif->media_dev);
	pm_runtime_disable(&pdev->dev);
}

static int rkcif_runtime_suspend(struct device *dev)
{
	struct rkcif_device *rkcif = dev_get_drvdata(dev);

	/* Mask and drain the dedicated controller IRQ before reset/clock gating. */
	rkcif_irq_disable(rkcif);

	/*
	 * Reset CIF (CRU, DMA, FIFOs) to allow a clean resume.
	 * Since this resets the IOMMU too, we cannot issue this reset when
	 * resuming.
	 */
	reset_control_assert(rkcif->reset);
	udelay(5);
	reset_control_deassert(rkcif->reset);

	clk_bulk_disable_unprepare(rkcif->clks_num, rkcif->clks);

	return 0;
}

static int rkcif_runtime_resume(struct device *dev)
{
	struct rkcif_device *rkcif = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(rkcif->clks_num, rkcif->clks);
	if (ret) {
		dev_err(dev, "failed to enable clocks\n");
		return ret;
	}

	/* The register bank is accessible before the IRQ can run. */
	if (!READ_ONCE(rkcif->irq_enabled)) {
		smp_store_release(&rkcif->irq_enabled, true);
		enable_irq(rkcif->irq);
	}
	return 0;
}

static const struct dev_pm_ops rkcif_plat_pm_ops = {
	.runtime_suspend = rkcif_runtime_suspend,
	.runtime_resume = rkcif_runtime_resume,
};

static struct platform_driver rkcif_plat_drv = {
	.driver = {
		   .name = RKCIF_DRIVER_NAME,
		   .of_match_table = rkcif_plat_of_match,
		   .pm = &rkcif_plat_pm_ops,
	},
	.probe = rkcif_probe,
	.remove = rkcif_remove,
};
module_platform_driver(rkcif_plat_drv);

MODULE_DESCRIPTION("Rockchip Camera Interface (CIF) platform driver");
MODULE_LICENSE("GPL");
