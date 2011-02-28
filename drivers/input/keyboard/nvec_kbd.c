#include <linux/slab.h>
#include <linux/input.h>
#include <linux/delay.h>
#include "nvec-keytable.h"

extern void nvec_add_handler(unsigned char type, void (*got_event)(unsigned char *data, unsigned char size));
typedef enum {
	NOT_REALLY,
	YES,
	NOT_AT_ALL,
} how_care;
const char *nvec_send_msg(unsigned char *src, unsigned char *dst_size, how_care care_resp, void (*rt_handler)(unsigned char *data));
typedef enum {
	NVEC_2BYTES,
	NVEC_3BYTES,
	NVEC_VAR_SIZE
} nvec_size;
static struct input_dev *idev;
static unsigned char keycodes[ARRAY_SIZE(code_tab_102us)+ARRAY_SIZE(extcode_tab_us102)];

static void nvec_event(unsigned char *data, unsigned char size) {
	nvec_size _size=(data[0]&(3<<5))>>5;
	if(_size==NVEC_3BYTES)
		input_report_key(idev, extcode_tab_us102[(data[2]&0x7f)-0x10], !(data[2]&0x80));
	else
		input_report_key(idev, code_tab_102us[(data[1]&0x7f)], !(data[1]&0x80));
}

static int nvec_kbd_event(struct input_dev *dev, unsigned int type, unsigned int code, int value) {
	char *buf="\x03\x05\xed\x01";
	if(type==EV_REP)
		return 0;
	if(type!=EV_LED)
		return -1;
	if(code!=LED_CAPSL)
		return -1;
	buf[3]=!!value;
	nvec_send_msg(buf, NULL, NOT_REALLY, NULL);
	return 0;
}

int __init nvec_kbd_init(void)
{
	int i,j;
	j=0;
	for(i=0;i<ARRAY_SIZE(code_tab_102us);++i)
		keycodes[j++]=code_tab_102us[i];
	for(i=0;i<ARRAY_SIZE(extcode_tab_us102);++i)
		keycodes[j++]=extcode_tab_us102[i];

	idev=input_allocate_device();
	idev->name="Tegra nvec keyboard";
	idev->phys="i2c3_slave/nvec";
	idev->evbit[0]=BIT_MASK(EV_KEY)|BIT_MASK(EV_REP)|BIT_MASK(EV_LED);
	idev->ledbit[0]=BIT_MASK(LED_CAPSL);
	idev->event=nvec_kbd_event;
	idev->keycode=keycodes;
	idev->keycodesize=sizeof(unsigned char);
	idev->keycodemax=ARRAY_SIZE(keycodes);
	for(i=0;i<ARRAY_SIZE(keycodes);++i)
		set_bit(keycodes[i], idev->keybit);
	clear_bit(0, idev->keybit);
	input_register_device(idev);
	nvec_add_handler(0, nvec_event);

	//Get extra events (AC, battery, power button)
	nvec_send_msg("\x07\x01\x01\x01\xff\xff\xff\xff", NULL, NOT_REALLY, NULL);
	//Enable keyboard
	nvec_send_msg("\x02\x05\xf4", NULL, NOT_REALLY, NULL);
	//Enable..... mouse ?
	nvec_send_msg("\x03\x06\x01\xf4\x00", NULL, NOT_REALLY, NULL);
	//Mouse shut up
	nvec_send_msg("\x02\x06\x04", NULL, NOT_REALLY, NULL);

	return 0;
}
