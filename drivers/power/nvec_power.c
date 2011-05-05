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
	int cap;
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
		case 0:
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
			power->cap = msg[5];
			return NOTIFY_STOP;
	}

	return NOTIFY_OK;
}

static int nvec_power_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{

	struct nvec_power *power = dev_get_drvdata(psy->dev->parent);
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval  = power->on;
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
			val->intval = power->cap;
			break;
		case POWER_SUPPLY_PROP_PRESENT:
			val->intval = power->bat_present;
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
