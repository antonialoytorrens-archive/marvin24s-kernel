/*
 * NVEC: NVIDIA compliant embedded controller interface
 *
 * Copyright (C) 2011 Marc Dietrich <marvin24@gmx.de>
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

/* #define DEBUG */

#include <asm/irq.h>
#include <mach/iomap.h>
#include <mach/clk.h>
#include <linux/io.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/serio.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/clk.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/mfd/core.h>
#include "nvec.h"


static const unsigned char EC_DISABLE_EVENT_REPORTING[] = {'\x04', '\x00', '\x00'};
static const unsigned char EC_ENABLE_EVENT_REPORTING[] =  {'\x04', '\x00', '\x01'};
static const unsigned char EC_GET_FIRMWARE_VERSION[] =    {'\x07', '\x15'};

static struct nvec_chip *nvec_power_handle;

static struct mfd_cell nvec_devices[] = {
	{
		.name		= "nvec-kbd",
		.id		= 1,
	},
	{
		.name		= "nvec-mouse",
		.id		= 1,
	},
	{
		.name		= "nvec-power",
		.id		= 1,
	},
	{
		.name		= "nvec-power",
		.id		= 2,
	},
        {
                .name           = "nvec-leds",
                .id             = 1,
        },
};

int nvec_register_notifier(struct nvec_chip *nvec, struct notifier_block *nb,
				unsigned int events)
{
	return atomic_notifier_chain_register(&nvec->notifier_list, nb);
}
EXPORT_SYMBOL_GPL(nvec_register_notifier);

static int nvec_status_notifier(struct notifier_block *nb,
				unsigned long event_type, void *data)
{
	unsigned char *msg = (unsigned char *)data;

	if (event_type != NVEC_CNTL)
		return NOTIFY_DONE;

	printk(KERN_WARNING "unhandled msg type %ld\n", event_type);
	print_hex_dump(KERN_WARNING, "payload: ", DUMP_PREFIX_NONE, 16, 1,
		msg, msg[1] + 2, true);

	return NOTIFY_OK;
}

void nvec_write_async(struct nvec_chip *nvec, const unsigned char *data, short size)
{
	struct nvec_msg *msg;
	unsigned long flags;

	msg = kzalloc(sizeof(struct nvec_msg), GFP_ATOMIC);
	msg->data[0] = size;
	memcpy(msg->data + 1, data, size);
	msg->size = size + 1;

	spin_lock_irqsave(&nvec->tx_lock, flags);
	list_add_tail(&msg->node, &nvec->tx_data);
	spin_unlock_irqrestore(&nvec->tx_lock, flags);
//	printk("new tx is at %p\n", msg);

	queue_work(nvec->wq, &nvec->tx_work);
}
EXPORT_SYMBOL(nvec_write_async);

struct nvec_msg *nvec_write_sync(struct nvec_chip *nvec,
		const unsigned char *data, short size)
{
	mutex_lock(&nvec->sync_write_mutex);

	nvec->sync_write_pending = (data[1] << 8) + data[0];
	nvec_write_async(nvec, data, size);

	dev_dbg(nvec->dev, "nvec_sync_write: 0x%04x\n",
					nvec->sync_write_pending);
	if (!(wait_for_completion_timeout(&nvec->sync_write,
				msecs_to_jiffies(2000)))) {
		dev_warn(nvec->dev, "timeout waiting for sync write to complete\n");
		mutex_unlock(&nvec->sync_write_mutex);
		return NULL;
	}

	dev_dbg(nvec->dev, "nvec_sync_write: pong!\n");

	mutex_unlock(&nvec->sync_write_mutex);

	return nvec->last_sync_msg;
}
EXPORT_SYMBOL(nvec_write_sync);

