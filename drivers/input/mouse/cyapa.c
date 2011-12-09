/*
 * Cypress APA trackpad with I2C interface
 *
 * Author: Dudley Du <dudl@cypress.com>
 *
 * Copyright (C) 2011 Cypress Semiconductor, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * Based on synaptics_i2c driver:
 *   Copyright (C) 2009 Compulab, Ltd.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive for
 * more details.
 */


#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/i2c/cyapa.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>


/* commands for read/write registers of Cypress trackpad */
#define CYAPA_CMD_SOFT_RESET       0x00
#define CYAPA_CMD_POWER_MODE       0x01
#define CYAPA_CMD_DEV_STATUS       0x02
#define CYAPA_CMD_GROUP_DATA       0x03
#define CYAPA_CMD_GROUP_CTRL       0x04
#define CYAPA_CMD_GROUP_CMD        0x05
#define CYAPA_CMD_GROUP_QUERY      0x06
#define CYAPA_CMD_BL_STATUS        0x07
#define CYAPA_CMD_BL_HEAD          0x08
#define CYAPA_CMD_BL_CMD           0x09
#define CYAPA_CMD_BL_DATA          0x0A
#define CYAPA_CMD_BL_ALL           0x0B
#define CYAPA_CMD_BLK_PRODUCT_ID   0x0C
#define CYAPA_CMD_BLK_HEAD         0x0D

/* report data start reg offset address. */
#define DATA_REG_START_OFFSET  0x0000

#define BL_HEAD_OFFSET 0x00
#define BL_DATA_OFFSET 0x10

/*
 * bit 7: Valid interrupt source
 * bit 6 - 4: Reserved
 * bit 3 - 2: Power status
 * bit 1 - 0: Device status
 */
#define REG_OP_STATUS     0x00
#define OP_STATUS_SRC     0x80
#define OP_STATUS_POWER   0x0C
#define OP_STATUS_DEV     0x03
#define OP_STATUS_MASK (OP_STATUS_SRC | OP_STATUS_POWER | OP_STATUS_DEV)

/*
 * bit 7 - 4: Number of touched finger
 * bit 3: Valid data
 * bit 2: Middle Physical Button
 * bit 1: Right Physical Button
 * bit 0: Left physical Button
 */
#define REG_OP_DATA1       0x01
#define OP_DATA_VALID      0x08
#define OP_DATA_MIDDLE_BTN 0x04
#define OP_DATA_RIGHT_BTN  0x02
#define OP_DATA_LEFT_BTN   0x01
#define OP_DATA_BTN_MASK (OP_DATA_MIDDLE_BTN | OP_DATA_RIGHT_BTN | OP_DATA_LEFT_BTN)

/*
 * bit 7: Busy
 * bit 6 - 5: Reserved
 * bit 4: Bootloader running
 * bit 3 - 1: Reserved
 * bit 0: Checksum valid
 */
#define REG_BL_STATUS        0x01
#define BL_STATUS_BUSY       0x80
#define BL_STATUS_RUNNING    0x10
#define BL_STATUS_DATA_VALID 0x08
#define BL_STATUS_CSUM_VALID 0x01
/*
 * bit 7: Invalid
 * bit 6: Invalid security key
 * bit 5: Bootloading
 * bit 4: Command checksum
 * bit 3: Flash protection error
 * bit 2: Flash checksum error
 * bit 1 - 0: Reserved
 */
#define REG_BL_ERROR         0x02
#define BL_ERROR_INVALID     0x80
#define BL_ERROR_INVALID_KEY 0x40
#define BL_ERROR_BOOTLOADING 0x20
#define BL_ERROR_CMD_CSUM    0x10
#define BL_ERROR_FLASH_PROT  0x08
#define BL_ERROR_FLASH_CSUM  0x04

#define REG_BL_KEY1 0x0D
#define REG_BL_KEY2 0x0E
#define REG_BL_KEY3 0x0F
#define BL_KEY1 0xC0
#define BL_KEY2 0xC1
#define BL_KEY3 0xC2

#define BL_STATUS_SIZE  3  /* length of bootloader status registers */
#define BLK_HEAD_BYTES 32

/* Macro for register map group offset. */
#define CYAPA_REG_MAP_SIZE  256

#define PRODUCT_ID_SIZE  16
#define QUERY_DATA_SIZE  27
#define REG_PROTOCOL_GEN_QUERY_OFFSET  20

#define REG_OFFSET_DATA_BASE     0x0000
#define REG_OFFSET_CONTROL_BASE  0x0000
#define REG_OFFSET_COMMAND_BASE  0x0028
#define REG_OFFSET_QUERY_BASE    0x002A

#define CYAPA_OFFSET_SOFT_RESET  REG_OFFSET_COMMAND_BASE

#define REG_OFFSET_POWER_MODE (REG_OFFSET_COMMAND_BASE + 1)
#define OP_POWER_MODE_MASK     0xC0
#define OP_POWER_MODE_SHIFT    6
#define PWR_MODE_FULL_ACTIVE   3
#define PWR_MODE_LIGHT_SLEEP   2
#define PWR_MODE_MEDIUM_SLEEP  1
#define PWR_MODE_DEEP_SLEEP    0
#define SET_POWER_MODE_DELAY   10000  /* unit: us */

/*
 * CYAPA trackpad device states.
 * Used in register 0x00, bit1-0, DeviceStatus field.
 * After trackpad boots, and can report data, it sets this value.
 * Other values indicate device is in an abnormal state and must be reset.
 */
#define CYAPA_DEV_NORMAL  0x03

enum cyapa_state {
	CYAPA_STATE_OP,
	CYAPA_STATE_BL_IDLE,
	CYAPA_STATE_BL_ACTIVE,
	CYAPA_STATE_BL_BUSY,
	CYAPA_STATE_NO_DEVICE,
};


struct cyapa_touch {
	/*
	 * high bits or x/y position value
	 * bit 7 - 4: high 4 bits of x position value
	 * bit 3 - 0: high 4 bits of y position value
	 */
	u8 xy;
	u8 x;  /* low 8 bits of x position value. */
	u8 y;  /* low 8 bits of y position value. */
	u8 pressure;
	/* id range is 1 - 15.  It is incremented with every new touch. */
	u8 id;
} __packed;

/* The touch.id is used as the MT slot id, thus max MT slot is 15 */
#define CYAPA_MAX_MT_SLOTS  15

/* CYAPA reports up to 5 touches per packet. */
#define CYAPA_MAX_TOUCHES  5

struct cyapa_reg_data {
	/*
	 * bit 0 - 1: device status
	 * bit 3 - 2: power mode
	 * bit 6 - 4: reserved
	 * bit 7: interrupt valid bit
	 */
	u8 device_status;
	/*
	 * bit 7 - 4: number of fingers currently touching pad
	 * bit 3: valid data check bit
	 * bit 2: middle mechanism button state if exists
	 * bit 1: right mechanism button state if exists
	 * bit 0: left mechanism button state if exists
	 */
	u8 finger_btn;
	struct cyapa_touch touches[CYAPA_MAX_TOUCHES];
} __packed;

/* The main device structure */
struct cyapa {
	/* synchronize accessing members of cyapa data structure. */
	spinlock_t miscdev_spinlock;
	/* synchronize accessing and updating file->f_pos. */
	struct mutex misc_mutex;
	int misc_open_count;

	enum cyapa_state state;

