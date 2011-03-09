#include <linux/slab.h>
#include <linux/serio.h>
#include <linux/mfd/nvec.h>

struct nvec_ps2 {
	struct serio *ser_dev;
	struct notifier_block notifier;
	struct device *master;
};

static struct nvec_ps2 ps2_dev;

static int ps2_startstreaming(struct serio *ser_dev) {
	nvec_write_async("\x06\x03\x01", 3);
	return 0;
}

static void ps2_stopstreaming(struct serio *ser_dev) {
	nvec_write_async("\x06\x04", 2);
}

/* is this really needed?
static void nvec_resp_handler(unsigned char *data) {
	serio_interrupt(ser_dev, data[4], 0);
}
*/

static int ps2_sendcommand(struct serio *ser_dev, unsigned char cmd) {
	unsigned char *buf="\x06\x01\xf4\x01";
	buf[2]=cmd;
	printk(KERN_ERR "Sending ps2 cmd %02x\n", cmd);
	nvec_write_async(buf, 4);
	//ret=nvec_send_msg(buf, &size, NOT_AT_ALL, nvec_resp_handler);
	return 0;
}

static int nvec_ps2_notifier(struct notifier_block *nb,
				 unsigned long event_type, 
		unsigned char *data)
{
	if (event_type == NVEC_PS2_EVT)
		serio_interrupt(ps2_dev.ser_dev, data[2], 0);

	return 0;
}

int __init nvec_ps2(void) {
	struct serio *ser_dev=kzalloc(sizeof(struct serio), GFP_KERNEL);
	ser_dev->id.type=SERIO_8042;
	ser_dev->write=ps2_sendcommand;
	ser_dev->open=ps2_startstreaming;
	ser_dev->close=ps2_stopstreaming;
	strlcpy(ser_dev->name, "NVEC PS2", sizeof(ser_dev->name));
	strlcpy(ser_dev->phys, "NVEC I2C slave", sizeof(ser_dev->phys));

	ps2_dev.ser_dev = ser_dev;
	ps2_dev.notifier.notifier_call = nvec_ps2_notifier;
	nvec_register_notifier(NULL, &ps2_dev.notifier, 0);

	serio_register_port(ser_dev);
	return 0;
}
