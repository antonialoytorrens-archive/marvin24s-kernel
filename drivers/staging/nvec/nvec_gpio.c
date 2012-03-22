#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/mfd/core.h>
#include <linux/platform_device.h>

#include "nvec.h"

struct nvec_gpio_data {
	struct nvec_chip *nvec;
	struct gpio_chip gpio_func;
	struct nvec_gpio *gpios;
};

static void nvec_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct nvec_gpio_data *nvec_gpio = container_of(chip, struct nvec_gpio_data, gpio_func);
	struct nvec_chip *nvec = nvec_gpio->nvec;

	if (nvec_gpio->gpios[offset].high != NULL) {
		dev_info(nvec->dev, "gpio %d set to value %d\n", offset, value);
		if (value)
			nvec_write_async(nvec, nvec_gpio->gpios[offset].high, sizeof(nvec_gpio->gpios[offset].high));
		else
			nvec_write_async(nvec, nvec_gpio->gpios[offset].low, sizeof(nvec_gpio->gpios[offset].low));
	} else {
		dev_err(nvec->dev, "standard gpios are not supported yet\n");
	}
}

static struct gpio_chip template_chip = {
	.label		= "nvec",
	.owner		= THIS_MODULE,
	.set		= nvec_gpio_set,
	.can_sleep	= 1,
};

static int __devinit nvec_gpio_probe(struct platform_device *pdev)
{
	struct nvec_chip *nvec = dev_get_drvdata(pdev->dev.parent);
	struct nvec_gpio_platform_data *pdata = pdev->mfd_cell->platform_data;
	struct nvec_gpio_data *nvec_gpio;
	int ret;

	nvec_gpio = devm_kzalloc(&pdev->dev, sizeof(*nvec_gpio), GFP_KERNEL);
	if (nvec_gpio == NULL)
		return -ENOMEM;

	nvec_gpio->nvec = nvec;
	nvec_gpio->gpio_func = template_chip;
	nvec_gpio->gpio_func.ngpio = pdata->nrgpios;
	nvec_gpio->gpio_func.base = pdata->base;
	nvec_gpio->gpio_func.dev = &pdev->dev;
	nvec_gpio->gpios = pdata->gpios;

	dev_err(&pdev->dev, "base is at %d\n", pdata->base);

	ret = gpiochip_add(&nvec_gpio->gpio_func);
	if (ret < 0) {
		dev_err(&pdev->dev, "Could not register gpiochip, %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, nvec_gpio);

	return ret;
}

static int __devexit nvec_gpio_remove(struct platform_device *pdev)
{
	struct nvec_gpio_data *nvec_gpio = platform_get_drvdata(pdev);
	int ret;

	ret = gpiochip_remove(&nvec_gpio->gpio_func);
	if (ret == 0)
		kfree(nvec_gpio);

	return ret;
}

static struct platform_driver nvec_gpio_driver = {
	.driver.name	= "nvec-gpio",
	.driver.owner	= THIS_MODULE,
	.probe		= nvec_gpio_probe,
	.remove		= __devexit_p(nvec_gpio_remove),
};

module_platform_driver(nvec_gpio_driver)

MODULE_AUTHOR("Marc Dietrich <marvin24@gmx.de>");
MODULE_DESCRIPTION("GPIO interface for NVEC");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:nvec-gpio");