	struct i2c_client	*client;
	struct input_dev	*input;
	struct work_struct detect_work;
	struct workqueue_struct *detect_wq;
	int irq;
	u8 adapter_func;
	bool smbus;

	/* read from query data region. */
	char product_id[16];
	u8 capability[14];
	u8 fw_maj_ver;  /* firmware major version. */
	u8 fw_min_ver;  /* firmware minor version. */
	u8 hw_maj_ver;  /* hardware major version. */
	u8 hw_min_ver;  /* hardware minor version. */
	enum cyapa_gen gen;
	int max_abs_x;
	int max_abs_y;
	int physical_size_x;
	int physical_size_y;
};

static const u8 bl_activate[] = { 0x00, 0xFF, 0x38, 0x00, 0x01, 0x02, 0x03,
		0x04, 0x05, 0x06, 0x07 };
static const u8 bl_deactivate[] = { 0x00, 0xFF, 0x3B, 0x00, 0x01, 0x02, 0x03,
		0x04, 0x05, 0x06, 0x07 };
static const u8 bl_exit[] = { 0x00, 0xFF, 0xA5, 0x00, 0x01, 0x02, 0x03, 0x04,
		0x05, 0x06, 0x07 };

/* global pointer to trackpad touch data structure. */
static struct cyapa *global_cyapa;

struct cyapa_cmd_len {
	unsigned char cmd;
	unsigned char len;
};

#define CYAPA_ADAPTER_FUNC_NONE   0
#define CYAPA_ADAPTER_FUNC_I2C    1
#define CYAPA_ADAPTER_FUNC_SMBUS  2
#define CYAPA_ADAPTER_FUNC_BOTH   3

#define CYTP_I2C 0
#define CYTP_SMBUS 1

/*
 * macros for SMBus communication
 */
#define SMBUS_READ   0x01
#define SMBUS_WRITE 0x00
#define SMBUS_ENCODE_IDX(cmd, idx) ((cmd) | (((idx) & 0x03) << 1))
#define SMBUS_ENCODE_RW(cmd, rw) ((cmd) | ((rw) & 0x01))
#define SMBUS_BYTE_BLOCK_CMD_MASK 0x80
#define SMBUS_GROUP_BLOCK_CMD_MASK 0x40

 /* for byte read/write command */
#define CMD_RESET 0
#define CMD_POWER_MODE 1
#define CMD_DEV_STATUS 2
#define SMBUS_BYTE_CMD(cmd) (((cmd) & 0x3F) << 1)
#define CYAPA_SMBUS_RESET SMBUS_BYTE_CMD(CMD_RESET)
#define CYAPA_SMBUS_POWER_MODE SMBUS_BYTE_CMD(CMD_POWER_MODE)
#define CYAPA_SMBUS_DEV_STATUS SMBUS_BYTE_CMD(CMD_DEV_STATUS)

 /* for group registers read/write command */
#define REG_GROUP_DATA 0
#define REG_GROUP_CTRL 1
#define REG_GROUP_CMD 2
#define REG_GROUP_QUERY 3
#define SMBUS_GROUP_CMD(grp) (0x80 | (((grp) & 0x07) << 3))
#define CYAPA_SMBUS_GROUP_DATA SMBUS_GROUP_CMD(REG_GROUP_DATA)
#define CYAPA_SMBUS_GROUP_CTRL SMBUS_GROUP_CMD(REG_GROUP_CTRL)
#define CYAPA_SMBUS_GROUP_CMD SMBUS_GROUP_CMD(REG_GROUP_CMD)
#define CYAPA_SMBUS_GROUP_QUERY SMBUS_GROUP_CMD(REG_GROUP_QUERY)

 /* for register block read/write command */
#define CMD_BL_STATUS 0
#define CMD_BL_HEAD 1
#define CMD_BL_CMD 2
#define CMD_BL_DATA 3
#define CMD_BL_ALL 4
#define CMD_BLK_PRODUCT_ID 5
#define CMD_BLK_HEAD 6
#define SMBUS_BLOCK_CMD(cmd) (0xC0 | (((cmd) & 0x1F) << 1))
/* register block read/write command in bootloader mode */
#define CYAPA_SMBUS_BL_STATUS SMBUS_BLOCK_CMD(CMD_BL_STATUS)
#define CYAPA_SMBUS_BL_HEAD SMBUS_BLOCK_CMD(CMD_BL_HEAD)
#define CYAPA_SMBUS_BL_CMD SMBUS_BLOCK_CMD(CMD_BL_CMD)
#define CYAPA_SMBUS_BL_DATA SMBUS_BLOCK_CMD(CMD_BL_DATA)
#define CYAPA_SMBUS_BL_ALL SMBUS_BLOCK_CMD(CMD_BL_ALL)
/* register block read/write command in operational mode */
#define CYAPA_SMBUS_BLK_PRODUCT_ID SMBUS_BLOCK_CMD(CMD_BLK_PRODUCT_ID)
#define CYAPA_SMBUS_BLK_HEAD SMBUS_BLOCK_CMD(CMD_BLK_HEAD)

static const struct cyapa_cmd_len cyapa_i2c_cmds[] = {
	{CYAPA_OFFSET_SOFT_RESET, 1},
	{REG_OFFSET_COMMAND_BASE + 1, 1},
	{REG_OFFSET_DATA_BASE, 1},
	{REG_OFFSET_DATA_BASE, sizeof(struct cyapa_reg_data)},
	{REG_OFFSET_CONTROL_BASE, 0},
	{REG_OFFSET_COMMAND_BASE, 0},
	{REG_OFFSET_QUERY_BASE, QUERY_DATA_SIZE},
	{BL_HEAD_OFFSET, 3},
	{BL_HEAD_OFFSET, 16},
	{BL_HEAD_OFFSET, 16},
	{BL_DATA_OFFSET, 16},
	{BL_HEAD_OFFSET, 32},
	{REG_OFFSET_QUERY_BASE, PRODUCT_ID_SIZE},
	{REG_OFFSET_DATA_BASE, 32}
};

static const struct cyapa_cmd_len cyapa_smbus_cmds[] = {
	{CYAPA_SMBUS_RESET, 1},
	{CYAPA_SMBUS_POWER_MODE, 1},
	{CYAPA_SMBUS_DEV_STATUS, 1},
	{CYAPA_SMBUS_GROUP_DATA, sizeof(struct cyapa_reg_data)},
	{CYAPA_SMBUS_GROUP_CTRL, 0},
	{CYAPA_SMBUS_GROUP_CMD, 2},
	{CYAPA_SMBUS_GROUP_QUERY, QUERY_DATA_SIZE},
	{CYAPA_SMBUS_BL_STATUS, 3},
	{CYAPA_SMBUS_BL_HEAD, 16},
	{CYAPA_SMBUS_BL_CMD, 16},
	{CYAPA_SMBUS_BL_DATA, 16},
	{CYAPA_SMBUS_BL_ALL, 32},
	{CYAPA_SMBUS_BLK_PRODUCT_ID, PRODUCT_ID_SIZE},
	{CYAPA_SMBUS_BLK_HEAD, 16},
};

static void cyapa_detect(struct cyapa *cyapa);

#define BYTE_PER_LINE  8
void cyapa_dump_data(struct cyapa *cyapa, size_t length, const u8 *data)
{
	struct device *dev = &cyapa->client->dev;
	int i;
	char buf[BYTE_PER_LINE * 3 + 1];
	char *s = buf;

	for (i = 0; i < length; i++) {
		s += sprintf(s, " %02x", data[i]);
		if ((i + 1) == length || ((i + 1) % BYTE_PER_LINE) == 0) {
			dev_dbg(dev, "%s\n", buf);
			s = buf;
		}
	}
}
#undef BYTE_PER_LINE

