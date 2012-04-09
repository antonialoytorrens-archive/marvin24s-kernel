/*
 * nvec_ps2: mouse driver for a NVIDIA compliant embedded controller
 *
 * Copyright (C) 2011 The AC100 Kernel Team <ac100@lists.launchpad.net>
 *
 * Authors:  Pierre-Hugues Husson <phhusson@free.fr>
 *           Ilya Petrov <ilya.muromec@gmail.com>
 *           Marc Dietrich <marvin24@gmx.de>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/serio.h>
#include <linux/delay.h>
#include <linux/platform_device.h>

#include "nvec.h"

#define PACKET_SIZE 6

#ifdef NVEC_PS2_DEBUG
#define NVEC_PHD(str, buf, len) \
	print_hex_dump(KERN_DEBUG, str, DUMP_PREFIX_NONE, \
			16, 1, buf, len, false)
#else
#define NVEC_PHD(str, buf, len)
#endif

struct nvec_ps2 {
	struct serio *ser_dev;
	struct notifier_block notifier;
	struct nvec_chip *nvec;
};

static struct nvec_ps2 ps2_dev;

static int ps2_startstreaming(struct serio *ser_dev)
{
	return NVEC_CALL(ps2_dev.nvec, PS2, AUTO_RECEIVE, PACKET_SIZE);
}

static void ps2_stopstreaming(struct serio *ser_dev)
{
	NVEC_CALL(ps2_dev.nvec, PS2, CANCEL_AUTO_RECEIVE);
}

static int ps2_sendcommand(struct serio *ser_dev, unsigned char cmd)
{
	unsigned char buf[] = NVEC_CMD_STR(PS2, SEND_CMD, 0, 1);

	buf[2] = cmd & 0xff;

	dev_dbg(&ser_dev->dev, "Sending ps2 cmd %02x\n", cmd);
	return nvec_write_async(ps2_dev.nvec, buf, sizeof(buf));
}

static int nvec_ps2_notifier(struct notifier_block *nb,
			     unsigned long event_type, void *data)
{
	int i;
	unsigned char *msg = (unsigned char *)data;

	switch (event_type) {
	case NVEC_PS2_EVT:
		for (i = 0; i < msg[1]; i++)
			serio_interrupt(ps2_dev.ser_dev, msg[2 + i], 0);
		NVEC_PHD("ps/2 mouse event: ", &msg[2], msg[1]);
		return NOTIFY_STOP;

	case NVEC_PS2:
		if (msg[2] == 1) {
			for (i = 0; i < (msg[1] - 2); i++)
				serio_interrupt(ps2_dev.ser_dev, msg[i + 4], 0);
			NVEC_PHD("ps/2 mouse reply: ", &msg[4], msg[1] - 2);
		}

		else if (msg[1] != 2) /* !ack */
			NVEC_PHD("unhandled mouse event: ", msg, msg[1] + 2);
		return NOTIFY_STOP;
	}

	return NOTIFY_DONE;
}

static int __devinit nvec_mouse_probe(struct platform_device *pdev)
{
	struct nvec_chip *nvec = dev_get_drvdata(pdev->dev.parent);
	struct serio *ser_dev = kzalloc(sizeof(struct serio), GFP_KERNEL);

	ser_dev->id.type = SERIO_PS_PSTHRU;
	ser_dev->write = ps2_sendcommand;
	ser_dev->start = ps2_startstreaming;
	ser_dev->stop = ps2_stopstreaming;

	strlcpy(ser_dev->name, "nvec mouse", sizeof(ser_dev->name));
	strlcpy(ser_dev->phys, "nvec", sizeof(ser_dev->phys));

	ps2_dev.ser_dev = ser_dev;
	ps2_dev.notifier.notifier_call = nvec_ps2_notifier;
	ps2_dev.nvec = nvec;
	nvec_register_notifier(nvec, &ps2_dev.notifier, 0);

	serio_register_port(ser_dev);

	/* mouse reset */
	NVEC_CALL(ps2_dev.nvec, PS2, SEND_CMD, 0xff, 0x03);

	return 0;
}

static int nvec_mouse_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct nvec_chip *nvec = dev_get_drvdata(pdev->dev.parent);

	NVEC_CALL(nvec, PS2, PS2_DISABLE);
	NVEC_CALL(nvec, PS2, CANCEL_AUTO_RECEIVE);

	return 0;
}

static int nvec_mouse_resume(struct platform_device *pdev)
{
	struct nvec_chip *nvec = dev_get_drvdata(pdev->dev.parent);

	ps2_startstreaming(ps2_dev.ser_dev);

	/* enable mouse */
	NVEC_CALL(nvec, PS2, PS2_ENABLE);

	return 0;
}

static struct platform_driver nvec_mouse_driver = {
	.probe  = nvec_mouse_probe,
	.suspend = nvec_mouse_suspend,
	.resume = nvec_mouse_resume,
	.driver = {
		.name = "nvec-mouse",
		.owner = THIS_MODULE,
	},
};

static int __init nvec_mouse_init(void)
{
	return platform_driver_register(&nvec_mouse_driver);
}

module_init(nvec_mouse_init);

MODULE_DESCRIPTION("NVEC mouse driver");
MODULE_AUTHOR("Marc Dietrich <marvin24@gmx.de>");
MODULE_LICENSE("GPL");
