// #define DEBUG

/* ToDo list (incomplete, unorderd)
	- add nvec_sync_write
	- move parse_msg out of the interrupt (e.g. add rx workqueue)
	- convert mouse, keyboard, and power to platform devices
*/

#include <asm/io.h>
#include <asm/irq.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/serio.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/clk.h>
#include <mach/iomap.h>
#include <mach/clk.h>
#include <linux/mfd/nvec.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>

static unsigned char EC_DISABLE_EVENT_REPORTING[] =	{'\x04','\x00','\x00'};
static unsigned char EC_ENABLE_EVENT_REPORTING[] =	{'\x04','\x00','\x01'};
static unsigned char EC_GET_FIRMWARE_VERSION[] =	{'\x07','\x15'};

int nvec_register_notifier(struct nvec_chip *nvec, struct notifier_block *nb,
				unsigned int events)
{
	return atomic_notifier_chain_register(&nvec->notifier_list, nb);
}
EXPORT_SYMBOL_GPL(nvec_register_notifier);

static int nvec_status_notifier(struct notifier_block *nb, unsigned long event_type,
				void *data)
{
	unsigned char tmp, *msg = (unsigned char *)data;
	struct nvec_chip *nvec = container_of(nb, struct nvec_chip, nvec_status_notifier);
	int i;

	if(event_type != NVEC_CNTL)
		return NOTIFY_DONE;

/* 1st byte is ctrl, 2nd is len => swap it */
	tmp = msg[0];
	msg[0] = msg[1];
	msg[1] = tmp;

	if(!strncmp(&msg[1],
		    EC_GET_FIRMWARE_VERSION,
				sizeof(EC_GET_FIRMWARE_VERSION))) {
		dev_warn(nvec->dev, "ec firmware version %02x.%02x.%02x / %02x\n",
			msg[4], msg[5], msg[6], msg[7]);
		return NOTIFY_OK;
	}

	dev_warn(nvec->dev, "unhandled event %ld, payload: ", event_type);
	for (i = 0; i < msg[0]; i++)
		dev_warn(nvec->dev, "%0x ", msg[i+2]);
	dev_warn(nvec->dev, "\n");

	return NOTIFY_OK;
}

void nvec_write_async(struct nvec_chip *nvec, unsigned char *data, short size)
{
	struct nvec_msg *msg = kzalloc(sizeof(struct nvec_msg), GFP_NOWAIT);

	msg->data = kzalloc(size, GFP_NOWAIT);
	msg->data[0] = size;
	memcpy(msg->data + 1, data, size);
	msg->size = size + 1;
	msg->pos = 0;
	INIT_LIST_HEAD(&msg->node);

	list_add_tail(&msg->node, &nvec->tx_data);

	gpio_set_value(nvec->gpio, 0);
}
EXPORT_SYMBOL(nvec_write_async);

static void nvec_request_master(struct work_struct *work)
{
	struct nvec_chip *nvec = container_of(work, struct nvec_chip, tx_work);

	if(!list_empty(&nvec->tx_data)) {
		gpio_set_value(nvec->gpio, 0);
	}
}

static int parse_msg(struct nvec_chip *nvec)
{
	int i;

	//Not an actual message
	if(nvec->rcv_size < 2)
		return -EINVAL;

	if ((nvec->rcv_data[0] & 1<<7) == 0 && nvec->rcv_data[3])
	{
		dev_err(nvec->dev, "ec responded %02x %02x %02x %02x\n", nvec->rcv_data[0],
			nvec->rcv_data[1], nvec->rcv_data[2], nvec->rcv_data[3]);
		return -EINVAL;
	}

	if ((nvec->rcv_data[0] >> 7 ) == 1 && (nvec->rcv_data[0] & 0x0f) == 5)
	{
		dev_warn(nvec->dev, "ec system event ");
		for (i=0; i < nvec->rcv_data[1]; i++)
			dev_warn(nvec->dev, "%02x ", nvec->rcv_data[2+i]);
		dev_warn(nvec->dev, "\n");
	}

	atomic_notifier_call_chain(&nvec->notifier_list, nvec->rcv_data[0] & 0x8f, nvec->rcv_data);

	return 0;
}