/* TX worker */
static void nvec_request_master(struct work_struct *work)
{
	struct nvec_chip *nvec = container_of(work, struct nvec_chip, tx_work);
	unsigned long flags;
	struct nvec_msg *msg;

	mutex_lock(&nvec->async_write_mutex);
	spin_lock_irqsave(&nvec->tx_lock, flags);
	while (!list_empty(&nvec->tx_data)) {
		msg = list_first_entry(&nvec->tx_data, struct nvec_msg, node);
		spin_unlock_irqrestore(&nvec->tx_lock, flags);
		gpio_set_value(nvec->gpio, 0);
		if (!(wait_for_completion_interruptible_timeout(&nvec->ec_transfer, msecs_to_jiffies(5000)))) {
			dev_warn(nvec->dev, "timeout waiting for ec transfer\n");
			gpio_set_value(nvec->gpio, 1);
			msg->pos = 0;
		} else {
			list_del_init(&msg->node);
			kfree(msg);
		}
		spin_lock_irqsave(&nvec->tx_lock, flags);
	}
	spin_unlock_irqrestore(&nvec->tx_lock, flags);
	mutex_unlock(&nvec->async_write_mutex);
}

static int parse_msg(struct nvec_chip *nvec, struct nvec_msg *msg)
{
	if ((msg->data[0] & 1<<7) == 0 && msg->data[3]) {
		dev_err(nvec->dev, "ec responded %02x %02x %02x %02x\n",
			msg->data[0], msg->data[1], msg->data[2], msg->data[3]);
		return -EINVAL;
	}

	if ((msg->data[0] >> 7) == 1 && (msg->data[0] & 0x0f) == 5) {
		print_hex_dump(KERN_WARNING, "ec system event ", DUMP_PREFIX_NONE,
				16, 1, msg->data, msg->data[1] + 2, true);
	}

	atomic_notifier_call_chain(&nvec->notifier_list, msg->data[0] & 0x8f,
		msg->data);

	return 0;
}

/* RX worker */
static void nvec_dispatch(struct work_struct *work)
{
	struct nvec_chip *nvec = container_of(work, struct nvec_chip, rx_work);
	unsigned long flags;
	struct nvec_msg *msg;

	mutex_lock(&nvec->dispatch_mutex);
	spin_lock_irqsave(&nvec->rx_lock, flags);
	while (!list_empty(&nvec->rx_data)) {
		msg = list_first_entry(&nvec->rx_data, struct nvec_msg, node);
		list_del_init(&msg->node);
		spin_unlock_irqrestore(&nvec->rx_lock, flags);
//		printk("dispatch of %p\n", msg);

		if (nvec->sync_write_pending ==
					(msg->data[2] << 8) + msg->data[0]) {
			dev_dbg(nvec->dev, "sync write completed!\n");
			nvec->sync_write_pending = 0;
			nvec->last_sync_msg = msg;
			complete(&nvec->sync_write);
		} else {
			parse_msg(nvec, msg);
			msg->used = 0;
		}
		spin_lock_irqsave(&nvec->rx_lock, flags);
	}
	spin_unlock_irqrestore(&nvec->rx_lock, flags);
	mutex_unlock(&nvec->dispatch_mutex);
}

