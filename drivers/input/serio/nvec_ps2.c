#include <linux/slab.h>
#include <linux/serio.h>

extern void nvec_add_handler(unsigned char type, void (*got_event)(unsigned char *data, unsigned char size));
extern const char *nvec_send_msg(unsigned char *src, unsigned char src_size, unsigned char *dst_size, int care_resp);
extern void nvec_release_msg(void);

static struct serio *ser_dev;
static int ps2_startstreaming(struct serio *ser_dev) {
	nvec_send_msg("\x03\x06\x03\x01", 0, NULL, 0);
	return 0;
}

static void ps2_stopstreaming(struct serio *ser_dev) {
	nvec_send_msg("\x02\x06\x04", 0, NULL, 0);
}

static int ps2_sendcommand(struct serio *ser_dev, unsigned char cmd) {
	unsigned char *buf="\x04\x06\x01\xf4\x00";
	const unsigned char *ret;
	unsigned char size;
	buf[3]=cmd;
	ret=nvec_send_msg(buf, 0, &size, 1);
	if(size>=5)
		serio_interrupt(ser_dev, ret[4], 0);
	nvec_release_msg();
	return 0;
}

static void parse_auxevent(unsigned char *data, unsigned char size) {
	serio_interrupt(ser_dev, data[2], 0);
}

static int __init nvec_ps2(void) {
	ser_dev=kzalloc(sizeof(struct serio), GFP_KERNEL);
	ser_dev->id.type=SERIO_8042;
	ser_dev->write=ps2_sendcommand;
	ser_dev->open=ps2_startstreaming;
	ser_dev->close=ps2_stopstreaming;
	strlcpy(ser_dev->name, "NVEC PS2", sizeof(ser_dev->name));
	strlcpy(ser_dev->phys, "NVEC I2C slave", sizeof(ser_dev->phys));
	nvec_add_handler(0x1/* NvEcEventType_AuxDevice0 */, parse_auxevent);

	serio_register_port(ser_dev);
	return 0;
}

module_init(nvec_ps2);
