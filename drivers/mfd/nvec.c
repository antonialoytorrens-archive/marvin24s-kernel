#include "../../../arch/arm/mach-tegra/include/mach/iomap.h"
#include "../../../arch/arm/mach-tegra/gpio-names.h"
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
#include <mach/clk.h>
#include <linux/mfd/nvec.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>

// #define DEBUG

#define GET_FIRMWARE_VERSION "\x07\x15"


// TODO: move everything to nvec_chip
static unsigned char rcv_data[256];
static unsigned char rcv_size;

//We need a mutex only for send_msg since events are maed in interrupt handler

/*struct nvec_chip {
	struct atomic_notifier_head notifier_list;
	struct list_head tx_data;
	struct delayed_work tx_work;
	struct device *dev;
	struct notifier_block nvec_status_notifier;
	unsigned char *i2c_regs;
	int gpio;
};

static struct nvec_chip chip;
*/
int nvec_register_notifier(struct nvec_chip *nvec, struct notifier_block *nb,
				unsigned int events)
{

	// TODO: drop global vars, move everything to chip
	// and retrieve chip from device struct
	return atomic_notifier_chain_register(&nvec->notifier_list,
		 nb);

}
EXPORT_SYMBOL_GPL(nvec_register_notifier);

static int nvec_status_notifier(struct notifier_block *nb, unsigned long event_type,
				void *data)
{
	unsigned char tmp, *msg = (unsigned char *)data;

	if(event_type != NVEC_CNTL)
		return NOTIFY_DONE;

	tmp = msg[0];
	msg[0] = msg[1];
	msg[1] = tmp;

	if(!strncmp(&msg[1], GET_FIRMWARE_VERSION, sizeof(GET_FIRMWARE_VERSION))) {
		printk("nvec: ec firmware version %02x.%02x.%02x / %02x\n",
			msg[4], msg[5], msg[6], msg[7]);
		return NOTIFY_OK;
	}
	
	printk("nvec: unhandled event %ld\n", event_type);

	return NOTIFY_OK;
}

static DEFINE_MUTEX(cmd_mutex);
static DEFINE_MUTEX(cmd_buf_mutex);

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
	gpio_direction_output(nvec->gpio, 0);

}

EXPORT_SYMBOL(nvec_write_async);

static void nvec_request_master(struct work_struct *work)
{
	struct nvec_chip *nvec = container_of(work, struct nvec_chip, tx_work.work);

	if(!list_empty(&nvec->tx_data)) {
		gpio_direction_output(nvec->gpio, 0);
	}
}

void nvec_release_msg(void)
{
	mutex_unlock(&cmd_buf_mutex);
}

EXPORT_SYMBOL(nvec_release_msg);

static int parse_msg(struct nvec_chip *nvec) {
	//Not an actual message
	if(rcv_size<2)
		return -EINVAL;

	if((rcv_data[0] & 1<<7) == 0 && rcv_data[3]) {
		printk(KERN_ERR "ec responded %x\n", rcv_data[3]);
		return -EINVAL;
	}

	atomic_notifier_call_chain(&nvec->notifier_list, rcv_data[0] & 0x8f, rcv_data);
	
	return 0;
}