static irqreturn_t nvec_interrupt(int irq, void *dev)
{
	unsigned long status;
	unsigned long received = 0;
	unsigned char to_send = 0;
	unsigned long irq_mask = I2C_SL_IRQ | END_TRANS | RCVD | RNW;
	unsigned long flags;
	unsigned long dtime;
	int valid_proto = 0;
	int end_trans = 0;
	struct timespec start_time, end_time, diff_time;
	struct nvec_chip *nvec = (struct nvec_chip *)dev;
	void __iomem *base = nvec->base;

	getnstimeofday(&start_time);

	status = readl(base + I2C_SL_STATUS);

	if (!(status & irq_mask) && !((status & ~irq_mask) == 0)) {
		dev_warn(nvec->dev, "unexpected irq mask %lx\n", status);
		goto handled;
	}

	if ((status & I2C_SL_IRQ) == 0) {
		dev_warn(nvec->dev, "Spurious IRQ\n");
		goto handled;
	}

	/* just ET (but not ET with new comm [0x1c] !) */
	if ((status & END_TRANS) && !(status & RCVD))
		goto handled;

	if (status & RNW) { /* ec reads from slave, status = 0x0a, 0x0e */
		spin_lock_irqsave(&nvec->tx_lock, flags);
		if (list_empty(&nvec->tx_data)) {
			dev_err(nvec->dev, "empty tx - sending no-op\n");

			nvec->tx_scratch.data[0] = 4;
			nvec->tx_scratch.data[1] = 0x8a;
			nvec->tx_scratch.data[2] = 0x02;
			nvec->tx_scratch.data[3] = 0x07;
			nvec->tx_scratch.data[4] = 0x02;

			nvec->tx_scratch.size = 5;
			nvec->tx_scratch.pos = 0;
			nvec->tx = &nvec->tx_scratch;
			list_add_tail(&nvec->tx->node, &nvec->tx_data);
			spin_unlock_irqrestore(&nvec->tx_lock, flags);
			valid_proto = 1;
		} else {
			if (status & RCVD) {	/* 0x0e, new transfer */
				nvec->tx = list_first_entry(&nvec->tx_data, struct nvec_msg, node);
				spin_unlock_irqrestore(&nvec->tx_lock, flags);
				/* Work around for AP20 New Slave Hw Bug.
				   ((1000 / 80) / 2) + 1 = 33 */
				getnstimeofday(&end_time);
				diff_time = timespec_sub(end_time, start_time);
				dtime = timespec_to_ns(&diff_time);
				if (dtime < 33000)
					ndelay(33000 - dtime);
				else
					dev_warn(nvec->dev, "isr time: %llu nsec\n", timespec_to_ns(&diff_time));

				if (!nvec->rx)
					dev_warn(nvec->dev, "no rx buffer available\n");
				else if ((nvec->rx->pos == 1) && (nvec->rx->data[0] == 1)) {
					valid_proto = 1;
				} else {
					dev_warn(nvec->dev, "new transaction "
						"during send (pos: %d) - trying to retransmit!\n", nvec->tx->pos);
					nvec->tx->pos = 0;
				}
			} else {	/* 0x0a, transfer continues */
				spin_unlock_irqrestore(&nvec->tx_lock, flags);
				if (nvec->tx != list_first_entry(&nvec->tx_data, struct nvec_msg, node)) {
					dev_warn(nvec->dev, "tx buffer corrupted");
				}
				if ((nvec->tx->pos >= 1) && (nvec->tx->pos < nvec->tx->size)) {
//					printk("pos: %d size %d ", nvec->tx->pos, nvec->tx->size);
					valid_proto = 1;
				}
			}
		}

		if (!valid_proto) {
			dev_err(nvec->dev, "invalid protocol (sta:%lx, pos:%d, size: %d)\n", status, nvec->tx->pos, nvec->tx->size);
			to_send = 0xff;
			nvec->tx->pos = 0;
		} else {
//			dev_dbg(nvec->dev, "tx pos: %d\n", nvec->tx->pos);
			to_send = nvec->tx->data[nvec->tx->pos++];
		}

		writel(to_send, base + I2C_SL_RCVD);

		if ((status & RCVD) && valid_proto) {
			gpio_set_value(nvec->gpio, 1);
//			dev_dbg(nvec->dev, "gpio -> high\n");
		}

//		dev_dbg(nvec->dev, "nvec sent %x\n", to_send);

//		printk("sd: %02x ", to_send);

		if (nvec->tx->pos == nvec->tx->size) {
			complete(&nvec->ec_transfer);
		}

//		nvec->rx->pos = 0;

		goto handled;
	} else { /* 0x0c, 0x08, 0x1c */
		if (nvec->rx) {
			if (status & RCVD) {
				local_irq_save(flags);
				received = readl(base + I2C_SL_RCVD);
				writel(0, base + I2C_SL_RCVD);
				local_irq_restore(flags);
			} else
				received = readl(base + I2C_SL_RCVD);

//			dev_dbg(nvec->dev, "got %x pos %d status %x\n", received, nvec->rx->pos, status);

			if (status & RCVD) { /* new transaction, 0x0c, 0x1c */
				nvec->rx->pos = 0;
				nvec->rx->size = 0;
				nvec->rx->used = 1;
				if (!(received == nvec->i2c_addr))
					dev_warn(nvec->dev, "unexpected response from new slave");
			} else if (nvec->rx->pos == 0) {   /* first byte of new transaction */
				nvec->rx->data[nvec->rx->pos++] = received;
				nvec->ev_type = (received & 0x80) >> 7; /* Event or Req/Res */
				nvec->ev_len = (received & 0x60) >> 5;  /* Event length */
			} else {			/* transaction continues */
				if (nvec->rx->pos < MAX_PKT_SIZE)
					nvec->rx->data[nvec->rx->pos++] = received;
				if ((nvec->ev_len == NVEC_VAR_SIZE) || (nvec->ev_type == 0)) { /* variable write from master */
					end_trans = 0;
					switch (nvec->rx->pos) {
					case 1:
						nvec->rx->pos = 0;
						break;
					case 2:
						if ((received == 0) || (received > MAX_PKT_SIZE))
							nvec->rx->pos = 0;
						break;
					default:
						if (nvec->rx->pos == 2 + nvec->rx->data[1])
							end_trans = 1;
					}
				} else if (nvec->ev_len == NVEC_2BYTES)	/* 2 byte event */
					end_trans = (nvec->rx->pos == 2);
				else if (nvec->ev_len == NVEC_3BYTES)	/* 3 byte event */
					end_trans = (nvec->rx->pos == 3);
				else
					dev_err(nvec->dev, "grap!\n");
			}
//			printk("rec: %lx pos: %d ", received, nvec->rx->pos);
		} else {
			/* FIXME: implement NACK here ! */
			received = readl(base + I2C_SL_RCVD);
			dev_err(nvec->dev, "no rx buffer available!\n");
		}

//		printk("re=%02lx ", received);

		if (end_trans) {
			spin_lock_irqsave(&nvec->rx_lock, flags);
			    /* add the received data to the work list
			       and move the ring buffer pointer to the next entry */
			    list_add_tail(&nvec->rx->node, &nvec->rx_data);
			    nvec->rx_pos++;
			    nvec->rx_pos &= RX_BUF_MASK;
			    WARN_ON(nvec->rx_buffer[nvec->rx_pos].used == 1);
			    if (nvec->rx_buffer[nvec->rx_pos].used) {
				    dev_err(nvec->dev, "next buffer full!");
//				    nvec->rx = NULL;
			    }
			    nvec->rx->used = 0;
			    nvec->rx = &nvec->rx_buffer[nvec->rx_pos];
			spin_unlock_irqrestore(&nvec->rx_lock, flags);

			/* only complete on responses */
//			if (nvec->ev_type == 0)
//				complete(&nvec->ec_transfer);
			queue_work(nvec->wq, &nvec->rx_work);
		}
	}

handled:
//	dev_dbg(nvec->dev, "irq mask %lx\n", status);
//	printk("%02lX ", status);
//	if (end_trans) printk("END\n");
//	writel(status, base + I2C_SL_STATUS);
	return IRQ_HANDLED;
}

