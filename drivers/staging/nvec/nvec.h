/*
 * NVEC: NVIDIA compliant embedded controller interface
 *
 * Copyright (C) 2011 The AC100 Kernel Team <ac100@lists.launchpad.net>
 *
 * Authors:  Pierre-Hugues Husson <phhusson@free.fr>
 *           Ilya Petrov <ilya.muromec@gmail.com>
 *           Marc Dietrich <marvin24@gmx.de>
 *           Julian Andres Klode <jak@jak-linux.org>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 */

#ifndef __LINUX_MFD_NVEC
#define __LINUX_MFD_NVEC

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

/* NVEC_POOL_SIZE - Size of the pool in &struct nvec_msg */
#define NVEC_POOL_SIZE	64

/*
 * NVEC_MSG_SIZE - Maximum size of the data field of &struct nvec_msg.
 *
 * A message must store up to a SMBus block operation which consists of
 * one command byte, one count byte, and up to 32 payload bytes = 34
 * byte.
 */
#define NVEC_MSG_SIZE	34

/**
 * enum nvec_event_size - The size of an event message
 * @NVEC_2BYTES: The message has one command byte and one data byte
 * @NVEC_3BYTES: The message has one command byte and two data bytes
 * @NVEC_VAR_SIZE: The message has one command byte, one count byte, and as
 *                 up to as many bytes as the number in the count byte. The
 *                 maximum is 32
 *
 * Events can be fixed or variable sized. This is useless on other message
 * types, which are always variable sized.
 */
enum nvec_event_size {
	NVEC_2BYTES,
	NVEC_3BYTES,
	NVEC_VAR_SIZE,
};

/**
 * enum nvec_msg_type - The type of a message
 * @NVEC_SYS: A system request/response
 * @NVEC_BAT: A battery request/response
 * @NVEC_GPIO: A gpio request/response
 * @NVEC_SLEEP: A sleep request/response
 * @NVEC_KBD: A keyboard request/response
 * @NVEC_PS2: A mouse request/response
 * @NVEC_CNTL: A EC control request/response
 * @NVEC_KB_EVT: An event from the keyboard
 * @NVEC_PS2_EVT: An event from the mouse
 *
 * Events can be fixed or variable sized. This is useless on other message
 * types, which are always variable sized.
 */
enum nvec_msg_type {
	NVEC_SYS = 1,
	NVEC_BAT,
	NVEC_GPIO,
	NVEC_SLEEP,
	NVEC_KBD,
	NVEC_PS2,
	NVEC_CNTL,
	NVEC_OEM0 = 0x0d,
	NVEC_KB_EVT = 0x80,
	NVEC_PS2_EVT,
};

enum nvec_bool {
	NVEC_DISABLE,
	NVEC_ENABLE,
};

enum nvec_sys_subcmd {
	NVEC_SYS_CNF_EVENT_REPORTING = 1,
};

enum nvec_sleep_subcmd {
	NVEC_SLEEP_GLOBAL_EVENTS,
	NVEC_SLEEP_AP_PWR_DOWN,
	NVEC_SLEEP_AP_SUSPEND,
};

enum nvec_cntl_subcmd {
	NVEC_CNTL_READ_FW_VER = 0x15,
};

enum nvec_kbd_subcmd {
	NVEC_KBD_CNF_WAKE = 3,
	NVEC_KBD_CNF_WAKE_KEY_REPORTING,
	NVEC_KBD_SET_LEDS = 0xed,
	NVEC_KBD_KBD_ENABLE = 0xf4,
};

enum nvec_ps2_subcmd {
	NVEC_PS2_SEND_CMD = 1,
	NVEC_PS2_RECEIVE,
	NVEC_PS2_AUTO_RECEIVE,
	NVEC_PS2_CANCEL_AUTO_RECEIVE = 4,
	NVEC_PS2_PS2_ENABLE = 0xf4,
	NVEC_PS2_PS2_DISABLE,
};

/* construct a nvec command string */
#define NVEC_CMD_STR(type, subtype, payload...)		\
    { NVEC_##type, NVEC_##type##_##subtype, payload }

/*
 * NVEC_CALL: submit an async nvec command string
 * @nvec: holds the pointer to the nvec handle
 * @type: contains the message type (see nvec_msg_type)
 * @subtype: contains the message sub-type
 * @payload: contains pointer to an array of char for the payload
 * returns -ENOMEM if the send buffer is full or 0.
 */
#ifdef DEBUG
#define NVEC_CALL(nvec, type, subtype, payload...) (		\
    {								\
	char buf[] = NVEC_CMD_STR(type, subtype, payload);	\
	print_hex_dump(KERN_WARNING, "payload: ", DUMP_PREFIX_NONE, 16, 1, \
		buf, sizeof(buf), false);			\
	nvec_write_async((nvec), buf, sizeof(buf));		\
    })
#else
#define NVEC_CALL(nvec, type, subtype, payload...) (		\
    {								\
	char buf[] = NVEC_CMD_STR(type, subtype, payload);	\
	nvec_write_async((nvec), buf, sizeof(buf));		\
    })
#endif

/**
  * NVEC_SYNC_CALL: submit a nvec command string synchronously
  * same paramters as for async call, but return the received message
  */
#define NVEC_SYNC_CALL(nvec, type, subtype, payload...) (	\
    {								\
	char buf[] = NVEC_CMD_STR(type, subtype, payload);	\
	nvec_write_sync((nvec), buf, sizeof(buf));		\
    })

