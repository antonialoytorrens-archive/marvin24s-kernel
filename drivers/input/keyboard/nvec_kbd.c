#include <linux/slab.h>
#include <linux/input.h>
#include <linux/delay.h>
#include "nvec-keytable.h"
#include <linux/mfd/nvec.h>

static unsigned char keycodes[ARRAY_SIZE(code_tab_102us)+ARRAY_SIZE(extcode_tab_us102)];

struct nvec_keys {
	struct input_dev *input;
	struct notifier_block notifier;
	struct device *master;
};

static struct nvec_keys keys_dev;

static int nvec_keys_notifier(struct notifier_block *nb,
				 unsigned long event_type, 
		unsigned char *data)
{
	if (event_type == NVEC_KB_EVT) {
		nvec_size _size=(data[0]&(3<<5))>>5;
		if(_size==NVEC_3BYTES)
			data++;
			
		input_report_key(keys_dev.input, code_tab_102us[(data[1]&0x7f)], !(data[1]&0x80));
	}


	return 0;
}

static int nvec_kbd_event(struct input_dev *dev, unsigned int type, unsigned int code, int value) {
	char *buf="\x05\xed\x01";
	if(type==EV_REP)
		return 0;
	if(type!=EV_LED)
		return -1;
	if(code!=LED_CAPSL)
		return -1;
	buf[2]=!!value;
	nvec_write_async(buf, 3);
	return 0;
}

int __init nvec_kbd_init(void)
{
	int i,j;
	struct input_dev *idev;

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

	keys_dev.input = idev;
	keys_dev.notifier.notifier_call = nvec_keys_notifier;
	nvec_register_notifier(NULL, &keys_dev.notifier, 0);

	//Get extra events (AC, battery, power button)
	nvec_write_async("\x01\x01\x01\xff\xff\xff\xff", 7);
	//Enable keyboard
	nvec_write_async("\x05\xf4", 2);
	//Enable..... mouse ?
	nvec_write_async("\x06\x01\xf4\x00", 3); // wtf?
	//Mouse shut up
	nvec_write_async("\x06\x04", 2);

	return 0;
}