/*
 * cyapa_i2c_reg_read_block - read a block of data from device i2c registers
 * @cyapa  - private data structure of driver
 * @reg    - register at which to start reading
 * @length - length of block to read, in bytes
 * @values - buffer to store values read from registers
 *
 * Returns negative errno, else number of bytes read.
 *
 * Note: The trackpad register block is 256 bytes.
 */
static ssize_t cyapa_i2c_reg_read_block(struct cyapa *cyapa, u8 reg, size_t len,
					u8 *values)
{
	struct device *dev = &cyapa->client->dev;
	ssize_t ret;

	ret = i2c_smbus_read_i2c_block_data(cyapa->client, reg, len, values);
	dev_dbg(dev, "i2c read block reg: 0x%02x len: %d ret: %d\n",
		reg, len, ret);
	if (ret > 0)
		cyapa_dump_data(cyapa, ret, values);

	return ret;
}

/*
 * cyapa_i2c_reg_write_block - write a block of data to device i2c registers
 * @cyapa  - private data structure of driver
 * @reg    - register at which to start writing
 * @length - length of block to write, in bytes
 * @values - buffer to write to i2c registers
 *
 * Returns 0 on success, else negative errno on failure.
 *
 * Note: The trackpad register block is 256 bytes.
 */
static ssize_t cyapa_i2c_reg_write_block(struct cyapa *cyapa, u8 reg,
					 size_t len, const u8 *values)
{
	struct device *dev = &cyapa->client->dev;
	ssize_t ret;

	ret = i2c_smbus_write_i2c_block_data(cyapa->client, reg, len, values);
	dev_dbg(dev, "i2c write block reg: 0x%02x len: %d ret: %d\n",
		reg, len, ret);
	cyapa_dump_data(cyapa, len, values);

	return ret;
}

/*
 * cyapa_smbus_read_block - perform smbus block read command
 * @cyapa  - private data structure of the driver
 * @cmd    - the properly encoded smbus command
 * @length - expected length of smbus command result
 * @values - buffer to store smbus command result
 *
 * Returns negative errno, else the number of bytes written.
 *
 * Note:
 * In trackpad device, the memory block allocated for I2C register map
 * is 256 bytes, so the max read block for I2C bus is 256 bytes.
 */
static ssize_t cyapa_smbus_read_block(struct cyapa *cyapa, u8 cmd, size_t len,
				      u8 *values)
{
	ssize_t ret;
	u8 index;
	u8 smbus_cmd;
	u8 *buf;
	struct i2c_client *client = cyapa->client;
	struct device *dev = &client->dev;

	if (!(SMBUS_BYTE_BLOCK_CMD_MASK & cmd))
		return -EINVAL;

	if (SMBUS_GROUP_BLOCK_CMD_MASK & cmd) {
		/* read specific block registers command. */
		smbus_cmd = SMBUS_ENCODE_RW(cmd, SMBUS_READ);
		ret = i2c_smbus_read_block_data(client, smbus_cmd, values);
		goto out;
	}

	ret = 0;
	for (index = 0; index * I2C_SMBUS_BLOCK_MAX < len; index++) {
		smbus_cmd = SMBUS_ENCODE_IDX(cmd, index);
		smbus_cmd = SMBUS_ENCODE_RW(smbus_cmd, SMBUS_READ);
		buf = values + I2C_SMBUS_BLOCK_MAX * index;
		ret = i2c_smbus_read_block_data(client, smbus_cmd, buf);
		if (ret < 0)
			goto out;
	}

out:
	dev_dbg(dev, "smbus read block cmd: 0x%02x len: %d ret: %d\n",
		cmd, len, ret);
	if (ret > 0)
		cyapa_dump_data(cyapa, len, values);
	return (ret > 0) ? len : ret;
}

static s32 cyapa_read_byte(struct cyapa *cyapa, u8 cmd_idx)
{
	struct device *dev = &cyapa->client->dev;
	int ret;
	u8 cmd;

	if (cyapa->smbus) {
		cmd = cyapa_smbus_cmds[cmd_idx].cmd;
		cmd = SMBUS_ENCODE_RW(cmd, SMBUS_READ);
	} else {
		cmd = cyapa_i2c_cmds[cmd_idx].cmd;
	}
	ret = i2c_smbus_read_byte_data(cyapa->client, cmd);
	dev_dbg(dev, "read byte [0x%02x] = 0x%02x  ret: %d\n",
		cmd, ret, ret);

	return ret;
}

static s32 cyapa_write_byte(struct cyapa *cyapa, u8 cmd_idx, u8 value)
{
	struct device *dev = &cyapa->client->dev;
	int ret;
	u8 cmd;

	if (cyapa->smbus) {
		cmd = cyapa_smbus_cmds[cmd_idx].cmd;
		cmd = SMBUS_ENCODE_RW(cmd, SMBUS_WRITE);
	} else {
		cmd = cyapa_i2c_cmds[cmd_idx].cmd;
	}
	ret = i2c_smbus_write_byte_data(cyapa->client, cmd, value);
	dev_dbg(dev, "write byte [0x%02x] = 0x%02x  ret: %d\n",
		cmd, value, ret);

	return ret;
}

static ssize_t cyapa_read_block(struct cyapa *cyapa, u8 cmd_idx, u8 *values)
{
	u8 cmd;
	size_t len;

	if (cyapa->smbus) {
		cmd = cyapa_smbus_cmds[cmd_idx].cmd;
		len = cyapa_smbus_cmds[cmd_idx].len;
		return cyapa_smbus_read_block(cyapa, cmd, len, values);
	} else {
		cmd = cyapa_i2c_cmds[cmd_idx].cmd;
		len = cyapa_i2c_cmds[cmd_idx].len;
		return cyapa_i2c_reg_read_block(cyapa, cmd, len, values);
	}
}

/*
 * Query device for its current operating state.
 *
 * Possible states are:
 *   OPERATION_MODE
 *   BOOTLOADER_IDLE
 *   BOOTLOADER_ACTIVE
 *   BOOTLOADER_BUSY
 *   NO_DEVICE
 *
 * Returns:
 *   0 on success, and sets cyapa->state
 *   < 0 on error, and sets cyapa->state = CYAPA_STATE_NO_DEVICE
 */