static void tegra_init_i2c_slave(struct nvec_chip *nvec)
{
	u32 val;

	clk_enable(nvec->i2c_clk);

	tegra_periph_reset_assert(nvec->i2c_clk);
	udelay(2);
	tegra_periph_reset_deassert(nvec->i2c_clk);

	val = I2C_CNFG_NEW_MASTER_SFM | I2C_CNFG_PACKET_MODE_EN |
		(0x2 << I2C_CNFG_DEBOUNCE_CNT_SHIFT);
	writel(val, nvec->base + I2C_CNFG);

	clk_set_rate(nvec->i2c_clk, 8 * 80000);

	writel(I2C_SL_NEWL, nvec->base + I2C_SL_CNFG);
	writel(0x1E, nvec->base + I2C_SL_DELAY_COUNT);

	writel(nvec->i2c_addr>>1, nvec->base + I2C_SL_ADDR1);
	writel(0, nvec->base + I2C_SL_ADDR2);

	enable_irq(nvec->irq);

	clk_disable(nvec->i2c_clk);
}

static void nvec_disable_i2c_slave(struct nvec_chip *nvec)
{
	disable_irq(nvec->irq);
	writel(I2C_SL_NEWL | I2C_SL_NACK, nvec->base + I2C_SL_CNFG);
	clk_disable(nvec->i2c_clk);
}

