/*
 * Copyright (c) 2012 NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/gpio.h>
#include <linux/of_gpio.h>

#ifdef CONFIG_OF
static int power_seq_of_parse_gpio(struct device_node *node,
				   struct power_seq *seq,
				   unsigned int step_nbr,
				   struct power_seq_resource *res)
{
	struct power_seq_step *step = &seq->steps[step_nbr];
	int gpio;
	int err;

	gpio = of_get_named_gpio(node, "gpio", 0);
	if (gpio < 0) {
		power_seq_err(seq, step_nbr, "error reading gpio property\n");
		return gpio;
	}
	res->gpio.gpio = gpio;

	err = of_property_read_u32(node, "value", &step->gpio.value);
	if (err < 0) {
		power_seq_err(seq, step_nbr, "error reading value property\n");
	} else if (step->gpio.value < 0 || step->gpio.value > 1) {
		power_seq_err(seq, step_nbr,
			      "value out of range (must be 0 or 1)\n");
		err = -EINVAL;
	}

	return err;
}
#else
#define of_power_seq_parse_gpio NULL
#endif

static bool power_seq_res_compare_gpio(struct power_seq_resource *res,
				       struct power_seq_resource *res2)
{
	return res->gpio.gpio == res2->gpio.gpio;
}

static int power_seq_res_alloc_gpio(struct device *dev,
				    struct power_seq_resource *res)
{
	int err;

	err = devm_gpio_request(dev, res->gpio.gpio, dev_name(dev));
	if (err) {
		dev_err(dev, "cannot get gpio %d\n", res->gpio.gpio);
		return err;
	}

	return 0;
}

static int power_seq_step_run_gpio(struct power_seq_step *step)
{
	struct power_seq_resource *res = step->resource;

	/* set the GPIO direction at first use */
	if (!res->gpio.is_set) {
		int err = gpio_direction_output(res->gpio.gpio,
						step->gpio.value);
		if (err)
			return err;
		res->gpio.is_set = true;
	} else {
		gpio_set_value_cansleep(res->gpio.gpio, step->gpio.value);
	}

	return 0;
}

#define POWER_SEQ_GPIO_TYPE {					\
	.name = "gpio",					\
	.of_parse = power_seq_of_parse_gpio,		\
	.step_run = power_seq_step_run_gpio,		\
	.res_compare = power_seq_res_compare_gpio,	\
	.res_alloc = power_seq_res_alloc_gpio,		\
}
