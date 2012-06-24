/*
 * nvec_power: power supply driver for a NVIDIA compliant embedded controller
 *
 * Copyright (C) 2011 The AC100 Kernel Team <ac100@lists.launchpad.net>
 *
 * Authors:  Ilya Petrov <ilya.muromec@gmail.com>
 *           Marc Dietrich <marvin24@gmx.de>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/delay.h>

#include "nvec.h"

#define GET_SYSTEM_STATUS 0x00

struct nvec_power {
	struct notifier_block notifier;
	struct delayed_work poller;
	int ac_present;
	int present;
	int status;
	int voltage_now;
	int current_now;
	int current_avg;
	int time_remain;
	int charge_full_design;
	int charge_last_full;
	int critical_capacity;
	int capacity_remain;
	int temperature;
	int capacity;
	int type_enum;
	char manu[30];
	char model[30];
	char type[30];
};

struct nvec_power_struct {
	struct nvec_chip *nvec;
	struct nvec_power *psy;
};

struct nvec_power_struct nvec_power;

enum {
	SLOT_STATUS,
	VOLTAGE,
	TIME_REMAINING,
	CURRENT,
	AVERAGE_CURRENT,
	AVERAGING_TIME_INTERVAL,
	CAPACITY_REMAINING,
	LAST_FULL_CHARGE_CAPACITY,
	DESIGN_CAPACITY,
	CRITICAL_CAPACITY,
	TEMPERATURE,
	MANUFACTURER,
	MODEL,
	TYPE,
};

struct nvec_power_response {
	u8 event_type;
	u8 length;
	u8 sub_type;
	u8 status;
	/* payload */
	union {
		char plc[30];
		u16 plu;
		s16 pls;
	};
};

static struct power_supply nvec_bat_psy;
static struct power_supply nvec_ac_psy;

static const int bat_init[] = {
	LAST_FULL_CHARGE_CAPACITY, DESIGN_CAPACITY, CRITICAL_CAPACITY,
	MANUFACTURER, MODEL, TYPE,
};

static void get_bat_mfg_data(void)
{
	int i;
	char buf[] = { NVEC_BAT, SLOT_STATUS };

	for (i = 0; i < ARRAY_SIZE(bat_init); i++) {
		buf[1] = bat_init[i];
		nvec_write_async(nvec_power.nvec, buf, 2);
	}
}

static int nvec_power_notifier(struct notifier_block *nb,
				   unsigned long event_type, void *data)
{
	struct nvec_power *bat = nvec_power.psy;
	struct nvec_power_response *res = (struct nvec_power_response *)data;
	int status_changed = 0;

	if ((event_type == NVEC_SYS) &&
	    (res->sub_type == 0)) {
		if (bat->ac_present != (res->plu & 1)) {
			bat->ac_present = (res->plu & 1);
			power_supply_changed(&nvec_ac_psy);
		}
		return NOTIFY_STOP;
	}

	if (event_type != NVEC_BAT)
		return NOTIFY_DONE;

	switch (res->sub_type) {
	case SLOT_STATUS:
		if (res->plc[0] & 1) {
			if (bat->present == 0) {
				status_changed = 1;
				get_bat_mfg_data();
			}
			bat->present = 1;

			switch ((res->plc[0] >> 1) & 3) {
			case 0:
				bat->status =
				    POWER_SUPPLY_STATUS_NOT_CHARGING;
				break;
			case 1:
				bat->status =
				    POWER_SUPPLY_STATUS_CHARGING;
				break;
			case 2:
				bat->status =
				    POWER_SUPPLY_STATUS_DISCHARGING;
				break;
			default:
				bat->status = POWER_SUPPLY_STATUS_UNKNOWN;
			}
		} else {
			if (bat->present == 1)
				status_changed = 1;

			bat->present = 0;
			bat->status = POWER_SUPPLY_STATUS_UNKNOWN;
		}
		bat->capacity = res->plc[1];
		if (status_changed)
			power_supply_changed(&nvec_bat_psy);
		break;
	case VOLTAGE:
		bat->voltage_now = res->plu * 1000;
		break;
	case TIME_REMAINING:
		bat->time_remain = res->plu * 3600;
		break;
	case CURRENT:
		bat->current_now = res->pls * 1000;
		break;
	case AVERAGE_CURRENT:
		bat->current_avg = res->pls * 1000;
		break;
	case CAPACITY_REMAINING:
		bat->capacity_remain = res->plu * 1000;
		break;
	case LAST_FULL_CHARGE_CAPACITY:
		bat->charge_last_full = res->plu * 1000;
		break;
	case DESIGN_CAPACITY:
		bat->charge_full_design = res->plu * 1000;
		break;
	case CRITICAL_CAPACITY:
		bat->critical_capacity = res->plu * 1000;
		break;
	case TEMPERATURE:
		bat->temperature = res->plu - 2732;
		break;
	case MANUFACTURER:
		memcpy(bat->manu, &res->plc, res->length - 2);
		bat->model[res->length - 2] = '\0';
		break;
	case MODEL:
		memcpy(bat->model, &res->plc, res->length - 2);
		bat->model[res->length - 2] = '\0';
		break;
	case TYPE:
		memcpy(bat->type, &res->plc, res->length - 2);
		bat->type[res->length - 2] = '\0';
		/* this differs a little from the spec
		   fill in more if you find some */
		if (!strncmp(bat->type, "Li", 30))
			bat->type_enum = POWER_SUPPLY_TECHNOLOGY_LION;
		else
			bat->type_enum = POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
		break;
	default:
		return NOTIFY_STOP;
	}

	return NOTIFY_STOP;
}