static int cyapa_get_state(struct cyapa *cyapa)
{
	struct device *dev = &cyapa->client->dev;
	int ret;
	u8 status[BL_STATUS_SIZE];

	cyapa->state = CYAPA_STATE_NO_DEVICE;

	/*
	 * Get trackpad status by reading 3 registers starting from 0.
	 * If the device is in the bootloader, this will be BL_HEAD.
	 * If the device is in operation mode, this will be the DATA regs.
	 *
	 * Note: on SMBus, this may be slow.
	 * TODO(djkurtz): make it fast on SMBus!
	 */
	ret = cyapa_i2c_reg_read_block(cyapa, BL_HEAD_OFFSET, BL_STATUS_SIZE,
				       status);

	/*
	 * On smbus systems in OP mode, the i2c_reg_read will fail with
	 * -ETIMEDOUT.  In this case, try again using the smbus equivalent
	 * command.  This should return a BL_HEAD indicating CYAPA_STATE_OP.
	 */
	if (cyapa->smbus && ret == -ETIMEDOUT) {
		dev_dbg(dev, "smbus: probing with BL_STATUS command\n");
		ret = cyapa_read_block(cyapa, CYAPA_CMD_BL_STATUS, status);
	}

	if (ret != BL_STATUS_SIZE)
		return (ret < 0) ? ret : -EAGAIN;

	if ((status[REG_OP_STATUS] & OP_STATUS_DEV) == CYAPA_DEV_NORMAL) {
		dev_dbg(dev, "device state: operational mode\n");
		cyapa->state = CYAPA_STATE_OP;
	} else if (status[REG_BL_STATUS] & BL_STATUS_BUSY) {
		dev_dbg(dev, "device state: bootloader busy\n");
		cyapa->state = CYAPA_STATE_BL_BUSY;
	} else if (status[REG_BL_ERROR] & BL_ERROR_BOOTLOADING) {
		dev_dbg(dev, "device state: bootloader active\n");
		cyapa->state = CYAPA_STATE_BL_ACTIVE;
	} else {
		dev_dbg(dev, "device state: bootloader idle\n");
		cyapa->state = CYAPA_STATE_BL_IDLE;
	}

	return 0;
}
/*
 * Poll device for its status in a loop, waiting up to timeout for a response.
 *
 * When the device switches state, it usually takes ~300 ms.
 * Howere, when running a new firmware image, the device must calibrate its
 * sensors, which can take as long as 2 seconds.
 *
 * Note: The timeout has granularity of the polling rate, which is 300 ms.
 *
 * Returns:
 *   0 when the device eventually responds with a valid non-busy state.
 *   -ETIMEDOUT if device never responds (too many -EAGAIN)
 *   < 0    other errors
 */
static int cyapa_poll_state(struct cyapa *cyapa, unsigned int timeout)
{
	int ret;
	int tries = timeout / 100;

	ret = cyapa_get_state(cyapa);
	while ((ret || cyapa->state >= CYAPA_STATE_BL_BUSY) && tries--) {
		msleep(100);
		ret = cyapa_get_state(cyapa);
	}
	return (ret == -EAGAIN || ret == -ETIMEDOUT) ? -ETIMEDOUT : ret;
}

/*
 * Enter bootloader by soft resetting the device.
 *
 * If device is already in the bootloader, the function just returns.
 * Otherwise, reset the device; after reset, device enters bootloader idle
 * state immediately.
 *
 * Also, if device was unregister device from input core.  Device will
 * re-register after it is detected following resumption of operational mode.
 *
 * Returns:
 *   0 on success
 *   -EAGAIN  device was reset, but is not now in bootloader idle state
 *   < 0 if the device never responds within the timeout
 */
static int cyapa_bl_enter(struct cyapa *cyapa)
{
	int ret;

	if (cyapa->input) {
		disable_irq(cyapa->irq);
		input_unregister_device(cyapa->input);
		cyapa->input = NULL;
	}

	if (cyapa->state != CYAPA_STATE_OP)
		return 0;

	cyapa->state = CYAPA_STATE_NO_DEVICE;
	ret = cyapa_write_byte(cyapa, CYAPA_CMD_SOFT_RESET, 0x01);

	if (ret < 0)
		return -EIO;

	ret = cyapa_get_state(cyapa);
	if (ret < 0)
		return ret;
	if (cyapa->state != CYAPA_STATE_BL_IDLE)
		return -EAGAIN;

	return 0;
}
static int cyapa_bl_activate(struct cyapa *cyapa)
{
	int ret;

	ret = cyapa_i2c_reg_write_block(cyapa, 0, sizeof(bl_activate),
					bl_activate);
	if (ret < 0)
		return ret;

	/* Wait for bootloader to activate; takes between 2 and 12 seconds */
	msleep(2000);
	ret = cyapa_poll_state(cyapa, 10000);
	if (ret < 0)
		return ret;
	if (cyapa->state != CYAPA_STATE_BL_ACTIVE)
		return -EAGAIN;

	return 0;
}

static int cyapa_bl_deactivate(struct cyapa *cyapa)
{
	int ret;

	ret = cyapa_i2c_reg_write_block(cyapa, 0, sizeof(bl_deactivate),
					bl_deactivate);
	if (ret < 0)
		return ret;

	/* wait for bootloader to switch to idle state; should take < 100ms */
	msleep(100);
	ret = cyapa_poll_state(cyapa, 500);
	if (ret < 0)
		return ret;
	if (cyapa->state != CYAPA_STATE_BL_IDLE)
		return -EAGAIN;
	return 0;
}

/*
 * Exit bootloader
 *
 * Send bl_exit command, then wait 300 ms to let device transition to
 * operational mode.  If this is the first time the device's firmware is
 * running, it can take up to 2 seconds to calibrate its sensors.  So, poll
 * the device's new state for up to 2 seconds.
 *
 * Returns:
 *   -EIO    failure while reading from device
 *   -EAGAIN device is stuck in bootloader, b/c it has invalid firmware
 *   0       device is supported and in operational mode
 */
static int cyapa_bl_exit(struct cyapa *cyapa)
{
	int ret;

	ret = cyapa_i2c_reg_write_block(cyapa, 0, sizeof(bl_exit), bl_exit);
	if (ret < 0)
		return ret;

	/*
	 * Wait for bootloader to exit, and operation mode to start.
	 * Normally, this takes at least 50 ms.
	 */
	usleep_range(50000, 100000);
	/*
	 * In addition, when a device boots for the first time after being
	 * updated to new firmware, it must first calibrate its sensors, which
	 * can take up to an additional 2 seconds.
	 */
	ret = cyapa_poll_state(cyapa, 2000);
	if (ret < 0)
		return ret;
	if (cyapa->state != CYAPA_STATE_OP)
		return -EAGAIN;

	return 0;
}

/*
 * Set device power mode
 *
 * Device power mode can only be set when device is in operational mode.
 */
static int cyapa_set_power_mode(struct cyapa *cyapa, u8 power_mode)
{
	int ret;
	u8 power;
	int tries = 3;

	if (cyapa->state != CYAPA_STATE_OP)
		return 0;

	power = cyapa_read_byte(cyapa, CYAPA_CMD_POWER_MODE);
	power &= ~OP_POWER_MODE_MASK;
	power |= ((power_mode << OP_POWER_MODE_SHIFT) & OP_POWER_MODE_MASK);
	do {
		ret = cyapa_write_byte(cyapa, CYAPA_CMD_POWER_MODE, power);
		/* sleep at least 10 ms. */
		usleep_range(SET_POWER_MODE_DELAY, 2 * SET_POWER_MODE_DELAY);
	} while ((ret != 0) && (tries-- > 0));

	return ret;
}

