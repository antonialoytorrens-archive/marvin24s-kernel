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

#ifdef CONFIG_PWM

#include <linux/pwm.h>

#ifdef CONFIG_OF
static int power_seq_of_parse_pwm(struct device_node *node,
				  struct power_seq *seq,
				  unsigned int step_nbr,
				  struct power_seq_resource *res)
{
	struct power_seq_step *step = &seq->steps[step_nbr];
	int err;

	err = of_property_read_string(node, "id", &res->pwm.id);
	if (err) {
		power_seq_err(seq, step_nbr, "error reading id property\n");
		return err;
	}

	err = of_power_seq_parse_enable_properties(node, seq, step_nbr,
						   &step->pwm.enable);
	return err;
}
#else
#define of_power_seq_parse_pwm NULL
#endif

static bool power_seq_res_compare_pwm(struct power_seq_resource *res,
				      struct power_seq_resource *res2)
{
	return !strcmp(res->pwm.id, res2->pwm.id);
}

static int power_seq_res_alloc_pwm(struct device *dev,
				   struct power_seq_resource *res)
{
	res->pwm.pwm = devm_pwm_get(dev, res->pwm.id);
	if (IS_ERR(res->pwm.pwm)) {
		dev_err(dev, "cannot get pwm \"%s\"\n", res->pwm.id);
		return PTR_ERR(res->pwm.pwm);
	}

	return 0;
}

static int power_seq_step_run_pwm(struct power_seq_step *step)
{
	if (step->pwm.enable) {
		return pwm_enable(step->resource->pwm.pwm);
	} else {
		pwm_disable(step->resource->pwm.pwm);
		return 0;
	}
}

#define POWER_SEQ_PWM_TYPE {				\
	.name = "pwm",					\
	.of_parse = power_seq_of_parse_pwm,		\
	.step_run = power_seq_step_run_pwm,		\
	.res_compare = power_seq_res_compare_pwm,	\
	.res_alloc = power_seq_res_alloc_pwm,		\
}

#else

#define POWER_SEQ_PWM_TYPE {}

#endif