static irqreturn_t i2c_interrupt(int irq, void *dev) {
	unsigned short status;
	unsigned short received;
	unsigned char to_send;
	struct nvec_msg *msg;
	struct nvec_chip *nvec = (struct nvec_chip *)dev;
	unsigned char *i2c_regs = nvec->i2c_regs;
	
	
	status = readw(I2C_SL_STATUS);

	gpio_direction_output(nvec->gpio, 1);
	if(!(status&I2C_SL_IRQ)) {
		printk(KERN_WARNING "nvec Spurious IRQ\n");
		//Yup, handled. ahum.
		goto handled;
	}
	if(status&END_TRANS && !(status&RCVD)) {
		//Reenable IRQ only when even has been sent
		//printk("Write sequence ended !\n");
                parse_msg(nvec);
		return IRQ_HANDLED;
	} else if(status&RNW) {
		// Work around for AP20 New Slave Hw Bug. Give 1us extra.
		// nvec/smbus/nvec_i2c_transport.c in NV`s crap for reference
		if(status&RCVD)
			udelay(3);
#ifdef DEBUG
		if(status&RCVD) {
			//Master wants something from us. New communication
			//printk(KERN_ERR "New read comm!\n");
		} else {
			//Master wants something from us from a communication we've already started
			//printk(KERN_ERR "Read comm cont !\n");
		}
#endif
		//if(msg_pos<msg_size) {
		if(list_empty(&nvec->tx_data)) {
			printk(KERN_ERR "nvec empty tx!\n");
			to_send = 0x01;
		} else {
			msg = list_first_entry(&nvec->tx_data,
				struct nvec_msg, node);	
			if(msg->pos < msg->size) {
				to_send = msg->data[msg->pos];
				msg->pos++;
			} else {
				printk(KERN_ERR "nvec crap! %d\n", msg->size);
				to_send = 0x01;
			}

			if(msg->pos >= msg->size) {
				list_del(&msg->node);
				kfree(msg->data);
				kfree(msg);
				schedule_delayed_work(&nvec->tx_work, msecs_to_jiffies(100));
			}
		}
		writew(to_send, I2C_SL_RCVD);

#ifdef DEBUG
		printk("nvec sent %x\n", to_send);
#endif
		goto handled;
	} else {
		received=readw(I2C_SL_RCVD);
		//Workaround?
		if(status&RCVD) {
			writew(0, I2C_SL_RCVD);
			//New transaction
#ifdef DEBUG
			printk(KERN_ERR "Received a new transaction destined to %02x (we're %02x)\n", received, 0x8a);
#endif
			rcv_size=0;
			goto handled;
		}
#ifdef DEBUG
		printk(KERN_ERR "Got %02x from Master !\n", received);
#endif
		rcv_data[rcv_size]=received;
		rcv_size++;
	}
handled:
	return IRQ_HANDLED;
}

static int __devinit nvec_add_subdev(struct nvec_chip *nvec, struct nvec_subdev *subdev)
{
	struct platform_device *pdev;

	pdev =  platform_device_alloc(subdev->name, subdev->id);
	pdev->dev.parent = nvec->dev;
	pdev->dev.platform_data = subdev->platform_data;

	return platform_device_add(pdev);
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

	i2c_clk = clk_get_sys("tegra-i2c.2", NULL);
	if(IS_ERR_OR_NULL(i2c_clk)) {
		dev_err(nvec->dev, "failed to get clock tegra-i2c.2\n");
		goto failed;
	}
	clk_enable(i2c_clk);
	clk_set_rate(i2c_clk, 400000);

/*	
	i2c_clk=clk_get_sys(NULL, "i2c");
	if(IS_ERR_OR_NULL(i2c_clk))
		printk(KERN_ERR"No such clock tegra-i2c.2\n");
	else
		clk_enable(i2c_clk);
*/
	i2c_regs = ioremap(TEGRA_I2C3_BASE, TEGRA_I2C3_SIZE);
	if(!i2c_regs) {
		dev_err(nvec->dev, "failed to ioremap registers\n");
		goto failed;
	}
	nvec->i2c_regs = i2c_regs;

	ATOMIC_INIT_NOTIFIER_HEAD(&nvec->notifier_list);
	INIT_LIST_HEAD(&nvec->tx_data);
	INIT_DELAYED_WORK(&nvec->tx_work, nvec_request_master);

	err = request_irq(INT_I2C3, i2c_interrupt, 0, "i2c-slave", nvec);
	if(err) {
		dev_err(nvec->dev, "couldn't request irq");
		goto failed;
	}

	writew(pdata->i2c_addr>>1, I2C_SL_ADDR1);
	writew(0, I2C_SL_ADDR2);

	writew(0x1E, I2C_SL_DELAY_COUNT);
	writew(I2C_NEW_MASTER_SFM, I2C_CNFG);
	writew(I2C_SL_NEWL, I2C_SL_CNFG);

	//Set the gpio to low when we've got something to say
	gpio_request(nvec->gpio, "nvec gpio");
	mutex_init(&cmd_mutex);
	mutex_init(&cmd_buf_mutex);
	//Ping (=noop)
	nvec_write_async(nvec, "\x07\x02", 2);

	nvec_kbd_init(nvec);
	nvec_ps2(nvec);

        /* setup subdevs */
	for (i = 0; i < pdata->num_subdevs; i++) {
		ret = nvec_add_subdev(nvec, &pdata->subdevs[i]);
	}

	nvec->nvec_status_notifier.notifier_call = nvec_status_notifier;
	nvec_register_notifier(nvec, &nvec->nvec_status_notifier, 0);
	
	/* Get Firmware Version */
	nvec_write_async(nvec, GET_FIRMWARE_VERSION, sizeof(GET_FIRMWARE_VERSION));

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

static struct platform_driver nvec_device_driver = {
	.probe = tegra_nvec_probe,
	.remove = __devexit_p(tegra_nvec_remove),
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
