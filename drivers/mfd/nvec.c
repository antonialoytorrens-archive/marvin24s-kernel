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
#include <linux/notifier.h>

//#define DEBUG

// TODO: move everything to nvec_chip
static int nvec_gpio = TEGRA_GPIO_PV2;
static unsigned char rcv_data[256];
static unsigned char rcv_size;
static unsigned char resp_data[256];
static unsigned char resp_size;
static unsigned char msg_buf[256];
static int msg_pos=0,msg_size=0;
static struct completion cmd_done;
static unsigned char *i2c_regs;

//We need a mutex only for send_msg since events are maed in interrupt handler

struct nvec_chip {
	struct atomic_notifier_head notifier_list;
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
const char *nvec_send_msg(unsigned char *src, unsigned char *dst_size, how_care care_resp, void (*rt_handler)(unsigned char *data)) {
	static char tmp[256];
	int src_size;
	mutex_lock(&cmd_mutex);
	if(care_resp==YES)
		mutex_lock(&cmd_buf_mutex);
	response_handler=rt_handler;
	src_size=src[0]+1;
	memcpy(msg_buf, src, src_size);
	msg_pos=0;
	msg_size=src_size;
	gpio_direction_output(nvec_gpio, 0);
	int i;
	if(care_resp==NOT_AT_ALL)
		wait_for_completion_timeout(&cmd_done, 500);
	else
		wait_for_completion(&cmd_done);
	for(i=0;i<rcv_size;++i)
		printk("%02x ", resp_data[i]);
	printk("\n");
	if(care_resp==YES)
		memcpy(tmp, resp_data, resp_size);

	if(dst_size)
		*dst_size=resp_size;
	mutex_unlock(&cmd_mutex);
	return tmp;
}
EXPORT_SYMBOL(nvec_send_msg);

void nvec_release_msg() {
	mutex_unlock(&cmd_buf_mutex);
}
EXPORT_SYMBOL(nvec_release_msg);

static void parse_event(void) {

	unsigned char type=rcv_data[0]&0xf;

	atomic_notifier_call_chain(&chip.notifier_list, type, rcv_data);

}


static void parse_response(void) {
	unsigned char status=rcv_data[3];
	if(status!=0)
		printk("Reponse failed ! status=%02x\n", status);
	complete(&cmd_done);
	if(response_handler)
		response_handler(rcv_data);
	memcpy(resp_data, rcv_data, rcv_size);
	resp_size=rcv_size;
}

static void parse_msg(void) {
	//Not an actual message
	if(rcv_size<2)
		return;
	if(rcv_data[0]&(1<<7)) {
		parse_event();
	} else {
		parse_response();
	}
}

static irqreturn_t i2c_interrupt(int irq, void *dev) {
	unsigned short status=readw(I2C_SL_STATUS);
	unsigned short received;
	int i;

	gpio_direction_output(nvec_gpio, 1);
	if(!(status&I2C_SL_IRQ)) {
		printk("Spurious IRQ\n");
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
		if(msg_pos<msg_size) {
#ifdef DEBUG
			printk(KERN_ERR "Sending %02x\n", msg_buf[msg_pos]);
#endif
			writew(msg_buf[msg_pos++], I2C_SL_RCVD);
		} else {
#ifdef DEBUG
			printk(KERN_ERR "No more data(%02d/%02d)\n", msg_pos, msg_size);
#endif
			writew(0x01, I2C_SL_RCVD);
		}
		//gpio_direction_output(nvec_gpio, 1);
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
	nvec_send_msg("\x02\x07\x02", NULL, NOT_AT_ALL, NULL);
	nvec_kbd_init();
	nvec_ps2();


	return 0;
}

module_init(tegra_nvec_init);
