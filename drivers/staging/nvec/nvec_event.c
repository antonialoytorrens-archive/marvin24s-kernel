/*
 * various events driver for a NVIDIA compliant embedded controller
 *
 * Copyright (C) 2011 Julian Andres Klode <jak@jak-linux.org>
 *
 * Authors:  Julian Andres Klode <jak@jak-linux.org>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include "nvec.h"

#define NVEC_SYSTEM_EVENT_VAR_LENGTH (0xC5 & 0x8F)

/**
 * nvec_event_entry: entry for the event list handled by the nvec event driver
 *
 * @node: list node
 * @dev: points to the input device
 * @key: key to report on event
 * @mask: bit used in the event bitmask of the nvec
 */
struct nvec_event_entry {
	struct list_head node;
	struct input_dev *dev;
	int key;
	unsigned long mask;
};

/**
 * nvec_event_device: device structure of the nvec event driver
 *
 * @nvec: points to the device structure of the parent nvec mfd
 * @notifier: callback to handle nvec events
 * @event_list: list of events handled by this driver
 */
struct nvec_event_device {
	struct nvec_chip *nvec;
	struct notifier_block notifier;
	struct list_head event_list;
};

/**
 * nvec_sys_event: helper structure to decode event packages
 *
 * @command: command byte in the event string
 * @length: length of payload
 * @payload: 4-byte payload
 *     high word is system event
 *     low word is oem event
 */
struct nvec_sys_event {
	unsigned char command;
	unsigned char length;
	unsigned long payload;
};

enum nvec_sys_subcmds {
	GET_SYSTEM_STATUS,
	CONF_EV_REPORTING,
};

static int nvec_event_notifier(struct notifier_block *nb,
			       unsigned long event_type, void *data)
{
	struct nvec_sys_event *event = data;
	struct nvec_event_device *evdev =
		container_of(nb, struct nvec_event_device, notifier);
	struct nvec_event_entry *e;

	if (event_type != NVEC_SYSTEM_EVENT_VAR_LENGTH ||
		(event->command & (NVEC_VAR_SIZE << 5)) == 0 ||
		 event->length != 4)
		return NOTIFY_DONE;

	print_hex_dump(KERN_DEBUG, "payload: ", DUMP_PREFIX_NONE, 16, 1,
			&event->command, event->length + 2, false);

	list_for_each_entry(e, &evdev->event_list, node) {
		if (e->mask && event->payload) {
			if (test_bit(EV_KEY, e->dev->evbit)) {
				input_report_key(e->dev, e->key, 1);
				input_sync(e->dev);
				input_report_key(e->dev, e->key, 0);
			} else if (test_bit(EV_SW, e->dev->evbit)) {
				input_report_switch(e->dev, e->key, 1);
			} else {
				pr_err("unknown event type\n");
				return NOTIFY_OK;
			}
		} else if (event->payload == 0)
			input_report_switch(e->dev, e->key, 0);
		input_sync(e->dev);
	}

	return NOTIFY_STOP;
}

/**
 * nvec_configure_event: disables or enables an event
 *
 * @nvec: points to the nvec mfd device structure
 * @mask: bit inside the nvec event bitmask
 * @state: 0=disable, 1=enable
 */
static void nvec_configure_event(struct nvec_chip *nvec, long mask, int state)
{
	char buf[7] = { NVEC_SYS, CONF_EV_REPORTING, state };

	buf[3] = (mask >> 16) & 0xff;
	buf[4] = (mask >> 24) & 0xff;
	buf[5] = (mask >> 0) & 0xff;
	buf[6] = (mask >> 8) & 0xff;

	nvec_write_async(nvec, buf, 7);
};

static int __devinit nvec_event_probe(struct platform_device *pdev)
{
	struct nvec_chip *nvec = dev_get_drvdata(pdev->dev.parent);
	struct nvec_events_platform_data *pdata = pdev->dev.platform_data;
	struct nvec_event_device *event_handler;
	struct input_dev *nvec_idev;
	int i = 0, err;

	if (!pdata) {
		dev_err(&pdev->dev, "no platform data\n");
		return -ENODEV;
	}

	event_handler = devm_kzalloc(&pdev->dev, sizeof(*event_handler),
				    GFP_KERNEL);
	if (event_handler == NULL) {
		dev_err(&pdev->dev, "failed to reserve memory\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, event_handler);
	event_handler->nvec = nvec;
	INIT_LIST_HEAD(&event_handler->event_list);

	while (pdata[i].status_mask) {
		struct nvec_event_entry *ev_list_entry;

		nvec_idev = input_allocate_device();
		if (nvec_idev == NULL) {
			dev_err(&pdev->dev, "failed to allocate input device\n");
			break;
		}

		nvec_idev->name = pdata[i].name;
		nvec_idev->phys = "NVEC";
		nvec_idev->evbit[0] = BIT_MASK(pdata[i].input_type);

		if (pdata[i].input_type == EV_KEY)
			set_bit(pdata[i].key_code, nvec_idev->keybit);
		else if (pdata[i].input_type == EV_SW)
			set_bit(pdata[i].key_code, nvec_idev->swbit);
		else {
			dev_err(&pdev->dev, "unsupported event type %d\n",
				pdata[i].input_type);
			input_free_device(nvec_idev);
			break;
		}

		ev_list_entry = devm_kzalloc(&pdev->dev, sizeof(*ev_list_entry),
						GFP_KERNEL);
		if (ev_list_entry == NULL) {
			dev_err(&pdev->dev,
				"failed to allocate event device entry\n");
			input_free_device(nvec_idev);
			break;
		}
		ev_list_entry->mask = pdata[i].status_mask;
		ev_list_entry->dev = nvec_idev;
		ev_list_entry->key = pdata[i].key_code;

		err = input_register_device(nvec_idev);
		if (err) {
			dev_err(&pdev->dev,
				"failed to register input device (%d)\n", err);
			devm_kfree(&pdev->dev, ev_list_entry);
			input_free_device(nvec_idev);
			break;
		}

		if (pdata[i].enabled)
			nvec_configure_event(nvec, pdata[i].status_mask, 1);

		list_add_tail(&ev_list_entry->node, &event_handler->event_list);
		i++;
	}

	event_handler->notifier.notifier_call = nvec_event_notifier;
	nvec_register_notifier(nvec, &event_handler->notifier, 0);

	return 0;
}

static int nvec_event_remove(struct platform_device *pdev)
{
	struct nvec_event_device *event_handler = platform_get_drvdata(pdev);
	struct input_dev *idev;

	list_for_each_entry(idev, &event_handler->event_list, node) {
		input_unregister_device(idev);
		input_free_device(idev);
	}

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int nvec_event_suspend(struct device *dev)
{
	return 0;
}

static int nvec_event_resume(struct device *dev)
{
	return 0;
}
#endif

static const SIMPLE_DEV_PM_OPS(nvec_event_pm_ops, nvec_event_suspend,
				nvec_event_resume);

static struct platform_driver nvec_event_driver = {
	.probe = nvec_event_probe,
	.remove = nvec_event_remove,
	.driver = {
		.name = "nvec-event",
		.owner = THIS_MODULE,
		.pm = &nvec_event_pm_ops,
	},
};

module_platform_driver(nvec_event_driver);

MODULE_AUTHOR("Julian Andres Klode <jak@jak-linux.org>");
MODULE_DESCRIPTION("NVEC power/sleep/lid switch driver");
MODULE_LICENSE("GPL");