static irqreturn_t i2c_interrupt(int irq, void *dev)
{
	unsigned long status;
	unsigned long received;
	unsigned char to_send;
	struct nvec_msg *msg;
	struct nvec_chip *nvec = (struct nvec_chip *)dev;
	unsigned char *i2c_regs = nvec->i2c_regs;

	status = readl(i2c_regs + I2C_SL_STATUS);

	if(!(status & I2C_SL_IRQ)) {
		dev_warn(nvec->dev, "nvec Spurious IRQ\n");
		//Yup, handled. ahum.
		goto handled;
	}
	if(status & END_TRANS && !(status & RCVD)) {
		//Reenable IRQ only when even has been sent
		//printk("Write sequence ended !\n");
                parse_msg(nvec);
		return IRQ_HANDLED;
	} else if(status & RNW) {
		// Work around for AP20 New Slave Hw Bug. Give 1us extra.
		// nvec/smbus/nvec_i2c_transport.c in NV`s crap for reference
		if(status & RCVD)
			udelay(3);

		if(status & RCVD) {
			//Master wants something from us. New communication
//			dev_dbg(nvec->dev, "New read comm!\n");
		} else {
			//Master wants something from us from a communication we've already started
//			dev_dbg(nvec->dev, "Read comm cont !\n");
		}
		//if(msg_pos<msg_size) {
		if(list_empty(&nvec->tx_data)) {
			dev_err(nvec->dev, "nvec empty tx - sending no-op\n");
			to_send = 0x8a;
			nvec_write_async(nvec, "\x07\x02", 2);
		} else {
			msg = list_first_entry(&nvec->tx_data,
				struct nvec_msg, node);	
			if(msg->pos < msg->size) {
				to_send = msg->data[msg->pos];
				msg->pos++;
			} else {
				dev_err(nvec->dev, "nvec crap! %d\n", msg->size);
				to_send = 0x01;
			}

			if(msg->pos >= msg->size) {
				list_del(&msg->node);
				kfree(msg->data);
				kfree(msg);
				schedule_work(&nvec->tx_work);
			}
		}
		writel(to_send, i2c_regs + I2C_SL_RCVD);

		gpio_set_value(nvec->gpio, 1);

		dev_dbg(nvec->dev, "nvec sent %x\n", to_send);

		goto handled;
	} else {
		received = readl(i2c_regs + I2C_SL_RCVD);
		//Workaround?
		if(status & RCVD) {
			writel(0, i2c_regs + I2C_SL_RCVD);
			nvec->rcv_size = 0;
			goto handled;
		}
		dev_dbg(nvec->dev, "Got %02lx from Master !\n", received);

		nvec->rcv_data[nvec->rcv_size] = received;
		nvec->rcv_size++;
	}
handled:

	return IRQ_HANDLED;
}

static int __devinit nvec_add_subdev(struct nvec_chip *nvec, struct nvec_subdev *subdev)
{
	struct platform_device *pdev;

	pdev = platform_device_alloc(subdev->name, subdev->id);
	pdev->dev.parent = nvec->dev;
	pdev->dev.platform_data = subdev->platform_data;

	return platform_device_add(pdev);
}

static void tegra_init_i2c_slave(struct nvec_platform_data *pdata, unsigned char *i2c_regs,
					struct clk *i2c_clk)
{
	u32 val;

	clk_enable(i2c_clk);
	tegra_periph_reset_assert(i2c_clk);
	udelay(2);
	tegra_periph_reset_deassert(i2c_clk);

	writel(pdata->i2c_addr>>1, i2c_regs + I2C_SL_ADDR1);
	writel(0, i2c_regs + I2C_SL_ADDR2);

	writel(0x1E, i2c_regs + I2C_SL_DELAY_COUNT);
	val = I2C_CNFG_NEW_MASTER_SFM | I2C_CNFG_PACKET_MODE_EN |
		(0x2 << I2C_CNFG_DEBOUNCE_CNT_SHIFT);
	writel(val, i2c_regs + I2C_CNFG);
	writel(I2C_SL_NEWL, i2c_regs + I2C_SL_CNFG);

	clk_disable(i2c_clk);
}