static int cyapa_get_query_data(struct cyapa *cyapa)
{
	u8 query_data[QUERY_DATA_SIZE];
	int ret;

	if (cyapa->state != CYAPA_STATE_OP)
		return -EBUSY;

	ret = cyapa_read_block(cyapa, CYAPA_CMD_GROUP_QUERY, query_data);
	if (ret < 0)
		return ret;
	if (ret != QUERY_DATA_SIZE)
		return -EIO;

	cyapa->product_id[0] = query_data[0];
	cyapa->product_id[1] = query_data[1];
	cyapa->product_id[2] = query_data[2];
	cyapa->product_id[3] = query_data[3];
	cyapa->product_id[4] = query_data[4];
	cyapa->product_id[5] = '-';
	cyapa->product_id[6] = query_data[5];
	cyapa->product_id[7] = query_data[6];
	cyapa->product_id[8] = query_data[7];
	cyapa->product_id[9] = query_data[8];
	cyapa->product_id[10] = query_data[9];
	cyapa->product_id[11] = query_data[10];
	cyapa->product_id[12] = '-';
	cyapa->product_id[13] = query_data[11];
	cyapa->product_id[14] = query_data[12];
	cyapa->product_id[15] = '\0';

	cyapa->fw_maj_ver = query_data[15];
	cyapa->fw_min_ver = query_data[16];
	cyapa->hw_maj_ver = query_data[17];
	cyapa->hw_min_ver = query_data[18];

	cyapa->gen = query_data[20] & 0x0F;

	cyapa->max_abs_x = ((query_data[21] & 0xF0) << 4) | query_data[22];
	cyapa->max_abs_y = ((query_data[21] & 0x0F) << 8) | query_data[23];

	cyapa->physical_size_x =
		((query_data[24] & 0xF0) << 4) | query_data[25];
	cyapa->physical_size_y =
		((query_data[24] & 0x0F) << 8) | query_data[26];

	return 0;
}

/*
 * Check if device is operational.
 *
 * An operational device is responding, has exited bootloader, and has
 * firmware supported by this driver.
 *
 * Returns:
 *   -EBUSY  no device or in bootloader
 *   -EIO    failure while reading from device
 *   -EAGAIN device is still in bootloader
 *           if ->state = CYAPA_STATE_BL_IDLE, device has invalid firmware
 *   -EINVAL device is in operational mode, but not supported by this driver
 *   0       device is supported
 */
static int cyapa_check_is_operational(struct cyapa *cyapa)
{
	struct device *dev = &cyapa->client->dev;
	const char unique_str[] = "CYTRA";
	int ret;

	ret = cyapa_poll_state(cyapa, 2000);
	if (ret < 0)
		return ret;
	switch (cyapa->state) {
	case CYAPA_STATE_BL_ACTIVE:
		ret = cyapa_bl_deactivate(cyapa);
		if (ret)
			return ret;

	/* Fallthrough state */
	case CYAPA_STATE_BL_IDLE:
		ret = cyapa_bl_exit(cyapa);
		if (ret)
			return ret;

	/* Fallthrough state */
	case CYAPA_STATE_OP:
		ret = cyapa_get_query_data(cyapa);
		if (ret < 0)
			return ret;

		/* only support firmware protocol gen3 */
		if (cyapa->gen != CYAPA_GEN3) {
			dev_err(dev, "unsupported protocol version (%d)",
				cyapa->gen);
			return -EINVAL;
		}

		/* only support product ID starting with CYTRA */
		if (memcmp(cyapa->product_id, unique_str,
			   sizeof(unique_str) - 1)) {
			dev_err(dev, "unsupported product ID (%s)\n",
				cyapa->product_id);
			return -EINVAL;
		}
		return 0;

	default:
		return -EIO;
	}
	return 0;
}

/*
 **************************************************************
 * misc cyapa device for trackpad firmware update,
 * and for raw read/write operations.
 * The following programs may open and use cyapa device.
 * 1. X Input Driver.
 * 2. trackpad firmware update program.
 **************************************************************
 */
static int cyapa_misc_open(struct inode *inode, struct file *file)
{
	int count;
	unsigned long flags;
	struct cyapa *cyapa = global_cyapa;

	if (cyapa == NULL)
		return -ENODEV;
	file->private_data = (void *)cyapa;

	spin_lock_irqsave(&cyapa->miscdev_spinlock, flags);
	if (cyapa->misc_open_count) {
		spin_unlock_irqrestore(&cyapa->miscdev_spinlock, flags);
		return -EBUSY;
	}
	count = ++cyapa->misc_open_count;
	spin_unlock_irqrestore(&cyapa->miscdev_spinlock, flags);

	return 0;
}

static int cyapa_misc_close(struct inode *inode, struct file *file)
{
	int count;
	unsigned long flags;
	struct cyapa *cyapa = (struct cyapa *)file->private_data;

	spin_lock_irqsave(&cyapa->miscdev_spinlock, flags);
	count = --cyapa->misc_open_count;
	spin_unlock_irqrestore(&cyapa->miscdev_spinlock, flags);

	return 0;
}

static int cyapa_pos_validate(unsigned int pos)
{
	return (pos >= 0) && (pos < CYAPA_REG_MAP_SIZE);
}

static loff_t cyapa_misc_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t ret = -EINVAL;
	struct cyapa *cyapa = (struct cyapa *)file->private_data;
	struct device *dev = &cyapa->client->dev;

	if (cyapa == NULL) {
		dev_err(dev, "cypress trackpad device does not exit.\n");
		return -ENODEV;
	}

	mutex_lock(&cyapa->misc_mutex);
	switch (origin) {
	case SEEK_SET:
		if (cyapa_pos_validate(offset)) {
			file->f_pos = offset;
			ret = file->f_pos;
		}
		break;

	case SEEK_CUR:
		if (cyapa_pos_validate(file->f_pos + offset)) {
			file->f_pos += offset;
			ret = file->f_pos;
		}
		break;

	case SEEK_END:
		if (cyapa_pos_validate(CYAPA_REG_MAP_SIZE + offset)) {
			file->f_pos = (CYAPA_REG_MAP_SIZE + offset);
			ret = file->f_pos;
		}
		break;

	default:
		break;
	}
	mutex_unlock(&cyapa->misc_mutex);

	return ret;
}

static int cyapa_miscdev_rw_params_check(struct cyapa *cyapa,
	unsigned long offset, unsigned int length)
{
	struct device *dev = &cyapa->client->dev;
	unsigned int max_offset;

	if (cyapa == NULL)
		return -ENODEV;

	/*
	 * application may read/write 0 length byte
	 * to reset read/write pointer to offset.
	 */
	max_offset = (length == 0) ? offset : (length - 1 + offset);

	/* max registers contained in one register map in bytes is 256. */
	if (cyapa_pos_validate(offset) && cyapa_pos_validate(max_offset))
		return 0;

	dev_warn(dev, "invalid parameters, length=%d, offset=0x%x\n", length,
		 (unsigned int)offset);

	return -EINVAL;
}

static ssize_t cyapa_misc_read(struct file *file, char __user *usr_buf,
		size_t count, loff_t *offset)
{
	int ret;
	int reg_len = (int)count;
	unsigned long reg_offset = *offset;
	u8 reg_buf[CYAPA_REG_MAP_SIZE];
	struct cyapa *cyapa = (struct cyapa *)file->private_data;
	struct device *dev = &cyapa->client->dev;

	ret = cyapa_miscdev_rw_params_check(cyapa, reg_offset, count);
	if (ret < 0)
		return ret;

	ret = cyapa_i2c_reg_read_block(cyapa, (u16)reg_offset, reg_len,
				       reg_buf);
	if (ret < 0) {
		dev_err(dev, "I2C read FAILED.\n");
		return ret;
	}

	if (ret < reg_len)
		dev_warn(dev, "Expected %d bytes, read %d bytes.\n",
			reg_len, ret);
	reg_len = ret;

	if (copy_to_user(usr_buf, reg_buf, reg_len)) {
		ret = -EFAULT;
	} else {
		*offset += reg_len;
		ret = reg_len;
	}

	return ret;
}

