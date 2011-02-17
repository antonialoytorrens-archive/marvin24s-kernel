#include "../../../arch/arm/mach-tegra/include/mach/iomap.h"
#include "../../../arch/arm/mach-tegra/gpio-names.h"
#include <asm/io.h>
#include <asm/irq.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>

#define I2C_CNFG			(i2c_regs+0x00)
#define I2C_NEW_MASTER_SFM 	(1<<11)

#define I2C_SL_CNFG		(i2c_regs+0x20)
#define I2C_SL_NEWL		(1<<2)
#define I2C_SL_NACK		(1<<1)
#define I2C_SL_RESP		(1<<0)
#define I2C_SL_IRQ		(1<<3)
#define END_TRANS		(1<<4)
#define RCVD		(1<<2)
#define RNW		(1<<1)

#define I2C_SL_RCVD		(i2c_regs+0x24)
#define I2C_SL_STATUS		(i2c_regs+0x28)
#define I2C_SL_ADDR1		(i2c_regs+0x2c)
#define I2C_SL_ADDR2		(i2c_regs+0x30)
#define I2C_SL_DELAY_COUNT	(i2c_regs+0x3c)

static struct completion synchr;
static unsigned char *i2c_regs;
static unsigned char received;
static int nvec_gpio = TEGRA_GPIO_PV2;

static irqreturn_t i2c_interrupt(int irq, void *dev) {
	unsigned short status=readw(I2C_SL_STATUS);
	unsigned char buf[]={0x8a, 0x02, 0x07, 0x02};
	static int pos=0;
	static int started=0;

	if(!status&I2C_SL_IRQ) {
		printk("Spurious IRQ\n");
		//Yup, handled. ahum.
		return IRQ_HANDLED;
	}
	if(status&END_TRANS && !(status&RCVD)) {
		//Yeah ... nothing there
		return IRQ_HANDLED;
	} else if(status&RNW) {
		if(status&RCVD) {
			//Master wants something from us. New communication
		} else {
			//Master wants something from us from a communication we've already started
		}
		if(pos<sizeof(buf)) {
			writew(buf[pos++], I2C_SL_RCVD);
			gpio_direction_output(nvec_gpio, 1);
		} else {
			writew(I2C_SL_NEWL|I2C_SL_NACK, I2C_SL_CNFG);
		}
		return IRQ_HANDLED;
	} else {
		received=readw(I2C_SL_RCVD);
		//Workaround?
		writew(0, I2C_SL_RCVD);
		if(status&RCVD) {
			//New transaction
			printk("Received a new transaction destined to %02x (we're %02x)\n", received, 0x8a);
			return IRQ_HANDLED;
		}
		printk("Got %02x from Master !\n", received);
		complete(&synchr);
	}
	
	return IRQ_HANDLED;
}

static int __init tegra_nvec_init(void)
{
	i2c_regs=ioremap(TEGRA_I2C3_BASE, TEGRA_I2C3_SIZE);
	init_completion(&synchr);
	//Ping message
	unsigned char addr=0x8a;
	unsigned char buf[]={addr, 0x02, 0x07, 0x02};
	unsigned char rec[42];
	unsigned int pos;
	int err;
	
	err = request_irq(INT_I2C3, i2c_interrupt, 0, "i2c-slave", NULL);
	printk("ec: req irq is %d\n", err);
	writew(addr>>1, I2C_SL_ADDR1);
	writew(0, I2C_SL_ADDR2);

	writew(0x1E, I2C_SL_DELAY_COUNT);
	writew(I2C_NEW_MASTER_SFM, I2C_CNFG);
	writew(I2C_SL_NEWL, I2C_SL_CNFG);

	//Set the gpio to low when we've got something to say
	gpio_request(nvec_gpio, "nvec gpio");
	gpio_direction_output(nvec_gpio, 1);
	while(1) {
		wait_for_completion(&synchr);
		rec[pos++]=received;
		if(pos>=rec[0]+1) {
			printk("Received a message !\n");
			int i;
			for(i=0;i<=rec[0];++i) {
				printk("aa=%02x ", rec[i]);
			}
			printk("\n");
			pos=0;
		}
	}
}

module_init(tegra_nvec_init);
