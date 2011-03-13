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

//#define DEBUG

// TODO: move everything to nvec_chip
static int nvec_gpio = TEGRA_GPIO_PV2;
static unsigned char rcv_data[256];
static unsigned char rcv_size;
static unsigned char resp_data[256];
static unsigned char resp_size;
static struct completion cmd_done;
static unsigned char *i2c_regs;

//We need a mutex only for send_msg since events are maed in interrupt handler

struct nvec_chip {
	struct atomic_notifier_head notifier_list;
	struct list_head tx_data;
	struct delayed_work tx_work;
};

static struct nvec_chip chip;

int nvec_register_notifier(struct device *dev, struct notifier_block *nb,
				unsigned int events)
{

	// TODO: drop global vars, move everything to chip
	// and retrieve chip from device struct
	return atomic_notifier_chain_register(&chip.notifier_list,
		 nb);

}
EXPORT_SYMBOL_GPL(nvec_register_notifier);

static DEFINE_MUTEX(cmd_mutex);
static DEFINE_MUTEX(cmd_buf_mutex);
static void (*response_handler)(void *data)=NULL;

void nvec_write_async(unsigned char *data, short size) {
	struct nvec_msg *msg= kzalloc(sizeof(struct nvec_msg), GFP_NOWAIT);
	msg->data = kzalloc(size, GFP_NOWAIT);
	msg->data[0] = size;
	memcpy(msg->data+1, data, size);
	msg->size = size+1;
	msg->pos = 0;
	INIT_LIST_HEAD(&msg->node);

	list_add_tail(&msg->node, &chip.tx_data);
	gpio_direction_output(nvec_gpio, 0);

}

EXPORT_SYMBOL(nvec_write_async);

static void nvec_request_master(struct work_struct *work) {
	if(!list_empty(&chip.tx_data)) {
		gpio_direction_output(nvec_gpio, 0);
	}
}

void nvec_release_msg() {
	mutex_unlock(&cmd_buf_mutex);
}
EXPORT_SYMBOL(nvec_release_msg);

static void parse_msg(void) {
	//Not an actual message
	if(rcv_size<2)
		return;

	atomic_notifier_call_chain(&chip.notifier_list, rcv_data[0] & 0x8f, rcv_data);
}

static irqreturn_t i2c_interrupt(int irq, void *dev) {
	unsigned short status=readw(I2C_SL_STATUS);
	unsigned short received;
	unsigned char to_send;
	struct nvec_msg *msg;

	gpio_direction_output(nvec_gpio, 1);
	if(!(status&I2C_SL_IRQ)) {
		printk(KERN_WARNING "nvec Spurious IRQ\n");
		//Yup, handled. ahum.
		goto handled;
	}
	if(status&END_TRANS && !(status&RCVD)) {
		//Reenable IRQ only when even has been sent
		//printk("Write sequence ended !\n");
		parse_msg();
		return IRQ_HANDLED;
	} else if(status&RNW) {
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
		if(list_empty(&chip.tx_data)) {
			printk(KERN_ERR "nvec empty tx!\n");
			to_send = 0x01;
		} else {
			msg = list_first_entry(&chip.tx_data,
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
				schedule_delayed_work(&chip.tx_work, msecs_to_jiffies(100));
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

void nvec_kbd_init(void);
void nvec_ps2(void);
static int __init tegra_nvec_init(void)
{
	unsigned char addr=0x8a;
	int err;
	struct clk *i2c_clk;

	i2c_clk=clk_get_sys("tegra-i2c.2", NULL);
	if(IS_ERR_OR_NULL(i2c_clk))
		printk(KERN_ERR"No such clock\n");
	else
		clk_enable(i2c_clk);
	clk_set_rate(i2c_clk, 400000);
	i2c_clk=clk_get_sys(NULL, "i2c");
	if(IS_ERR_OR_NULL(i2c_clk))
		printk(KERN_ERR"No such clock tegra-i2c.2\n");
	else
		clk_enable(i2c_clk);

	i2c_regs=ioremap(TEGRA_I2C3_BASE, TEGRA_I2C3_SIZE);
	init_completion(&cmd_done);
	
	ATOMIC_INIT_NOTIFIER_HEAD(&chip.notifier_list);
	INIT_LIST_HEAD(&chip.tx_data);
	INIT_DELAYED_WORK(&chip.tx_work, nvec_request_master);

	err = request_irq(INT_I2C3, i2c_interrupt, 0, "i2c-slave", NULL);
	printk("ec: req irq is %d\n", err);
	writew(addr>>1, I2C_SL_ADDR1);
	writew(0, I2C_SL_ADDR2);

	writew(0x1E, I2C_SL_DELAY_COUNT);
	writew(I2C_NEW_MASTER_SFM, I2C_CNFG);
	writew(I2C_SL_NEWL, I2C_SL_CNFG);

	//Set the gpio to low when we've got something to say
	gpio_request(nvec_gpio, "nvec gpio");
	mutex_init(&cmd_mutex);
	mutex_init(&cmd_buf_mutex);
	//Ping (=noop)
	nvec_write_async("\x07\x02", 2);
	nvec_kbd_init();
	nvec_ps2();


	return 0;
}

module_init(tegra_nvec_init);