static void nvec_power_off(void)
{
	nvec_write_async(nvec_power_handle, EC_DISABLE_EVENT_REPORTING, 3);
	nvec_write_async(nvec_power_handle, "\x04\x01", 2);
}

static int __devinit tegra_nvec_probe(struct platform_device *pdev)
{
	int err, ret;
	struct clk *i2c_clk;
	struct nvec_platform_data *pdata = pdev->dev.platform_data;
	struct nvec_chip *nvec;
	struct nvec_msg *msg;
	struct resource *res;
	struct resource *iomem;
	void __iomem *base;

	nvec = kzalloc(sizeof(struct nvec_chip), GFP_KERNEL);
	if (nvec == NULL) {
		dev_err(&pdev->dev, "failed to reserve memory\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, nvec);
	nvec->dev = &pdev->dev;
	nvec->gpio = pdata->gpio;
	nvec->i2c_addr = pdata->i2c_addr;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "no mem resource?\n");
		return -ENODEV;
	}

	iomem = request_mem_region(res->start, resource_size(res), pdev->name);
	if (!iomem) {
		dev_err(&pdev->dev, "I2C region already claimed\n");
		return -EBUSY;
	}

	base = ioremap(iomem->start, resource_size(iomem));
	if (!base) {
		dev_err(&pdev->dev, "Can't ioremap I2C region\n");
		return -ENOMEM;
	}

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		dev_err(&pdev->dev, "no irq resource?\n");
		ret = -ENODEV;
		goto err_iounmap;
	}

	i2c_clk = clk_get_sys("tegra-i2c.2", NULL);
	if (IS_ERR(i2c_clk)) {
		dev_err(nvec->dev, "failed to get controller clock\n");
		goto err_iounmap;
	}

	nvec->base = base;
	nvec->irq = res->start;
	nvec->i2c_clk = i2c_clk;
	nvec->rx = &nvec->rx_buffer[0];

	err = gpio_request(nvec->gpio, "nvec gpio");
	if (err < 0)
		dev_err(nvec->dev, "couldn't request gpio\n");
	ATOMIC_INIT_NOTIFIER_HEAD(&nvec->notifier_list);

	init_completion(&nvec->sync_write);
	init_completion(&nvec->ec_transfer);
	mutex_init(&nvec->sync_write_mutex);
	mutex_init(&nvec->async_write_mutex);
	mutex_init(&nvec->dispatch_mutex);
	spin_lock_init(&nvec->tx_lock);
	spin_lock_init(&nvec->rx_lock);
	INIT_LIST_HEAD(&nvec->rx_data);
	INIT_LIST_HEAD(&nvec->tx_data);
	INIT_WORK(&nvec->rx_work, nvec_dispatch);
	INIT_WORK(&nvec->tx_work, nvec_request_master);
	nvec->wq = alloc_workqueue("nvec", WQ_NON_REENTRANT, 1);

	err = request_irq(nvec->irq, nvec_interrupt, 0, "nvec", nvec);
	if (err) {
		dev_err(nvec->dev, "couldn't request irq\n");
		goto failed;
	}
	disable_irq(nvec->irq);

	tegra_init_i2c_slave(nvec);

	clk_enable(i2c_clk);

	gpio_direction_output(nvec->gpio, 1);
	gpio_set_value(nvec->gpio, 1);
	tegra_gpio_enable(nvec->gpio);

	/* enable event reporting */
	nvec_write_async(nvec, EC_ENABLE_EVENT_REPORTING,
				sizeof(EC_ENABLE_EVENT_REPORTING));


	nvec->nvec_status_notifier.notifier_call = nvec_status_notifier;
	nvec_register_notifier(nvec, &nvec->nvec_status_notifier, 0);

	nvec_power_handle = nvec;
	pm_power_off = nvec_power_off;

	/* Get Firmware Version */
	msg = nvec_write_sync(nvec, EC_GET_FIRMWARE_VERSION,
		sizeof(EC_GET_FIRMWARE_VERSION));

	if (msg) {
		dev_warn(nvec->dev, "ec firmware version %02x.%02x.%02x / %02x\n",
			msg->data[4], msg->data[5], msg->data[6], msg->data[7]);

		msg->used = 0;
	}

	ret = mfd_add_devices(nvec->dev, -1, nvec_devices, ARRAY_SIZE(nvec_devices),
			base, 0);
	if(ret)
		dev_err(nvec->dev, "error adding subdevices\n");

	/* unmute speakers? */
	nvec_write_async(nvec, "\x0d\x10\x59\x95", 4);

	/* enable lid switch event */
	nvec_write_async(nvec, "\x01\x01\x01\x00\x00\x02\x00", 7);

	/* enable power button event */
	nvec_write_async(nvec, "\x01\x01\x01\x00\x00\x80\x00", 7);

	return 0;

