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

#include <linux/delay.h>

#ifdef CONFIG_OF
static int of_power_seq_parse_delay(struct device_node *node,
				    struct power_seq *seq,
				    unsigned int step_nbr,
				    struct power_seq_resource *res)
{
	struct power_seq_step *step = &seq->steps[step_nbr];
	int err;

	err = of_property_read_u32(node, "delay",
				   &step->delay.delay);
	if (err < 0)
		power_seq_err(seq, step_nbr, "error reading delay property\n");

	return err;
}
#else
#define of_power_seq_parse_delay NULL
#endif

static bool power_seq_res_compare_delay(struct power_seq_resource *res,
					struct power_seq_resource *res2)
{
	/* Delay resources are just here to hold the type of steps, so they are
	 * all equivalent. */
	return true;
}

static int power_seq_res_alloc_delay(struct device *dev,
				     struct power_seq_resource *res)
{
	return 0;
}

static int power_seq_step_run_delay(struct power_seq_step *step)
{
	usleep_range(step->delay.delay,
		     step->delay.delay + 1000);

	return 0;
}

#define POWER_SEQ_DELAY_TYPE {				\
	.name = "delay",				\
	.of_parse = of_power_seq_parse_delay,		\
	.step_run = power_seq_step_run_delay,		\
	.res_compare = power_seq_res_compare_delay,	\
	.res_alloc = power_seq_res_alloc_delay,		\
}
