#define DEBUG

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
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/clk.h>
#include <mach/clk.h>

#define I2C_CNFG		(i2c_regs+0x00)
#define I2C_NEW_MASTER_SFM 	(1<<11)

#define I2C_SL_CNFG		(i2c_regs+0x20)
#define I2C_SL_NEWL		(1<<2)
#define I2C_SL_NACK		(1<<1)
#define I2C_SL_RESP		(1<<0)
#define I2C_SL_IRQ		(1<<3)
#define END_TRANS		(1<<4)
#define RCVD			(1<<2)
#define RNW			(1<<1)

#define I2C_SL_RCVD		(i2c_regs+0x24)
#define I2C_SL_STATUS		(i2c_regs+0x28)
#define I2C_SL_ADDR1		(i2c_regs+0x2c)
#define I2C_SL_ADDR2		(i2c_regs+0x30)
#define I2C_SL_DELAY_COUNT	(i2c_regs+0x3c)


static int nvec_gpio = TEGRA_GPIO_PV2;
static unsigned char rcv_data[256];
static unsigned char rcv_size;
static unsigned char msg_buf[256];
static int msg_pos=0,msg_size=0;
static struct completion cmd_done;
static unsigned char *i2c_regs;

typedef struct {
	unsigned char evt_type;
	void (*got_event)(unsigned char *data, unsigned char size);
} nvec_event_handler;

static nvec_event_handler handler_list[10];

void nvec_add_handler(unsigned char type, void (*got_event)(unsigned char *data, unsigned char size)) {
	int i;
	for(i=0;handler_list[i].got_event;++i);
	handler_list[i].got_event=got_event;
	handler_list[i].evt_type=type;
}
EXPORT_SYMBOL(nvec_add_handler);

//We need a mutex only for send_msg since events are maed in interrupt handler
static DEFINE_MUTEX(cmd_mutex);
const char *nvec_send_msg(unsigned char *src, unsigned char src_size, unsigned char *dst_size, int care_resp) {
	static char tmp[256];
	if(care_resp)
		mutex_lock(&cmd_mutex);
	if(!src_size)
		src_size=src[0]+1;
	memcpy(msg_buf, src, src_size);
	msg_pos=0;
	msg_size=src_size;
	gpio_direction_output(nvec_gpio, 0);
	if(care_resp) {
		wait_for_completion(&cmd_done);
		memcpy(tmp, rcv_data, rcv_size);

		if(dst_size)
			*dst_size=rcv_size;
		enable_irq(INT_I2C3);
		return rcv_data;
	} else {
		return NULL;
	}
}
EXPORT_SYMBOL(nvec_send_msg);

void nvec_release_msg() {
	mutex_unlock(&cmd_mutex);
}
EXPORT_SYMBOL(nvec_release_msg);

static void parse_event(void) {
	unsigned char type=rcv_data[0]&0xf;
	int i;
	for(i=0;handler_list[i].got_event;++i)
		if(type==handler_list[i].evt_type)
			handler_list[i].got_event(rcv_data, rcv_size);
}


static void parse_response(void) {
	unsigned char status=rcv_data[3];
	if(status!=0)
		printk("Reponse failed ! status=%02x\n", status);
	if(mutex_is_locked(&cmd_mutex)) {
		complete(&cmd_done);
		disable_irq_nosync(INT_I2C3);
	}
}

static void parse_msg(void) {
	if(rcv_data[0]&(1<<7)) {
		parse_event();
	} else {
		parse_response();
	}
}

static irqreturn_t i2c_interrupt(int irq, void *dev) {
	unsigned short status=readw(I2C_SL_STATUS);
	unsigned short received;

	if(!(status&I2C_SL_IRQ)) {
		printk("Spurious IRQ\n");
		//Yup, handled. ahum.
		goto handled;
	}
	gpio_direction_output(nvec_gpio, 1);
	if(status&END_TRANS && !(status&RCVD)) {
		//Reenable IRQ only when even has been sent
		parse_msg();
		return IRQ_HANDLED;
	} else if(status&RNW) {
#ifdef DEBUG
		if(status&RCVD) {
			//Master wants something from us. New communication
			printk(KERN_ERR "New read comm!\n");
		} else {
			//Master wants something from us from a communication we've already started
			printk(KERN_ERR "Read comm cont !\n");
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
		goto handled;
	} else {
		received=readw(I2C_SL_RCVD);
		//Workaround?
		writew(0, I2C_SL_RCVD);
		if(status&RCVD) {
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
	i2c_clk=clk_get_sys(NULL, "i2c");
	if(IS_ERR_OR_NULL(i2c_clk))
		printk(KERN_ERR"No such clock tegra-i2c.2\n");
	else
		clk_enable(i2c_clk);

	i2c_regs=ioremap(TEGRA_I2C3_BASE, TEGRA_I2C3_SIZE);
	init_completion(&cmd_done);
	
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

	return 0;
}

module_init(tegra_nvec_init);
