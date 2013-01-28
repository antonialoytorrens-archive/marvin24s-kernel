/*
 * CLAA101WA01A Display Panel
 *
 * Copyright (C) 2013 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/backlight.h>

#include <video/display.h>

#define CLAA101WA01A_WIDTH	223
#define CLAA101WA01A_HEIGHT	125

struct panel_claa101 {
	struct display_entity entity;
	struct backlight_device *backlight;
	struct regulator *vdd_pnl;
	struct regulator *vdd_bl;
	/* Enable GPIOs */
	int pnl_enable;
	int bl_enable;
};

#define to_panel_claa101(p)	container_of(p, struct panel_claa101, entity)

static int panel_claa101_off(struct panel_claa101 *panel)
{
	/* TODO error checking? */
	if (panel->backlight) {
		panel->backlight->props.state |= BL_CORE_FBBLANK;
		backlight_update_status(panel->backlight);
	}

	gpio_set_value_cansleep(panel->bl_enable, 0);
	usleep_range(10000, 10000);
	regulator_disable(panel->vdd_bl);
	usleep_range(200000, 200000);
	gpio_set_value_cansleep(panel->pnl_enable, 0);
	regulator_disable(panel->vdd_pnl);

	if (panel->entity.source)
		display_entity_set_stream(panel->entity.source,
					  DISPLAY_ENTITY_STREAM_STOPPED);

	return 0;
}

static int panel_claa101_on(struct panel_claa101 *panel)
{
	/* TODO error checking? */
	if (panel->entity.source)
		display_entity_set_stream(panel->entity.source,
					  DISPLAY_ENTITY_STREAM_CONTINUOUS);

	regulator_enable(panel->vdd_pnl);
	gpio_set_value_cansleep(panel->pnl_enable, 1);
	usleep_range(200000, 200000);
	regulator_enable(panel->vdd_bl);
	usleep_range(10000, 10000);
	gpio_set_value_cansleep(panel->bl_enable, 1);

	if (panel->backlight) {
		panel->backlight->props.state &= ~BL_CORE_FBBLANK;
		backlight_update_status(panel->backlight);
	}

	return 0;
}

static int panel_claa101_set_state(struct display_entity *entity,
				   enum display_entity_state state)
{
	struct panel_claa101 *panel = to_panel_claa101(entity);

	switch (state) {
	case DISPLAY_ENTITY_STATE_OFF:
	case DISPLAY_ENTITY_STATE_STANDBY:
		/* OFF and STANDBY are the same to us. Avoid unbalanced calls to
		 * off() when we are switching between these two states. */
		if (entity->state == DISPLAY_ENTITY_STATE_OFF ||
		    entity->state == DISPLAY_ENTITY_STATE_STANDBY)
			return 0;

		return panel_claa101_off(panel);

	case DISPLAY_ENTITY_STATE_ON:
		return panel_claa101_on(panel);
	}

	return 0;
}

static int panel_claa101_get_modes(struct display_entity *entity,
				   const struct videomode **modes)
{
	/* TODO get modes from EDID? */
	return 0;
}

static int panel_claa101_get_size(struct display_entity *entity,
				  unsigned int *width, unsigned int *height)
{
	*width = CLAA101WA01A_WIDTH;
	*height = CLAA101WA01A_HEIGHT;

	return 0;
}

static int panel_claa101_get_params(struct display_entity *entity,
				 struct display_entity_interface_params *params)
{
	return 0;
}

static const struct display_entity_control_ops panel_claa101_control_ops = {
	.set_state = panel_claa101_set_state,
	.get_modes = panel_claa101_get_modes,
	.get_size = panel_claa101_get_size,
	.get_params = panel_claa101_get_params,
};

static int __init panel_claa101_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct panel_claa101 *panel;
	struct device_node *node;
	int err;

	panel = devm_kzalloc(dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	panel->vdd_pnl = devm_regulator_get(dev, "pnl");
	if (IS_ERR(panel->vdd_pnl)) {
		dev_err(dev, "cannot get vdd regulator\n");
		return PTR_ERR(panel->vdd_pnl);
	}

	panel->vdd_bl = devm_regulator_get(dev, "bl");
	if (IS_ERR(panel->vdd_bl)) {
		dev_err(dev, "cannot get bl regulator\n");
		return PTR_ERR(panel->vdd_bl);
	}

	err = of_get_named_gpio(dev->of_node, "pnl-enable-gpios", 0);
	if (err < 0) {
		dev_err(dev, "cannot find panel enable GPIO!\n");
		return err;
	}
	panel->pnl_enable = err;
	err = devm_gpio_request_one(dev, panel->pnl_enable,
				    GPIOF_DIR_OUT | GPIOF_INIT_LOW, "panel");
	if (err < 0) {
		dev_err(dev, "cannot acquire panel enable GPIO!\n");
		return err;
	}

	err = of_get_named_gpio(dev->of_node, "bl-enable-gpios", 0);
	if (err < 0) {
		dev_err(dev, "cannot find backlight enable GPIO!\n");
		return err;
	}
	panel->bl_enable = err;
	err = devm_gpio_request_one(dev, panel->bl_enable,
				   GPIOF_DIR_OUT | GPIOF_INIT_LOW, "backlight");
	if (err < 0) {
		dev_err(dev, "cannot acquire backlight enable GPIO!\n");
		return err;
	}

	node = of_parse_phandle(dev->of_node, "backlight", 0);
	if (node) {
		panel->backlight = of_find_backlight_by_node(node);
		if (!panel->backlight)
			return -EPROBE_DEFER;
	}

	panel->entity.dev = dev;
	panel->entity.ops.ctrl = &panel_claa101_control_ops;
	err = display_entity_register(&panel->entity);
	if (err < 0) {
		if (panel->backlight)
			put_device(&panel->backlight->dev);
		return err;
	}

	platform_set_drvdata(pdev, panel);

	return 0;
}

static int __exit panel_claa101_remove(struct platform_device *pdev)
{
	struct panel_claa101 *panel = platform_get_drvdata(pdev);

	display_entity_unregister(&panel->entity);
	if (panel->backlight)
		put_device(&panel->backlight->dev);

	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id panel_claa101_of_match[] = {
	{ .compatible = "chunghwa,claa101wa01a", },
	{ },
};
MODULE_DEVICE_TABLE(of, panel_claa101_of_match);
#else
#endif

static const struct dev_pm_ops panel_claa101_dev_pm_ops = {
};

static struct platform_driver panel_claa101_driver = {
	.probe = panel_claa101_probe,
	.remove = panel_claa101_remove,
	.driver = {
		.name = "panel_claa101wa01a",
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &panel_claa101_dev_pm_ops,
#endif
#ifdef CONFIG_OF
		.of_match_table	= of_match_ptr(panel_claa101_of_match),
#endif
	},
};

module_platform_driver(panel_claa101_driver);

MODULE_AUTHOR("Alexandre Courbot <acourbot@nvidia.com>");
MODULE_DESCRIPTION("Chunghwa CLAA101WA01A Display Panel");
MODULE_LICENSE("GPL");
