#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/power_supply.h>
#include <linux/mfd/nvec.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

struct nvec_power {
	struct notifier_block notifier;
	struct delayed_work poller;
	struct nvec_chip *nvec;
	int on;
	int bat_present;
	int bat_status;
	int bat_voltage_now;
	int bat_current_now;
	int bat_current_avg;
	int time_remain;
	int charge_full_design; /* in µAh, off by factor 3 */
	int charge_last_full;   /* in µAh, off by factor 3 */
	int critical_capacity;
	int bat_temperature;
	int bat_cap;
	char bat_manu[8];
	char bat_model[8];
};

static int nvec_power_notifier(struct notifier_block *nb,
				 unsigned long event_type, void *data)
{
	struct nvec_power *power = container_of(nb, struct nvec_power, notifier);
	unsigned char *msg = (unsigned char *)data;

	if (event_type != NVEC_SYS)
		return NOTIFY_DONE;

	if(msg[2] == 0)
	{
		power->on = msg[4];
		return NOTIFY_STOP;
	}

        return NOTIFY_OK;
}

static int nvec_power_bat_notifier(struct notifier_block *nb,
				 unsigned long event_type, void *data)
{
	struct nvec_power *power = container_of(nb, struct nvec_power, notifier);
	unsigned char *msg = (unsigned char *)data;

	if (event_type != NVEC_BAT)
		return NOTIFY_DONE;

	switch(msg[2])
	{
		case 0: // Slot Status
			if (msg[4] & 1) {
				power->bat_present = 1;
				switch (msg[4] >> 1)
				{
					case 0:
						power->bat_status = POWER_SUPPLY_STATUS_NOT_CHARGING;
						break;
					case 1:
						power->bat_status = POWER_SUPPLY_STATUS_CHARGING;
						break;
					case 2:
						power->bat_status = POWER_SUPPLY_STATUS_DISCHARGING;
						break;
				}
			} else {
				power->bat_present = 0;
				power->bat_status = POWER_SUPPLY_STATUS_UNKNOWN;
			}
			power->bat_cap = msg[5];
			break;
		case 1: // voltage now
			power->bat_voltage_now = 1000 * ((msg[5] << 8) + msg[4]);
			break;
		case 2: // time remaining
			power->time_remain = (msg[5] << 8) + msg[4];
			break;
		case 3: // current now
			power->bat_current_now = (short)((msg[5] << 8) + msg[4]);
			break;
		case 4: // current avg
			power->bat_current_avg = (short)((msg[5] << 8) + msg[4]);
			break;
		case 7: // last full charge
			power->charge_last_full = ((msg[5] << 8) + msg[4]) * 1000;
			break;
		case 8: // charge full design
			power->charge_full_design = ((msg[5] << 8) + msg[4]) * 1000;
			break;
		case 9: // critical capacity / charge empty
			power->critical_capacity = ((msg[5] << 8) + msg[4]) * 1000;
			break;
		case 10: // temperature
			power->bat_temperature = ((msg[5] << 8) + msg[4]) / 10;
			break;
		case 11: // manufacturer
			strncpy(power->bat_manu, &msg[4], msg[1]-2);
			power->bat_model[msg[1]-2] = '\0';
			break;
		case 12: // model
			strncpy(power->bat_model, &msg[4], msg[1]-2);
			power->bat_model[msg[1]-2] = '\0';
			break;
		default:
			return NOTIFY_STOP;
	}

	return NOTIFY_STOP;
}