/**
 * struct nvec_msg - A buffer for a single message
 * @node: Messages are part of various lists in a &struct nvec_chip
 * @data: The data of the message
 * @size: For TX messages, the number of bytes used in @data
 * @pos:  For RX messages, the current position to write to. For TX messages,
 *        the position to read from.
 * @used: Used for the message pool to mark a message as free/allocated.
 *
 * This structure is used to hold outgoing and incoming messages. Outgoing
 * messages have a different format than incoming messages, and that is not
 * documented yet.
 */
struct nvec_msg {
	struct list_head node;
	unsigned char data[NVEC_MSG_SIZE];
	unsigned short size;
	unsigned short pos;
	atomic_t used;
};

/**
 * struct nvec_subdev - A subdevice of nvec, such as nvec_kbd
 * @name: The name of the sub device
 * @platform_data: Platform data
 * @id: Identifier of the sub device
 */
struct nvec_subdev {
	const char *name;
	void *platform_data;
	int id;
};

/**
 * struct nvec_platform_data - Platform data for NVIDIA Embedded Contoller
 * @gpio: GPIO number of the EC request pin
 * @adapter: number of the slave controller
 * @custom_drivers: board specific drivers like special events or device
 *                  initializations
 * @nr_custom_devs: number of entries in the custom_devices array
 *
 * Platform data, to be used in board definitions. For an example, take a
 * look at the paz00 board in arch/arm/mach-tegra/board-paz00.c
 */
struct nvec_platform_data {
	int gpio;
	int adapter;
	struct mfd_cell *nvec_devices;
	int nr_nvec_devs;
	bool has_poweroff;
};

struct nvec_gpio {
	const char *name;
	const char *high;
	const char *low;
};

struct nvec_gpio_platform_data {
	int base;
	struct nvec_gpio *gpios;
	int nrgpios;
};

 /**
 * struct nvec_event - defines an event used for the platform data below
 * @name: unique name of the event
 * @type: event type as defined in input.h, e.g. "EV_KEY, EV_SW"
 * @key: KEY which gets reported on event receive
 * @mask: bit inside the nvec event bitmask
 * @enabled: if the event must be enabled during boot
 */
struct nvec_event {
	const char *name;
	const int type, key;
	const long mask;
	const bool enabled;
};

/**
 * struct nvec_event_platform_data - platform data for the NVEC event driver
 * @event: points to an arry of nvec_event containing all events handled by
 *	this driver
 * @ nvevents: number of events defined
 */
struct nvec_event_platform_data {
	struct nvec_event *event;
	int nrevents;
};

/**
 * struct nvec_chip - A single connection to an NVIDIA Embedded controller
 * @dev: The device
 * @gpio: GPIO number of the EC request pin
 * @irq: The IRQ of the I2C device
 * @custom_devices: board specific drivers like special events or device
 *                  initializations
 * @nr_custom_devs: number of entries in the custom_devices array
 * @i2c_addr: The address of the I2C slave
 * @base: The base of the memory mapped region of the I2C device
 * @clk: The clock of the I2C device
 * @notifier_list: Notifiers to be called on received messages, see
 *                 nvec_register_notifier()
 * @rx_data: Received messages that have to be processed
 * @tx_data: Messages waiting to be sent to the controller
 * @nvec_status_notifier: Internal notifier (see nvec_status_notifier())
 * @rx_work: A work structure for the RX worker nvec_dispatch()
 * @tx_work: A work structure for the TX worker nvec_request_master()
 * @wq: The work queue in which @rx_work and @tx_work are executed
 * @msg_pool: A pool of messages for allocation
 * @rx: The message currently being retrieved or %NULL
 * @tx: The message currently being transferred
 * @tx_scratch: Used for building pseudo messages
 * @ec_transfer: A completion that will be completed once a message has been
 *               received (see nvec_rx_completed())
 * @tx_lock: Spinlock for modifications on @tx_data
 * @rx_lock: Spinlock for modifications on @rx_data
 * @sync_write_mutex: A mutex for nvec_write_sync()
 * @sync_write: A completion to signal that a synchronous message is complete
 * @sync_write_pending: The first two bytes of the request (type and subtype)
 * @last_sync_msg: The last synchronous message.
 * @state: State of our finite state machine used in nvec_interrupt()
 */
struct nvec_chip {
	struct device *dev;
	int gpio;
	int irq;
	struct mfd_cell *nvec_devices;
	int nr_nvec_devs;
	int i2c_addr;
	void __iomem *base;
	struct clk *clk;

	struct atomic_notifier_head notifier_list;
	struct list_head rx_data, tx_data;
	struct notifier_block nvec_status_notifier;
	struct work_struct rx_work, tx_work;
	struct workqueue_struct *wq;
	struct nvec_msg msg_pool[NVEC_POOL_SIZE];
	struct nvec_msg *rx;

	struct nvec_msg *tx;
	struct nvec_msg tx_scratch;
	struct completion ec_transfer;

	spinlock_t tx_lock, rx_lock;

	/* sync write stuff */
	struct mutex sync_write_mutex;
	struct completion sync_write;
	u16 sync_write_pending;
	struct nvec_msg *last_sync_msg;

	int state;
};

extern int nvec_write_async(struct nvec_chip *nvec, const unsigned char *data,
			     short size);

extern struct nvec_msg *nvec_write_sync(struct nvec_chip *nvec,
					const unsigned char *data, short size);

extern int nvec_register_notifier(struct nvec_chip *nvec,
				  struct notifier_block *nb,
				  unsigned int events);

extern int nvec_unregister_notifier(struct device *dev,
				    struct notifier_block *nb,
				    unsigned int events);

extern void nvec_msg_free(struct nvec_chip *nvec, struct nvec_msg *msg);

#endif
