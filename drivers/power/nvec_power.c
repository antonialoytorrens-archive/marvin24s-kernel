#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/power_supply.h>
#include <linux/mfd/nvec.h>
#include <linux/slab.h>

struct nvec_power {
	struct notifier_block notifier;
	int on;
};

static int nvec_power_notifier(struct notifier_block *nb,
				 unsigned long event_type, 
		unsigned char *data)
{
	int i;
	struct nvec_power *power = container_of(nb, struct nvec_power, notifier);

	if(event_type == NVEC_SYS) {
		printk("nvec got system event: ");

		for(i=0;i<=(data[1]+1);i++)
			printk("%02x ", data[i]);
		printk(".\n");
	}

	return 0;
}

static int nvec_power_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		nvec_write_async("\x01\x00", 2);
		val->intval = 1; // get from somewhere
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static enum power_supply_property nvec_power_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static char *nvec_power_supplied_to[] = {
	"main-battery",
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


static int __devinit nvec_power_probe(struct platform_device *pdev)
{
	struct nvec_power *power = kzalloc(sizeof(struct nvec_power), GFP_NOWAIT);

	power->notifier.notifier_call = nvec_power_notifier;
	nvec_register_notifier(NULL, &power->notifier, NVEC_SYS);

	dev_set_drvdata(&pdev->dev, power);

	nvec_write_async("\x01\x00", 2);
	return power_supply_register(&pdev->dev, &nvec_psy);
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