static int nvec_power_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct nvec_power *power = dev_get_drvdata(psy->dev->parent);
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = power->on;
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
	struct nvec_power *power = dev_get_drvdata(psy->dev->parent);
	switch(psp)
	{
		case POWER_SUPPLY_PROP_STATUS:
			val->intval = power->bat_status;
			break;
		case POWER_SUPPLY_PROP_CAPACITY:
			val->intval = power->bat_cap;
			break;
		case POWER_SUPPLY_PROP_PRESENT:
			val->intval = power->bat_present;
			break;
		case POWER_SUPPLY_PROP_VOLTAGE_NOW:
			val->intval = power->bat_voltage_now;
			break;
		case POWER_SUPPLY_PROP_CURRENT_NOW:
			val->intval = power->bat_current_now;
			break;
		case POWER_SUPPLY_PROP_CURRENT_AVG:
			val->intval = power->bat_current_avg;
			break;
		case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
			val->intval = power->time_remain;
			break;
		case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
			val->intval = power->charge_full_design;
			break;
		case POWER_SUPPLY_PROP_CHARGE_FULL:
			val->intval = power->charge_last_full;
			break;
		case POWER_SUPPLY_PROP_CHARGE_EMPTY:
			val->intval = power->critical_capacity;
			break;
		case POWER_SUPPLY_PROP_CHARGE_NOW:
			val->intval = power->bat_cap * power->charge_last_full / 100;
			break;
		case POWER_SUPPLY_PROP_TEMP:
			val->intval = power->bat_temperature;
			break;
		case POWER_SUPPLY_PROP_MANUFACTURER:
			val->strval = power->bat_manu;
			break;
		case POWER_SUPPLY_PROP_MODEL_NAME:
			val->strval = power->bat_model;
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
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_EMPTY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static char *nvec_power_supplied_to[] = {
	"battery",
};

static struct power_supply nvec_bat_psy = {
	.name		= "battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= nvec_battery_props,
	.num_properties	= ARRAY_SIZE(nvec_battery_props),
	.get_property	= nvec_battery_get_property,
};

static struct power_supply nvec_psy = {
	.name = "ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.supplied_to = nvec_power_supplied_to,
	.num_supplicants = ARRAY_SIZE(nvec_power_supplied_to),
	.properties = nvec_power_props,
	.num_properties = ARRAY_SIZE(nvec_power_props),
	.get_property = nvec_power_get_property,
};

static void nvec_power_poll(struct work_struct *work)
{
	struct nvec_power *power = container_of(work, struct nvec_power,
		 poller.work);

/* AC status via sys req */
	nvec_write_async(power->nvec, "\x01\x00", 2);
/* battery status */
	nvec_write_async(power->nvec, "\x02\x00", 2);
/* get voltage */
	nvec_write_async(power->nvec, "\x02\x01", 2);
/* get remaining time */
	nvec_write_async(power->nvec, "\x02\x02", 2);
/* get current */
	nvec_write_async(power->nvec, "\x02\x03", 2);
/* get average current */
	nvec_write_async(power->nvec, "\x02\x04", 2);
/* avg time interval */
//	nvec_write_async(power->nvec, "\x02\x05", 2);
/* capacity remain */
//	nvec_write_async(power->nvec, "\x02\x06", 2);
/* get last full charge */
	nvec_write_async(power->nvec, "\x02\x07", 2);
/* get design charge */
	nvec_write_async(power->nvec, "\x02\x08", 2);
/* get critical capacity */
	nvec_write_async(power->nvec, "\x02\x09", 2);
/* get temperature */
	nvec_write_async(power->nvec, "\x02\x0A", 2);

	schedule_delayed_work(to_delayed_work(work), msecs_to_jiffies(5000));
};

static int __devinit nvec_power_probe(struct platform_device *pdev)
{
	struct power_supply *psy;
	struct nvec_power *power = kzalloc(sizeof(struct nvec_power), GFP_NOWAIT);
	struct nvec_chip *nvec = dev_get_drvdata(pdev->dev.parent);

	dev_set_drvdata(&pdev->dev, power);
	power->nvec = nvec;

	switch (pdev->id) {
	case 0:

		psy = &nvec_psy;

		power->notifier.notifier_call = nvec_power_notifier;

		INIT_DELAYED_WORK(&power->poller, nvec_power_poll);
		schedule_delayed_work(&power->poller, 5000);
		break;
	case 1:
		psy = &nvec_bat_psy;

                power->notifier.notifier_call = nvec_power_bat_notifier;
		break;
	default:
		return -ENODEV;
	}

	nvec_register_notifier(nvec, &power->notifier, NVEC_SYS);

/* get bat manu */
	nvec_write_async(power->nvec, "\x02\x0B", 2);
/* get bat model */
	nvec_write_async(power->nvec, "\x02\x0C", 2);

	return power_supply_register(&pdev->dev, psy);
}

static struct platform_driver nvec_power_driver = {
	.probe = nvec_power_probe,
//	.remove = __devexit_p(nvec_power_remove),
	.driver = {
		.name = "nvec-power",
		.owner = THIS_MODULE,
	}
};

static int __init nvec_power_init(void) 
{
	return platform_driver_register(&nvec_power_driver);
}

module_init(nvec_power_init);

MODULE_AUTHOR("Ilya Petrov <ilya.muromec@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NVEC battery and AC driver");
MODULE_ALIAS("platform:nvec-power");
