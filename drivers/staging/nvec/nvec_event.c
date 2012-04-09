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
	NVEC_CALL(nvec, SYS, CNF_EVENT_REPORTING, state,
		(mask >> 16) & 0xff,
		(mask >> 24) & 0xff,
		(mask >> 0) & 0xff,
		(mask >> 8) & 0xff);
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

static int __devinit nvec_event_probe(struct platform_device *pdev)
{
	struct nvec_event_platform_data *pdata = pdev->mfd_cell->platform_data;
	struct nvec_event_device *event_handler;
	struct nvec_chip *nvec = dev_get_drvdata(pdev->dev.parent);
	struct input_dev *nvec_idev;
	struct nvec_event_entry *e;
	int i, err;

	event_handler = devm_kzalloc(&pdev->dev, sizeof(*event_handler), GFP_KERNEL);
	if (event_handler == NULL) {
		dev_err(&pdev->dev, "failed to reserve memory\n");
		return -ENOMEM;
	}

	if (pdata == NULL) {
		dev_err(&pdev->dev, "no events configured\n");
		return -ENODEV;
	}

	platform_set_drvdata(pdev, event_handler);
	event_handler->nvec = nvec;
	INIT_LIST_HEAD(&event_handler->event_list);

	for (i=0; i < pdata->nrevents; i++) {
		struct nvec_event *ev = &pdata->event[i];
		struct nvec_event_entry *ev_list_entry;

		nvec_idev = input_allocate_device();
		if (nvec_idev == NULL) {
			dev_err(&pdev->dev, "failed to allocate input device\n");
			err = -ENOMEM;
			goto fail;
		}

		nvec_idev->name = ev->name;
		nvec_idev->phys = "NVEC";
		nvec_idev->evbit[0] = BIT_MASK(ev->type);

		if (ev->type == EV_KEY)
			set_bit(ev->key, nvec_idev->keybit);
		else if(ev->type == EV_SW)
			set_bit(ev->key, nvec_idev->swbit);
		else {
			dev_err(&pdev->dev, "unsupported event type (%d)\n", ev->type);
			err = -ENODEV;
			goto fail;
		}

		err = input_register_device(nvec_idev);
		if (err) {
			dev_err(&pdev->dev,
			    "failed to register input device (%d)\n", err);
			goto fail;
		}

		ev_list_entry = devm_kzalloc(&pdev->dev, sizeof(*ev_list_entry), GFP_KERNEL);
		if (ev_list_entry == NULL) {
			dev_err(&pdev->dev,
			    "failed to allocate event device entry\n");
			err = -ENOMEM;
			goto fail;
		}

		if (ev->enabled)
			nvec_configure_event(nvec, ev->mask, 1);

		ev_list_entry->dev = nvec_idev;
		ev_list_entry->key = ev->key;
		ev_list_entry->mask = ev->mask;
		list_add_tail(&ev_list_entry->node, &event_handler->event_list);
	}

	event_handler->notifier.notifier_call = nvec_event_notifier;
	nvec_register_notifier(nvec, &event_handler->notifier, 0);

	return 0;

fail:
	list_for_each_entry(e, &event_handler->event_list, node) {
		input_free_device(e->dev);
	}

	return err;
}

static struct platform_driver nvec_event_driver = {
	.probe = nvec_event_probe,
	.driver = {
		.name = "nvec-event",
		.owner = THIS_MODULE,
	},
};

static int __init nvec_event_init(void)
{
	return platform_driver_register(&nvec_event_driver);
}

module_init(nvec_event_init);

MODULE_AUTHOR("Julian Andres Klode <jak@jak-linux.org>");
MODULE_DESCRIPTION("NVEC power/sleep/lid switch driver");
MODULE_LICENSE("GPL");