static int __devinit tegra_nvec_probe(struct platform_device *pdev)
{
	int err, i, ret;
	struct clk *i2c_clk;
	struct nvec_platform_data *pdata = pdev->dev.platform_data;
	struct nvec_chip *nvec;
	unsigned char *i2c_regs;

	nvec = kzalloc(sizeof(struct nvec_chip), GFP_KERNEL);
	if(nvec == NULL) {
		dev_err(&pdev->dev, "failed to reserve memory\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, nvec);
	nvec->dev = &pdev->dev;
	nvec->gpio = pdata->gpio;
	nvec->irq = pdata->irq;

/*
	i2c_clk=clk_get_sys(NULL, "i2c");
	if(IS_ERR_OR_NULL(i2c_clk))
		printk(KERN_ERR"No such clock tegra-i2c.2\n");
	else
		clk_enable(i2c_clk);
*/
	i2c_regs = ioremap(pdata->base, pdata->size);
	if(!i2c_regs) {
		dev_err(nvec->dev, "failed to ioremap registers\n");
		goto failed;
	}

	nvec->i2c_regs = i2c_regs;

	i2c_clk = clk_get_sys(pdata->clock, NULL);
	if(IS_ERR_OR_NULL(i2c_clk)) {
		dev_err(nvec->dev, "failed to get clock tegra-i2c.2\n");
		goto failed;
	}

	tegra_init_i2c_slave(pdata, i2c_regs, i2c_clk);

	err = request_irq(nvec->irq, i2c_interrupt, IRQF_DISABLED, "nvec", nvec);
	if(err) {
		dev_err(nvec->dev, "couldn't request irq");
		goto failed;
	}

	clk_enable(i2c_clk);
	clk_set_rate(i2c_clk, 8*80000);

	/* Set the gpio to low when we've got something to say */
	err = gpio_request(nvec->gpio, "nvec gpio");
	if(err < 0)
		dev_err(nvec->dev, "couldn't request gpio\n");

	tegra_gpio_enable(nvec->gpio);
	gpio_direction_output(nvec->gpio, 1);
	gpio_set_value(nvec->gpio, 1);

	ATOMIC_INIT_NOTIFIER_HEAD(&nvec->notifier_list);

	INIT_LIST_HEAD(&nvec->tx_data);
	INIT_LIST_HEAD(&nvec->rx_data);
	INIT_WORK(&nvec->tx_work, nvec_request_master);

	/* enable event reporting */
	nvec_write_async(nvec, EC_ENABLE_EVENT_REPORTING,
				sizeof(EC_ENABLE_EVENT_REPORTING));

	nvec_kbd_init(nvec);
#ifdef CONFIG_SERIO_NVEC_PS2
	nvec_ps2(nvec);
#endif

        /* setup subdevs */
	for (i = 0; i < pdata->num_subdevs; i++) {
		ret = nvec_add_subdev(nvec, &pdata->subdevs[i]);
	}

	nvec->nvec_status_notifier.notifier_call = nvec_status_notifier;
	nvec_register_notifier(nvec, &nvec->nvec_status_notifier, 0);

	/* Get Firmware Version */
	nvec_write_async(nvec, EC_GET_FIRMWARE_VERSION,
		sizeof(EC_GET_FIRMWARE_VERSION));

	/* unmute speakers? */
	nvec_write_async(nvec, "\x0d\x10\x59\x94", 4);

	/* enable lid switch event */
	nvec_write_async(nvec, "\x01\x01\x01\x00\x00\x02\x00", 7);

	/* enable power button event */
	nvec_write_async(nvec, "\x01\x01\x01\x00\x00\x80\x00", 7);

	return 0;

failed:
	kfree(nvec);
	return -ENOMEM;
}

static int __devexit tegra_nvec_remove(struct platform_device *pdev)
{
	// TODO: unregister
	return 0;
}

#ifdef CONFIG_PM

static int tegra_nvec_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct nvec_chip *nvec = platform_get_drvdata(pdev);

	dev_dbg(nvec->dev, "suspending\n");
	nvec_write_async(nvec, EC_DISABLE_EVENT_REPORTING, 3);
	nvec_write_async(nvec, "\x04\x02", 2);

	return 0;
}

static int tegra_nvec_resume(struct platform_device *pdev) {

	struct nvec_chip *nvec = platform_get_drvdata(pdev);

	dev_dbg(nvec->dev, "resuming\n");
	nvec_write_async(nvec, EC_ENABLE_EVENT_REPORTING, 3);

	return 0;
}

#else
#define tegra_nvec_suspend NULL
#define tegra_nvec_resume NULL
#endif

static struct platform_driver nvec_device_driver =
{
	.probe = tegra_nvec_probe,
	.remove = __devexit_p(tegra_nvec_remove),
	.suspend = tegra_nvec_suspend,
	.resume = tegra_nvec_resume,
	.driver = {
		.name = "nvec",
		.owner = THIS_MODULE,
	}
};

static int __init tegra_nvec_init(void)
{
	return platform_driver_register(&nvec_device_driver);
}

module_init(tegra_nvec_init);
MODULE_ALIAS("platform:nvec");
