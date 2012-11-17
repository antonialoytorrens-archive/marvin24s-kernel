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

#ifdef CONFIG_REGULATOR

#include <linux/regulator/consumer.h>

#ifdef CONFIG_OF
static int power_seq_of_parse_regulator(struct device_node *node,
					struct power_seq *seq,
					unsigned int step_nbr,
					struct power_seq_resource *res)
{
	struct power_seq_step *step = &seq->steps[step_nbr];
	int err;

	err = of_property_read_string(node, "id",
				      &res->regulator.id);
	if (err) {
		power_seq_err(seq, step_nbr, "error reading id property\n");
		return err;
	}

	err = of_power_seq_parse_enable_properties(node, seq, step_nbr,
						   &step->regulator.enable);
	return err;
}
#else
#define of_power_seq_parse_regulator NULL
#endif

static bool
power_seq_res_compare_regulator(struct power_seq_resource *res,
				struct power_seq_resource *res2)
{
	return !strcmp(res->regulator.id, res2->regulator.id);
}

static int power_seq_res_alloc_regulator(struct device *dev,
					 struct power_seq_resource *res)
{
	res->regulator.regulator = devm_regulator_get(dev, res->regulator.id);
	if (IS_ERR(res->regulator.regulator)) {
		dev_err(dev, "cannot get regulator \"%s\"\n",
			res->regulator.id);
		return PTR_ERR(res->regulator.regulator);
	}

	return 0;
}

static int power_seq_step_run_regulator(struct power_seq_step *step)
{
	if (step->regulator.enable)
		return regulator_enable(step->resource->regulator.regulator);
	else
		return regulator_disable(step->resource->regulator.regulator);
}

#define POWER_SEQ_REGULATOR_TYPE {			\
	.name = "regulator",				\
	.of_parse = power_seq_of_parse_regulator,	\
	.step_run = power_seq_step_run_regulator,	\
	.res_compare = power_seq_res_compare_regulator,	\
	.res_alloc = power_seq_res_alloc_regulator,	\
}

#else

#define POWER_SEQ_REGULATOR_TYPE {}

#endif