static ssize_t cyapa_misc_write(struct file *file, const char __user *usr_buf,
		size_t count, loff_t *offset)
{
	int ret;
	unsigned long reg_offset = *offset;
	u8 reg_buf[CYAPA_REG_MAP_SIZE];
	struct cyapa *cyapa = (struct cyapa *)file->private_data;

	ret = cyapa_miscdev_rw_params_check(cyapa, reg_offset, count);
	if (ret < 0)
		return ret;

	if (copy_from_user(reg_buf, usr_buf, (int)count))
		return -EINVAL;

	ret = cyapa_i2c_reg_write_block(cyapa,
					(u16)reg_offset,
					(int)count,
					reg_buf);

	if (!ret)
		*offset += count;

	return ret ? ret : count;
}

static int cyapa_send_bl_cmd(struct cyapa *cyapa, enum cyapa_bl_cmd cmd)
{
	struct device *dev = &cyapa->client->dev;
	int ret;

	switch (cmd) {
	case CYAPA_CMD_APP_TO_IDLE:
		ret = cyapa_bl_enter(cyapa);
		if (ret < 0)
			dev_err(dev, "enter bootloader failed, %d\n", ret);
		break;

	case CYAPA_CMD_IDLE_TO_ACTIVE:
		ret = cyapa_bl_activate(cyapa);
		if (ret)
			dev_err(dev, "activate bootloader failed, %d\n", ret);
		break;

	case CYAPA_CMD_ACTIVE_TO_IDLE:
		ret = cyapa_bl_deactivate(cyapa);
		if (ret)
			dev_err(dev, "deactivate bootloader failed, %d\n", ret);
		break;

	case CYAPA_CMD_IDLE_TO_APP:
		cyapa_detect(cyapa);
		break;

	default:
		/* unknown command. */
		ret = -EINVAL;
	}
	return ret;
}

