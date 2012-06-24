/*
 * nvec_paz00: OEM specific driver for Compal PAZ00 based devices
 *
 * Copyright (C) 2011 The AC100 Kernel Team <ac100@lists.launchpad.net>
 *
 * Authors:  Ilya Petrov <ilya.muromec@gmail.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 */

#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include "nvec.h"

#define NVEC_LED_MAX 8

enum nvec_oem0_subcmds {
	EXEC_EC_CMD = 0x10,
};

enum nvec_oem0_ec_cmds {
	SET_DEVICE_STATUS = 0x45,
};

struct nvec_paz00_struct {
	struct nvec_chip *nvec;
	struct led_classdev *led_dev;
	struct gpio_chip *gpio_dev;
};

struct nvec_paz00_struct nvec_paz00;

static void nvec_led_brightness_set(struct led_classdev *led_cdev,
				    enum led_brightness value)
{
	unsigned char buf[] = { NVEC_OEM0, EXEC_EC_CMD, SET_DEVICE_STATUS,
				'\x10', value };

	nvec_paz00.led_dev->brightness = value;
	nvec_write_async(nvec_paz00.nvec, buf, sizeof(buf));
}

static int paz00_init_leds(struct device *dev)
{
	nvec_paz00.led_dev = devm_kzalloc(dev, sizeof(struct led_classdev),
				GFP_KERNEL);
	if (!nvec_paz00.led_dev)
		return -ENOMEM;

	nvec_paz00.led_dev->max_brightness = NVEC_LED_MAX;
	nvec_paz00.led_dev->brightness_set = nvec_led_brightness_set;
	nvec_paz00.led_dev->brightness = 0;
	nvec_paz00.led_dev->name = "paz00-led";
	nvec_paz00.led_dev->flags |= LED_CORE_SUSPENDRESUME;

	return led_classdev_register(dev, nvec_paz00.led_dev);
}

static void nvec_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct nvec_chip *nvec = nvec_paz00.nvec;
	char buf[] = { NVEC_OEM0, EXEC_EC_CMD, 0x59, 0x94 | !!value };

	dev_dbg(nvec->dev, "gpio %d set to value %d\n", offset, value);
	nvec_write_async(nvec, buf, 4);
}

static struct gpio_chip gpio_ops = {
	.label		= "nvec",
	.owner		= THIS_MODULE,
	.set		= nvec_gpio_set,
	.base		= -1,
	.ngpio		= 1,
	.can_sleep	= 1,
};

static int paz00_init_gpios(struct device *dev)
{
	struct device_node *cell, *np = dev->parent->of_node;

	if (!np) {
		dev_err(dev, "no of node found\n");
		return -ENODEV;
	}

	cell = of_find_node_by_name(np, "cells");
	if (!cell) {
		dev_err(dev, "no cell info found\n");
		return -ENODEV;
	}

	cell = of_find_node_by_name(cell, "paz00");
	if (!cell) {
		dev_err(dev, "no platform data found\n");
		return -ENODEV;
	}

	gpio_ops.of_node = cell;
	nvec_paz00.gpio_dev = &gpio_ops;
	nvec_paz00.gpio_dev->dev = dev;

	return gpiochip_add(nvec_paz00.gpio_dev);
}

static int nvec_paz00_probe(struct platform_device *pdev)
{
	struct nvec_chip *nvec = dev_get_drvdata(pdev->dev.parent);
	int ret;

	platform_set_drvdata(pdev, &nvec_paz00);
	nvec_paz00.nvec = nvec;

	ret = paz00_init_leds(&pdev->dev);
	if (!ret)
		ret = paz00_init_gpios(&pdev->dev);

	return ret;
}

static int nvec_paz00_remove(struct platform_device *pdev)
{
	led_classdev_unregister(nvec_paz00.led_dev);
	return gpiochip_remove(nvec_paz00.gpio_dev);
}

static struct platform_driver nvec_paz00_driver = {
	.probe  = nvec_paz00_probe,
	.remove = nvec_paz00_remove,
	.driver = {
		.name  = "nvec-paz00",
		.owner = THIS_MODULE,
	},
};

module_platform_driver(nvec_paz00_driver);

MODULE_AUTHOR("Ilya Petrov <ilya.muromec@gmail.com>");
MODULE_DESCRIPTION("Tegra NVEC PAZ00 driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:nvec-paz00");
