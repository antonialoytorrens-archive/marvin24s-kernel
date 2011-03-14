#ifndef __LINUX_MFD_NVEC
#define __LINUX_MFD_NVEC

typedef enum {
	NVEC_2BYTES,
	NVEC_3BYTES,
	NVEC_VAR_SIZE
} nvec_size;

typedef enum {
	NOT_REALLY,
	YES,
	NOT_AT_ALL,
} how_care;

typedef enum {
	NVEC_KB,
	NVEC_PS2 = 6,
	NVEC_KB_EVT = 0x80,
	NVEC_PS2_EVT
} nvec_event;

struct nvec_msg {
	unsigned char *data;
	unsigned short size;
	unsigned short pos;
	struct list_head node;
};

struct nvec_subdev {
	const char *name;
	void *platform_data;
};

struct nvec_platform_data {
	int num_subdevs;
	int i2c_addr;
	int gpio;
	struct nvec_subdev *subdevs;
};

extern void nvec_write_async(unsigned char *data, short size);

extern int nvec_register_notifier(struct device *dev,
		 struct notifier_block *nb, unsigned int events);

extern int nvec_unregister_notifier(struct device *dev,
		struct notifier_block *nb, unsigned int events);

const char *nvec_send_msg(unsigned char *src, unsigned char *dst_size, how_care care_resp, void (*rt_handler)(unsigned char *data));

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

#endif