static int nvec_ac_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct nvec_power *ac = nvec_power.psy;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = ac->ac_present;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int nvec_battery_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct nvec_power *bat = nvec_power.psy;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = bat->status;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = bat->capacity;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = bat->present;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = bat->voltage_now;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = bat->current_now;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval = bat->current_avg;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		val->intval = bat->time_remain;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = bat->charge_full_design;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = bat->charge_last_full;
		break;
	case POWER_SUPPLY_PROP_CHARGE_EMPTY:
		val->intval = bat->critical_capacity;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = bat->capacity_remain;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = bat->temperature;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = bat->manu;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = bat->model;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = bat->type_enum;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static enum power_supply_property nvec_power_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static enum power_supply_property nvec_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
#ifdef EC_FULL_DIAG
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
#endif
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_EMPTY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_TECHNOLOGY,
};

static char *nvec_power_supplied_to[] = {
	"battery",
};

static struct power_supply nvec_bat_psy = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = nvec_battery_props,
	.num_properties = ARRAY_SIZE(nvec_battery_props),
	.get_property = nvec_battery_get_property,
};

static struct power_supply nvec_ac_psy = {
	.name = "ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.supplied_to = nvec_power_supplied_to,
	.num_supplicants = ARRAY_SIZE(nvec_power_supplied_to),
	.properties = nvec_power_props,
	.num_properties = ARRAY_SIZE(nvec_power_props),
	.get_property = nvec_ac_get_property,
};

static int counter;
static int const bat_iter[] = {
	SLOT_STATUS, VOLTAGE, CURRENT, CAPACITY_REMAINING,
#ifdef EC_FULL_DIAG
	AVERAGE_CURRENT, TEMPERATURE, TIME_REMAINING,
#endif
};

static void nvec_power_poll(struct work_struct *work)
{
	char buf[] = { NVEC_SYS, GET_SYSTEM_STATUS };

	if (counter >= ARRAY_SIZE(bat_iter))
		counter = 0;

	/* AC status via sys req */
	nvec_write_async(nvec_power.nvec, buf, sizeof(buf));
	msleep(100);

	/* select a battery request function via round robin
	   doing it all at once seems to overload the power supply */
	buf[0] = NVEC_BAT;
	buf[1] = bat_iter[counter++];
	nvec_write_async(nvec_power.nvec, buf, sizeof(buf));

	schedule_delayed_work(to_delayed_work(work), msecs_to_jiffies(5000));
};

static int nvec_power_probe(struct platform_device *pdev)
{
	struct nvec_chip *nvec = dev_get_drvdata(pdev->dev.parent);
	int ret = 0;

	nvec_power.nvec = nvec;
	dev_set_drvdata(&pdev->dev, &nvec_power);

	nvec_power.psy = devm_kzalloc(&pdev->dev, sizeof(struct nvec_power),
					GFP_NOWAIT);
	if (nvec_power.psy == NULL)
		return -ENOMEM;

	ret = power_supply_register(&pdev->dev, &nvec_ac_psy);
	if (ret < 0)
		return ret;

	ret = power_supply_register(&pdev->dev, &nvec_bat_psy);
	if (ret < 0) {
		power_supply_unregister(&nvec_ac_psy);
		return ret;
	}

	nvec_power.psy->notifier.notifier_call = nvec_power_notifier;
	nvec_register_notifier(nvec, &nvec_power.psy->notifier,
				NVEC_SYS | NVEC_BAT);

	INIT_DELAYED_WORK(&nvec_power.psy->poller, nvec_power_poll);
	schedule_delayed_work(&nvec_power.psy->poller, msecs_to_jiffies(5000));

	get_bat_mfg_data();

	return 0;
}

static int nvec_power_remove(struct platform_device *pdev)
{
	cancel_delayed_work_sync(&nvec_power.psy->poller);
	power_supply_unregister(&nvec_bat_psy);
	power_supply_unregister(&nvec_ac_psy);

	return 0;
}

static struct platform_driver nvec_power_driver = {
	.probe = nvec_power_probe,
	.remove = nvec_power_remove,
	.driver = {
		   .name = "nvec-power",
		   .owner = THIS_MODULE,
		   }
};

module_platform_driver(nvec_power_driver);

MODULE_AUTHOR("Ilya Petrov <ilya.muromec@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NVEC battery and AC driver");
MODULE_ALIAS("platform:nvec-power");
