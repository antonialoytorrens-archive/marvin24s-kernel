#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/i2c.h>

#include <linux/mfd/nvtegra-ec.h>

struct nvtegra_ec_chip {
	struct i2c_client *client;
	struct device *dev;
	struct mutex lock;
	int req_gpio;
	unsigned long id;
};

static int __nvtegra_ec_read(struct i2c_client *client,
				int reg, uint8_t *val)
{
	int ret;

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0) {
		dev_err(&client->dev, "failed reading at 0x%02x\n", reg);
		return ret;
	}

	*val = (uint8_t)ret;
	return 0;
}

static int __nvtegra_ec_write(struct i2c_client *client,
				 int reg, uint8_t val)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, reg, val);
	if (ret < 0) {
		dev_err(&client->dev, "failed writing 0x%02x to 0x%02x\n",
				val, reg);
		return ret;
	}
	return 0;
}

int nvtegra_ec_write(struct device *dev, int reg, uint8_t val)
{
	return __nvtegra_ec_write(to_i2c_client(dev), reg, val);
}
EXPORT_SYMBOL_GPL(nvtegra_ec_write);

int nvtegra_ec_read(struct device *dev, int reg, uint8_t *val)
{
	return __nvtegra_ec_read(to_i2c_client(dev), reg, val);
}
EXPORT_SYMBOL_GPL(nvtegra_ec_read);

static int __remove_subdev(struct device *dev, void *unused)
{
	platform_device_unregister(to_platform_device(dev));
	return 0;
}

static int nvtegra_ec_remove_subdevs(struct nvtegra_ec_chip *chip)
{
	return device_for_each_child(chip->dev, NULL, __remove_subdev);
}

static int __devinit nvtegra_ec_probe(struct i2c_client *client,
					const struct i2c_device_id *id)
{
	struct nvtegra_ec_platform_data *pdata = client->dev.platform_data;
/*	struct platform_device *pdev; */
	struct nvtegra_ec_chip *chip;

	if (!i2c_check_functionality(client->adapter,
					I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev, "SMBUS Word Data not Supported\n");
		return -EIO;
	}

	if (pdata == NULL) {
		dev_err(&client->dev, "missing platform data\n");
		return -ENODEV;
	}

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	i2c_set_clientdata(client, chip);
	chip->client = client;

	chip->dev = &client->dev;
	chip->id = id->driver_data;
	mutex_init(&chip->lock);

	return 0;
}

static int __devexit nvtegra_ec_remove(struct i2c_client *client)
{
	struct nvtegra_ec_chip *chip = dev_get_drvdata(&client->dev);

	nvtegra_ec_remove_subdevs(chip);
	kfree(chip);
	return 0;
}

static const struct i2c_device_id nvtegra_ec_id[] = {
	{ "nvec", 1 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, nvtegra_ec_id);

static struct i2c_driver nvtegra_ec_driver = {
	.driver = {
		.name	= "nvec",
		.owner	= THIS_MODULE,
	},
	.probe		= nvtegra_ec_probe,
	.remove		= __devexit_p(nvtegra_ec_remove),
	.suspend	= NULL,
	.resume		= NULL,
	.id_table 	= nvtegra_ec_id,
};

static int __init nvtegra_ec_init(void)
{
	return i2c_add_driver(&nvtegra_ec_driver);
}
module_init(nvtegra_ec_init);

static void __exit nvtegra_ec_exit(void)
{
	i2c_del_driver(&nvtegra_ec_driver);
}
module_exit(nvtegra_ec_exit);

MODULE_AUTHOR("Marc Dietrich <marvin24@gmx.de>");
MODULE_DESCRIPTION("NVidia Compliant EC-SMBus Interface");
MODULE_LICENSE("GPL");
