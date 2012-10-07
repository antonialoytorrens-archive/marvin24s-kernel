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
};

struct nvec_paz00_struct nvec_paz00;

static void nvec_led_brightness_set(struct led_classdev *led_cdev,
				    enum led_brightness value)
{
	unsigned char buf[] = { NVEC_OEM0, EXEC_EC_CMD, SET_DEVICE_STATUS,
				'\x10', '\x00' };

	buf[4] = value;
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

static int __devinit nvec_paz00_probe(struct platform_device *pdev)
{
	struct nvec_chip *nvec = dev_get_drvdata(pdev->dev.parent);

	platform_set_drvdata(pdev, &nvec_paz00);
	nvec_paz00.nvec = nvec;

	return paz00_init_leds(&pdev->dev);
}

static int __devexit nvec_paz00_remove(struct platform_device *pdev)
{
	led_classdev_unregister(nvec_paz00.led_dev);

	return 0;
}

static struct platform_driver nvec_paz00_driver = {
	.probe  = nvec_paz00_probe,
	.remove = __devexit_p(nvec_paz00_remove),
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
