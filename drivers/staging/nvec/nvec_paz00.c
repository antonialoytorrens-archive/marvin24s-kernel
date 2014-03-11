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
#include <linux/input.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include "nvec.h"

#define NVEC_LED_MAX 8
#define NVEC_SYSTEM_EVENT_VAR_LENGTH (0xC5 & 0x8F)

enum nvec_oem0_subcmds {
	EXEC_EC_CMD = 0x10,
};

enum nvec_oem0_ec_cmds {
	SET_DEVICE_STATUS = 0x45,
};

enum nvec_sys_ec_cmds {
	CONF_EV_REPORTING = 1,
};

struct nvec_paz00_struct {
	struct nvec_chip *nvec;
	struct led_classdev *led_dev;
	struct notifier_block notifier;
};

struct nvec_paz00_event {
	char name[32];
	struct input_dev *dev;
	int input_type;
	int key_code;
	unsigned long status_mask;
};

struct nvec_sys_event {
	unsigned char command;
	unsigned char length;
	unsigned long payload;
};

static struct nvec_paz00_struct nvec_paz00;

static struct nvec_paz00_event nvec_paz00_events[] = {
	{
		.name = "lid switch",
		.input_type = EV_SW,
		.key_code = SW_LID,
		.status_mask = BIT(1),
	}, {
		.name = "power key",
		.input_type = EV_KEY,
		.key_code = KEY_POWER,
		.status_mask = BIT(7),
	}, {
	/* sentinel */
	},
};

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

static int nvec_event_notifier(struct notifier_block *nb,
				unsigned long event_type, void *data)
{
	struct nvec_sys_event *event = data;
	struct nvec_paz00_event *e = nvec_paz00_events;

	if (event_type != NVEC_SYSTEM_EVENT_VAR_LENGTH ||
		(event->command & (NVEC_VAR_SIZE << 5)) == 0 ||
		 event->length != 4)
		return NOTIFY_DONE;

	print_hex_dump(KERN_DEBUG, "payload: ", DUMP_PREFIX_NONE, 16, 1,
		&event->command, event->length + 2, false);

	for (; e->name[0]; e++) {
		if (e->status_mask & event->payload) {
			if (test_bit(EV_KEY, e->dev->evbit)) {
				input_report_key(e->dev, e->key_code, 1);
				input_sync(e->dev);
				input_report_key(e->dev, e->key_code, 0);
			} else if (test_bit(EV_SW, e->dev->evbit)) {
				input_report_switch(e->dev, e->key_code, 1);
			} else {
				pr_err("unknown event type\n");
				return NOTIFY_OK;
			}
		} else if (event->payload == 0)
			input_report_switch(e->dev, e->key_code, 0);

		input_sync(e->dev);
	}

	return NOTIFY_STOP;
}

static void nvec_configure_event(struct nvec_chip *nvec, long mask, int state)
{
	char buf[7] = { NVEC_SYS, CONF_EV_REPORTING, state };

	buf[3] = (mask >> 16) & 0xff;
	buf[4] = (mask >> 24) & 0xff;
	buf[5] = (mask >> 0) & 0xff;
	buf[6] = (mask >> 8) & 0xff;

	nvec_write_async(nvec, buf, 7);
};

static int paz00_init_events(struct device *dev)
{
	struct nvec_paz00_event *event = nvec_paz00_events;
	int err;

	for (; event->name[0]; event++) {

		event->dev = input_allocate_device();
		if (event->dev == NULL) {
			dev_err(dev, "failed to allocate input device\n");
			break;
		}

		event->dev->name = event->name;
		event->dev->phys = "NVEC";
		event->dev->evbit[0] = BIT_MASK(event->input_type);

		if (event->input_type == EV_KEY)
			set_bit(event->key_code, event->dev->keybit);
		else if (event->input_type == EV_SW)
			set_bit(event->key_code, event->dev->swbit);
		else {
			dev_err(dev, "unsupported event type %d\n",
				event->input_type);
			input_free_device(event->dev);
			break;
		}

		err = input_register_device(event->dev);
		if (err) {
			dev_err(dev, "failed to register input device (%d)\n",
				err);
			input_free_device(event->dev);
			break;
		}

		nvec_configure_event(nvec_paz00.nvec, event->status_mask, 1);
	}

	nvec_paz00.notifier.notifier_call = nvec_event_notifier;
	nvec_register_notifier(nvec_paz00.nvec, &nvec_paz00.notifier, 0);

	return err;
}

static int nvec_paz00_probe(struct platform_device *pdev)
{
	struct nvec_chip *nvec = dev_get_drvdata(pdev->dev.parent);
	int ret;

	platform_set_drvdata(pdev, &nvec_paz00);
	nvec_paz00.nvec = nvec;

	ret = paz00_init_leds(&pdev->dev);
	if (!ret)
		dev_err(&pdev->dev, "error registrating led device %d\n",
			ret);

	ret = paz00_init_events(&pdev->dev);
	if (!ret)
		dev_err(&pdev->dev, "error registrating input device %d\n",
			ret);

	return ret;
}

static int nvec_paz00_remove(struct platform_device *pdev)
{
	struct nvec_paz00_event *event = nvec_paz00_events;

	led_classdev_unregister(nvec_paz00.led_dev);

	nvec_unregister_notifier(nvec_paz00.nvec, &nvec_paz00.notifier);

	for (; event->name[0]; event++) {
		nvec_configure_event(nvec_paz00.nvec, event->status_mask, 0);
		input_unregister_device(event->dev);
		input_free_device(event->dev);
	}

	return 0;
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