err_iounmap:
	iounmap(base);
failed:
	kfree(nvec);
	return -ENOMEM;
}

static int __devexit tegra_nvec_remove(struct platform_device *pdev)
{
	struct nvec_chip *nvec = platform_get_drvdata(pdev);

	nvec_write_async(nvec, EC_DISABLE_EVENT_REPORTING, 3);
	mfd_remove_devices(nvec->dev);
	free_irq(nvec->irq, &nvec_interrupt);
	iounmap(nvec->base);
	gpio_free(nvec->gpio);
	destroy_workqueue(nvec->wq);
	kfree(nvec);

	return 0;
}

#ifdef CONFIG_PM

static int tegra_nvec_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct nvec_chip *nvec = platform_get_drvdata(pdev);

	dev_dbg(nvec->dev, "suspending\n");
	nvec_write_async(nvec, EC_DISABLE_EVENT_REPORTING, 3);
	nvec_write_async(nvec, "\x04\x02", 2);
	nvec_disable_i2c_slave(nvec);

	return 0;
}

static int tegra_nvec_resume(struct platform_device *pdev)
{
	struct nvec_chip *nvec = platform_get_drvdata(pdev);

	dev_dbg(nvec->dev, "resuming\n");
	tegra_init_i2c_slave(nvec);
	nvec_write_async(nvec, EC_ENABLE_EVENT_REPORTING, 3);

	return 0;
}

#else
#define tegra_nvec_suspend NULL
#define tegra_nvec_resume NULL
#endif

static struct platform_driver nvec_device_driver = {
	.probe = tegra_nvec_probe,
	.remove = __devexit_p(tegra_nvec_remove),
	.suspend = tegra_nvec_suspend,
	.resume = tegra_nvec_resume,
	.driver = {
		.name = "nvec",
		.owner = THIS_MODULE,
	},
};

static int __init tegra_nvec_init(void)
{
	return platform_driver_register(&nvec_device_driver);
}

module_init(tegra_nvec_init);

MODULE_ALIAS("platform:nvec");
MODULE_DESCRIPTION("NVIDIA compliant embedded controller interface");
MODULE_AUTHOR("Marc Dietrich <marvin24@gmx.de>");
MODULE_LICENSE("GPL");
