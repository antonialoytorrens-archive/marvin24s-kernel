/*
 * various events driver for a NVIDIA compliant embedded controller
 *
 * Copyright (C) 2011 Julian Andres Klode <jak@jak-linux.org>
 *
 * Authors:  Julian Andres Klode <jak@jak-linux.org>
 *           Marc Dietrich <marvin24@gmx.de>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 */

#include <linux/delay.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/mfd/core.h>
#include <linux/of.h>
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
 *	high word is system event
 *	low word is oem event
 */
struct nvec_sys_event {
	unsigned char command;
	unsigned char length;
	unsigned long payload;
};

/**
 * nvec_configure_event: disables or enables an event
 *
 * @nvec: points to the nvec mfd device structure
 * @mask: bit inside the nvec event bitmask
 * @state: 0=disable, 1=enable
 */
static void nvec_configure_event(struct nvec_chip *nvec, long mask, int state)
{
	char buf[7] = { NVEC_SYS, true, state };

	buf[3] = (mask >> 16) & 0xff;
	buf[4] = (mask >> 24) & 0xff;
	buf[5] = (mask >> 0) & 0xff;
	buf[6] = (mask >> 8) & 0xff;

	nvec_write_async(nvec, buf, 7);
};

/**
 * nvec_event_notifier: callback to process nvec events
 *
 * @event_type: contains the event type, this callback only handles
 *	type 0x85h (System event, variable length)
 * @data: contains full nvec event package
 */
static int nvec_event_notifier(struct notifier_block *nb,
			       unsigned long event_type, void *data)
{
	struct nvec_sys_event *event = data;
	struct nvec_event_device *evdev =
	    container_of(nb, struct nvec_event_device, notifier);
	struct nvec_event_entry *e;

	if (event_type != NVEC_SYSTEM_EVENT_VAR_LENGTH ||
	    (event->command & (NVEC_VAR_SIZE << 5)) == 0 || event->length != 4)
		return NOTIFY_DONE;

#ifdef DEBUG
	print_hex_dump(KERN_WARNING, "payload: ", DUMP_PREFIX_NONE, 16, 1,
	    &event->command, event->length + 2, false);
#endif

	list_for_each_entry(e, &evdev->event_list, node) {
		if (e->mask == event->payload) {
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

static int nvec_event_probe(struct platform_device *pdev)
{
	struct device_node *cells, *np = pdev->dev.parent->of_node;
	struct nvec_event_device *event_handler;
	struct nvec_chip *nvec = dev_get_drvdata(pdev->dev.parent);
	struct input_dev *nvec_idev;
	int err;

	if (!np) {
		dev_err(&pdev->dev, "no of node found\n");
		return -ENODEV;
	}

	cells = of_find_node_by_name(np, "cells");
	if (!cells) {
		dev_err(&pdev->dev, "no cell info found\n");
		return -ENODEV;
	}

	cells = of_find_node_by_name(cells, "events");
	if (!cells) {
		dev_err(&pdev->dev, "no platform data found\n");
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

	for_each_child_of_node(cells, np) {
		struct nvec_event_entry *ev_list_entry;
		int ev_type, keybit, status_bit;
		const __be32 *prop;
		const char *props;

		prop = of_get_property(np, "linux,input-type", NULL);
		if (!prop) {
			dev_err(&pdev->dev, "no input type specified\n");
			break;
		}
		ev_type = be32_to_cpup(prop);

		prop = of_get_property(np, "linux,code", NULL);
		if (!prop) {
			dev_err(&pdev->dev, "no input code specified\n");
			break;
		}
		keybit = be32_to_cpup(prop);

		prop = of_get_property(np, "nvec,event-status-mask", NULL);
		if (!prop) {
			dev_err(&pdev->dev, "no nvec status mask specified\n");
			break;
		}
		status_bit = be32_to_cpup(prop);

		nvec_idev = input_allocate_device();
		if (nvec_idev == NULL) {
			dev_err(&pdev->dev, "failed to allocate input device\n");
			break;
		}

		nvec_idev->name = np->name;
		nvec_idev->phys = "NVEC";
		nvec_idev->evbit[0] = BIT_MASK(ev_type);

		if (ev_type == EV_KEY)
			set_bit(keybit, nvec_idev->keybit);
		else if (ev_type == EV_SW)
			set_bit(keybit, nvec_idev->swbit);
		else {
			dev_err(&pdev->dev, "unsupported event type %d\n",
				ev_type);
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
		ev_list_entry->mask = status_bit;
		ev_list_entry->dev = nvec_idev;
		ev_list_entry->key = keybit;

		err = input_register_device(nvec_idev);
		if (err) {
			dev_err(&pdev->dev,
			    "failed to register input device (%d)\n", err);
			devm_kfree(&pdev->dev, ev_list_entry);
			input_free_device(nvec_idev);
			break;
		}

		props = of_get_property(np, "nvec,event-status", NULL);
		if (props && (!strcmp(props, "enabled")))
			nvec_configure_event(nvec, status_bit, 1);

		list_add_tail(&ev_list_entry->node, &event_handler->event_list);
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

static struct platform_driver nvec_event_driver = {
	.probe = nvec_event_probe,
	.remove = nvec_event_remove,
	.driver = {
		.name = "nvec-events",
		.owner = THIS_MODULE,
	},
};

module_platform_driver(nvec_event_driver);

MODULE_AUTHOR("Julian Andres Klode <jak@jak-linux.org>");
MODULE_DESCRIPTION("NVEC power/sleep/lid switch driver");
MODULE_LICENSE("GPL");