static long cyapa_misc_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	int ret;
	int ioctl_len;
	struct cyapa *cyapa = (struct cyapa *)file->private_data;
	struct device *dev = &cyapa->client->dev;
	struct cyapa_misc_ioctl_data ioctl_data;
	struct cyapa_trackpad_run_mode run_mode;
	u8 buf[8];

	if (cyapa == NULL) {
		dev_err(dev, "device does not exist.\n");
		return -ENODEV;
	}

	/* copy to kernel space. */
	ioctl_len = sizeof(struct cyapa_misc_ioctl_data);
	if (copy_from_user(&ioctl_data, (u8 *)arg, ioctl_len))
		return -EINVAL;

	switch (cmd) {
	case CYAPA_GET_PRODUCT_ID:
		if (!ioctl_data.buf || ioctl_data.len < 16)
			return -EINVAL;

		ioctl_data.len = 16;
		if (copy_to_user(ioctl_data.buf, cyapa->product_id, 16))
				return -EIO;
		if (copy_to_user((void *)arg, &ioctl_data, ioctl_len))
			return -EIO;
		return ioctl_data.len;

	case CYAPA_GET_FIRMWARE_VER:
		if (!ioctl_data.buf || ioctl_data.len < 2)
			return -EINVAL;

		ioctl_data.len = 2;
		memset(buf, 0, sizeof(buf));
		buf[0] = cyapa->fw_maj_ver;
		buf[1] = cyapa->fw_min_ver;
		if (copy_to_user(ioctl_data.buf, buf, ioctl_data.len))
			return -EIO;
		if (copy_to_user((void *)arg, &ioctl_data, ioctl_len))
			return -EIO;
		return ioctl_data.len;

	case CYAPA_GET_HARDWARE_VER:
		if (!ioctl_data.buf || ioctl_data.len < 2)
			return -EINVAL;

		ioctl_data.len = 2;
		memset(buf, 0, sizeof(buf));
		buf[0] = cyapa->hw_maj_ver;
		buf[1] = cyapa->hw_min_ver;
		if (copy_to_user(ioctl_data.buf, buf, ioctl_data.len))
			return -EIO;
		if (copy_to_user((void *)arg, &ioctl_data, ioctl_len))
			return -EIO;
		return ioctl_data.len;

	case CYAPA_GET_PROTOCOL_VER:
		if (!ioctl_data.buf || ioctl_data.len < 1)
			return -EINVAL;

		ioctl_data.len = 1;
		memset(buf, 0, sizeof(buf));
		buf[0] = cyapa->gen;
		if (copy_to_user(ioctl_data.buf, buf, ioctl_data.len))
			return -EIO;
		if (copy_to_user((void *)arg, &ioctl_data, ioctl_len))
			return -EIO;
		return ioctl_data.len;

	case CYAPA_GET_TRACKPAD_RUN_MODE:
		if (!ioctl_data.buf || ioctl_data.len < 2)
			return -EINVAL;

		/* Convert cyapa->state to IOCTL format */
		switch (cyapa->state) {
		case CYAPA_STATE_OP:
			buf[0] = CYAPA_OPERATIONAL_MODE;
			buf[1] = CYAPA_BOOTLOADER_INVALID_STATE;
			break;
		case CYAPA_STATE_BL_IDLE:
			buf[0] = CYAPA_BOOTLOADER_MODE;
			buf[1] = CYAPA_BOOTLOADER_IDLE_STATE;
			break;
		case CYAPA_STATE_BL_ACTIVE:
			buf[0] = CYAPA_BOOTLOADER_MODE;
			buf[1] = CYAPA_BOOTLOADER_ACTIVE_STATE;
			break;
		case CYAPA_STATE_BL_BUSY:
			buf[0] = CYAPA_BOOTLOADER_MODE;
			buf[1] = CYAPA_BOOTLOADER_INVALID_STATE;
			break;
		default:
			buf[0] = CYAPA_BOOTLOADER_INVALID_STATE;
			buf[1] = CYAPA_BOOTLOADER_INVALID_STATE;
			break;
		}

		ioctl_data.len = 2;
		if (copy_to_user(ioctl_data.buf, buf, ioctl_data.len))
			return -EIO;

		if (copy_to_user((void *)arg, &ioctl_data, ioctl_len))
			return -EIO;

		return ioctl_data.len;

	case CYAYA_SEND_MODE_SWITCH_CMD:
		if (!ioctl_data.buf || ioctl_data.len < 3)
			return -EINVAL;

		ret = copy_from_user(&run_mode, (u8 *)ioctl_data.buf,
			sizeof(struct cyapa_trackpad_run_mode));
		if (ret)
			return -EINVAL;

		return cyapa_send_bl_cmd(cyapa, run_mode.rev_cmd);

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct file_operations cyapa_misc_fops = {
	.owner = THIS_MODULE,
	.open = cyapa_misc_open,
	.release = cyapa_misc_close,
	.unlocked_ioctl = cyapa_misc_ioctl,
	.llseek = cyapa_misc_llseek,
	.read = cyapa_misc_read,
	.write = cyapa_misc_write,
};

static struct miscdevice cyapa_misc_dev = {
	.name = CYAPA_MISC_NAME,
	.fops = &cyapa_misc_fops,
	.minor = MISC_DYNAMIC_MINOR,
};

static int __init cyapa_misc_init(void)
{
	return misc_register(&cyapa_misc_dev);
}

static void __exit cyapa_misc_exit(void)
{
	misc_deregister(&cyapa_misc_dev);
}

/*
 *******************************************************************
 * below routines export interfaces to sysfs file system.
 * so user can get firmware/driver/hardware information using cat command.
 * e.g.: use below command to get firmware version
 *      cat /sys/bus/i2c/drivers/cyapa/0-0067/firmware_version
 *******************************************************************
 */
static ssize_t cyapa_show_fm_ver(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cyapa *cyapa = dev_get_drvdata(dev);
	return sprintf(buf, "%d.%d\n", cyapa->fw_maj_ver, cyapa->fw_min_ver);
}

static ssize_t cyapa_show_hw_ver(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cyapa *cyapa = dev_get_drvdata(dev);
	return sprintf(buf, "%d.%d\n", cyapa->hw_maj_ver, cyapa->hw_min_ver);
}

static ssize_t cyapa_show_product_id(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct cyapa *cyapa = dev_get_drvdata(dev);
	return sprintf(buf, "%s\n", cyapa->product_id);
}

static ssize_t cyapa_show_protocol_version(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct cyapa *cyapa = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", cyapa->gen);
}

static DEVICE_ATTR(firmware_version, S_IRUGO, cyapa_show_fm_ver, NULL);
static DEVICE_ATTR(hardware_version, S_IRUGO, cyapa_show_hw_ver, NULL);
static DEVICE_ATTR(product_id, S_IRUGO, cyapa_show_product_id, NULL);
static DEVICE_ATTR(protocol_version, S_IRUGO, cyapa_show_protocol_version, NULL);

static struct attribute *cyapa_sysfs_entries[] = {
	&dev_attr_firmware_version.attr,
	&dev_attr_hardware_version.attr,
	&dev_attr_product_id.attr,
	&dev_attr_protocol_version.attr,
	NULL,
};

static const struct attribute_group cyapa_sysfs_group = {
	.attrs = cyapa_sysfs_entries,
};

/*
 **************************************************************
 * Cypress i2c trackpad input device driver.
 **************************************************************
*/

static irqreturn_t cyapa_irq(int irq, void *dev_id)
{
	struct cyapa *cyapa = dev_id;
	struct input_dev *input = cyapa->input;
	struct cyapa_reg_data data;
	int i;
	int ret;
	int num_fingers;
	unsigned int mask;

	/*
	 * Don't read input if input device has not been configured.
	 * This check check solves a race during probe() between irq_request()
	 * and irq_disable(), since there is no way to request an irq that is
	 * initially disabled.
	 */
	if (!input)
		return IRQ_HANDLED;

	ret = cyapa_read_block(cyapa, CYAPA_CMD_GROUP_DATA, (u8 *)&data);
	if (ret != sizeof(data))
		return IRQ_HANDLED;

	if ((data.device_status & OP_STATUS_SRC) != OP_STATUS_SRC ||
	    (data.device_status & OP_STATUS_DEV) != CYAPA_DEV_NORMAL ||
	    (data.finger_btn & OP_DATA_VALID) != OP_DATA_VALID) {
		return IRQ_HANDLED;
	}

	mask = 0;
	num_fingers = (data.finger_btn >> 4) & 0x0F;
	for (i = 0; i < num_fingers; i++) {
		const struct cyapa_touch *touch = &data.touches[i];
		/* Note: touch->id range is 1 to 15; slots are 0 to 14. */
		int slot = touch->id - 1;

		mask |= (1 << slot);
		input_mt_slot(input, slot);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		input_report_abs(input, ABS_MT_POSITION_X,
				 ((touch->xy & 0xF0) << 4) | touch->x);
		input_report_abs(input, ABS_MT_POSITION_Y,
				 ((touch->xy & 0x0F) << 8) | touch->y);
		input_report_abs(input, ABS_MT_PRESSURE, touch->pressure);
	}

	/* Invalidate all unreported slots */
	for (i = 0; i < CYAPA_MAX_MT_SLOTS; i++) {
		if (mask & (1 << i))
			continue;
		input_mt_slot(input, i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	}

	input_mt_report_pointer_emulation(input, true);
	input_report_key(input, BTN_LEFT, data.finger_btn & OP_DATA_BTN_MASK);
	input_sync(input);

	return IRQ_HANDLED;
}

static u8 cyapa_check_adapter_functionality(struct i2c_client *client)
{
	u8 ret = CYAPA_ADAPTER_FUNC_NONE;

	if (i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		ret |= CYAPA_ADAPTER_FUNC_I2C;
	if (i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_BLOCK_DATA |
				     I2C_FUNC_SMBUS_I2C_BLOCK))
		ret |= CYAPA_ADAPTER_FUNC_SMBUS;
	return ret;
}

static int cyapa_create_input_dev(struct cyapa *cyapa)
{
	struct device *dev = &cyapa->client->dev;
	int ret;
	struct input_dev *input;

	dev_info(dev,
		 "Cypress APA Trackpad Information:\n" \
		 "    Product ID:  %s\n" \
		 "    Protocol Generation:  %d\n" \
		 "    Firmware Version:  %d.%d\n" \
		 "    Hardware Version:  %d.%d\n" \
		 "    Max ABS X,Y:   %d,%d\n" \
		 "    Physical Size X,Y:   %d,%d\n",
		 cyapa->product_id,
		 cyapa->gen,
		 cyapa->fw_maj_ver, cyapa->fw_min_ver,
		 cyapa->hw_maj_ver, cyapa->hw_min_ver,
		 cyapa->max_abs_x, cyapa->max_abs_y,
		 cyapa->physical_size_x, cyapa->physical_size_y);

	input = cyapa->input = input_allocate_device();
	if (!input) {
		dev_err(dev, "allocate memory for input device failed\n");
		return -ENOMEM;
	}

	input->name = cyapa->client->name;
	input->phys = cyapa->client->adapter->name;
	input->id.bustype = BUS_I2C;
	input->id.version = 1;
	input->id.product = 0;  /* means any product in eventcomm. */
	input->dev.parent = &cyapa->client->dev;

	input_set_drvdata(input, cyapa);

	__set_bit(EV_ABS, input->evbit);

	/*
	 * set and report not-MT axes to support synaptics X Driver.
	 * When multi-fingers on trackpad, only the first finger touch
	 * will be reported as X/Y axes values.
	 */
	input_set_abs_params(input, ABS_X, 0, cyapa->max_abs_x, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, cyapa->max_abs_y, 0, 0);
	input_set_abs_params(input, ABS_PRESSURE, 0, 255, 0, 0);

	/* finger position */
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, cyapa->max_abs_x, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, cyapa->max_abs_y, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	ret = input_mt_init_slots(input, CYAPA_MAX_MT_SLOTS);
	if (ret < 0) {
		dev_err(dev, "allocate memory for MT slots failed, %d\n", ret);
		goto err_free_device;
	}

	if (cyapa->physical_size_x && cyapa->physical_size_y) {
		input_abs_set_res(input, ABS_X,
			cyapa->max_abs_x / cyapa->physical_size_x);
		input_abs_set_res(input, ABS_Y,
			cyapa->max_abs_y / cyapa->physical_size_y);
		input_abs_set_res(input, ABS_MT_POSITION_X,
			cyapa->max_abs_x / cyapa->physical_size_x);
		input_abs_set_res(input, ABS_MT_POSITION_Y,
			cyapa->max_abs_y / cyapa->physical_size_y);
	}

	__set_bit(EV_KEY, input->evbit);
	__set_bit(BTN_TOUCH, input->keybit);
	__set_bit(BTN_TOOL_FINGER, input->keybit);
	__set_bit(BTN_TOOL_DOUBLETAP, input->keybit);
	__set_bit(BTN_TOOL_TRIPLETAP, input->keybit);
	__set_bit(BTN_TOOL_QUADTAP, input->keybit);
	__set_bit(BTN_TOOL_QUINTTAP, input->keybit);

	__set_bit(BTN_LEFT, input->keybit);

	/* Register the device in input subsystem */
	ret = input_register_device(input);
	if (ret) {
		dev_err(dev, "input device register failed, %d\n", ret);
		goto err_free_device;
	}

	enable_irq(cyapa->irq);
	return 0;

err_free_device:
	input_free_device(input);
	cyapa->input = NULL;
	return ret;
}

static void cyapa_detect(struct cyapa *cyapa)
{
	struct device *dev = &cyapa->client->dev;
	int ret;

	ret = cyapa_check_is_operational(cyapa);
	if (ret == -ETIMEDOUT) {
		dev_err(dev, "no device detected, %d\n", ret);
		return;
	} else if (ret) {
		dev_err(dev, "device detected, but not operational, %d\n", ret);
		return;
	}

	if (!cyapa->input) {
		ret = cyapa_create_input_dev(cyapa);
		if (ret)
			dev_err(dev, "create input_dev instance failed, %d\n",
				ret);
	} else {
		ret = cyapa_set_power_mode(cyapa, PWR_MODE_FULL_ACTIVE);
		if (ret)
			dev_warn(dev, "resume active power failed, %d\n", ret);
	}
}

static void cyapa_detect_work(struct work_struct *work)
{
	struct cyapa *cyapa = container_of(work, struct cyapa, detect_work);
	cyapa_detect(cyapa);
}

static int __devinit cyapa_probe(struct i2c_client *client,
				 const struct i2c_device_id *dev_id)
{
	int ret;
	u8 adapter_func;
	struct cyapa *cyapa;
	struct device *dev = &client->dev;

	adapter_func = cyapa_check_adapter_functionality(client);
	if (adapter_func == CYAPA_ADAPTER_FUNC_NONE) {
		dev_err(dev, "not a supported I2C/SMBus adapter\n");
		return -EIO;
	}

	cyapa = kzalloc(sizeof(struct cyapa), GFP_KERNEL);
	if (!cyapa) {
		dev_err(dev, "allocate memory for cyapa failed\n");
		return -ENOMEM;
	}

	cyapa->gen = CYAPA_GEN3;
	cyapa->client = client;
	i2c_set_clientdata(client, cyapa);

	cyapa->adapter_func = adapter_func;
	/* i2c isn't supported, set smbus */
	if (cyapa->adapter_func == CYAPA_ADAPTER_FUNC_SMBUS)
		cyapa->smbus = true;
	cyapa->state = CYAPA_STATE_NO_DEVICE;

	global_cyapa = cyapa;
	cyapa->misc_open_count = 0;
	spin_lock_init(&cyapa->miscdev_spinlock);
	mutex_init(&cyapa->misc_mutex);

	/*
	 * Note: There is no way to request an irq that is initially disabled.
	 * Thus, there is a little race here, which is resolved in cyapa_irq()
	 * by checking that cyapa->input has been allocated, which happens
	 * in cyapa_detect(), before creating input events.
	 */
	cyapa->irq = client->irq;
	ret = request_threaded_irq(cyapa->irq,
				   NULL,
				   cyapa_irq,
				   IRQF_TRIGGER_FALLING,
				   CYAPA_I2C_NAME,
				   cyapa);
	if (ret) {
		dev_err(dev, "IRQ request failed: %d\n, ", ret);
		goto err_mem_free;
	}
	disable_irq(cyapa->irq);

	if (sysfs_create_group(&client->dev.kobj, &cyapa_sysfs_group))
		dev_warn(dev, "error creating sysfs entries.\n");

	/*
	 * At boot it can take up to 2 seconds for firmware to complete sensor
	 * calibration. Probe in a workqueue so as not to block system boot.
	 */
	cyapa->detect_wq = create_singlethread_workqueue("cyapa_detect_wq");
	if (!cyapa->detect_wq) {
		ret = -ENOMEM;
		dev_err(dev, "create detect workqueue failed\n");
		goto err_irq_free;
	}

	INIT_WORK(&cyapa->detect_work, cyapa_detect_work);
	ret = queue_work(cyapa->detect_wq, &cyapa->detect_work);
	if (ret < 0) {
		dev_err(dev, "device detect failed, %d\n", ret);
		goto err_wq_free;
	}

	return 0;

err_wq_free:
	destroy_workqueue(cyapa->detect_wq);
err_irq_free:
	free_irq(cyapa->irq, cyapa);
err_mem_free:
	kfree(cyapa);
	global_cyapa = NULL;

	return ret;
}

static int __devexit cyapa_remove(struct i2c_client *client)
{
	struct cyapa *cyapa = i2c_get_clientdata(client);

	sysfs_remove_group(&client->dev.kobj, &cyapa_sysfs_group);

	free_irq(cyapa->irq, cyapa);

	if (cyapa->input)
		input_unregister_device(cyapa->input);

	if (cyapa->detect_wq)
		destroy_workqueue(cyapa->detect_wq);
	kfree(cyapa);
	global_cyapa = NULL;

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int cyapa_suspend(struct device *dev)
{
	int ret;
	struct cyapa *cyapa = dev_get_drvdata(dev);

	/* Wait for detection to complete before allowing suspend. */
	flush_workqueue(cyapa->detect_wq);

	/* set trackpad device to light sleep mode. Just ignore any errors */
	ret = cyapa_set_power_mode(cyapa, PWR_MODE_LIGHT_SLEEP);
	if (ret < 0)
		dev_err(dev, "set power mode failed, %d\n", ret);

	if (device_may_wakeup(dev))
		enable_irq_wake(cyapa->irq);

	return 0;
}

static int cyapa_resume(struct device *dev)
{
	int ret;
	struct cyapa *cyapa = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		disable_irq_wake(cyapa->irq);

	PREPARE_WORK(&cyapa->detect_work, cyapa_detect_work);
	ret = queue_work(cyapa->detect_wq, &cyapa->detect_work);
	if (ret < 0) {
		dev_err(dev, "queue detect work failed, %d\n", ret);
		return ret;
	}

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(cyapa_pm_ops, cyapa_suspend, cyapa_resume);

static const struct i2c_device_id cyapa_id_table[] = {
	{ CYAPA_I2C_NAME, 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, cyapa_id_table);

static struct i2c_driver cyapa_driver = {
	.driver = {
		.name = CYAPA_I2C_NAME,
		.owner = THIS_MODULE,
		.pm = &cyapa_pm_ops,
	},

	.probe = cyapa_probe,
	.remove = __devexit_p(cyapa_remove),
	.id_table = cyapa_id_table,
};

static int __init cyapa_init(void)
{
	int ret;

	ret = i2c_add_driver(&cyapa_driver);
	if (ret) {
		pr_err("cyapa driver register FAILED.\n");
		return ret;
	}

	/*
	 * though misc cyapa interface device initialization may failed,
	 * but it won't affect the function of trackpad device when
	 * cypress_i2c_driver initialized successfully.
	 * misc init failure will only affect firmware upload function,
	 * so do not check cyapa_misc_init return value here.
	 */
	cyapa_misc_init();

	return ret;
}

static void __exit cyapa_exit(void)
{
	cyapa_misc_exit();

	i2c_del_driver(&cyapa_driver);
}

module_init(cyapa_init);
module_exit(cyapa_exit);

MODULE_DESCRIPTION("Cypress APA I2C Trackpad Driver");
MODULE_AUTHOR("Dudley Du <dudl@cypress.com>");
MODULE_LICENSE("GPL");
