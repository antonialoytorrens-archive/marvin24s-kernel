#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/i2c.h>

#include <linux/fs.h>
#include <linux/errno.h>

#include <linux/mfd/nvtegra-ec.h>

struct ec_dev {
	struct i2c_client *client;
	struct device *dev;
	int req_gpio;
	struct workqueue_struct *work_queue;
	struct mutex lock;
	unsigned long id;

	struct platform_device debug_dev;
} *ec;

static int __nvtegra_ec_read(struct ec_dev *ec,
				int reg, uint8_t *val)
{
	int ret;

	ret = i2c_smbus_read_byte_data(ec->client, reg);
	if (ret < 0) {
		dev_err(&ec->client->dev, "failed reading at 0x%02x\n", reg);
		return ret;
	}

	*val = (uint8_t)ret;
	return 0;
}

static int __nvtegra_ec_write(struct ec_dev *ec,
				 int reg, uint8_t val)
{
	int ret;

	ret = i2c_smbus_write_byte_data(ec->client, reg, val);
	if (ret < 0) {
		dev_err(&ec->client->dev, "failed writing 0x%02x to 0x%02x\n",
				val, reg);
		return ret;
	}
	return 0;
}

int nvtegra_ec_write(struct ec_dev *ec, int reg, uint8_t val)
{
	int err;

	mutex_lock(&ec->lock);
	err = __nvtegra_ec_write(ec, reg, val);
	mutex_unlock(&ec->lock);
	
	return err;
}
EXPORT_SYMBOL_GPL(nvtegra_ec_write);

int nvtegra_ec_read(struct ec_dev *ec, int reg, uint8_t *val)
{
	int err;

	mutex_lock(&ec->lock);
	err = __nvtegra_ec_read(ec, reg, val);
	mutex_unlock(&ec->lock);

	return err;
}
EXPORT_SYMBOL_GPL(nvtegra_ec_read);

static int __devinit nvtegra_ec_probe(struct i2c_client *client,
					const struct i2c_device_id *id)
{
	struct nvec_platform_data *pdata = client->dev.platform_data;

	if (pdata == NULL) {
		dev_err(&client->dev, "missing platform data\n");
		return -ENODEV;
	}

	ec = kzalloc(sizeof(struct ec_dev), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;

	i2c_set_clientdata(client, ec);

	ec->client = client;
	ec->dev = &client->dev;
	ec->id = id->driver_data;
	ec->req_gpio = pdata->req_gpio;

	mutex_init(&ec->lock);

	printk(KERN_ALERT "nvec: init success (ReqGPIO@0x%02X)\n", ec->req_gpio);

	return 0;
}

static int __devexit nvtegra_ec_remove(struct i2c_client *client)
{
	return 0;
}

static const struct i2c_device_id nvtegra_ec_id[] = {
	{ "nvec", 0 },
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

MODULE_AUTHOR("Marc Dietrich");
MODULE_DESCRIPTION("NVIDIA compliant EC-SMBus interface");
MODULE_LICENSE("GPL");
