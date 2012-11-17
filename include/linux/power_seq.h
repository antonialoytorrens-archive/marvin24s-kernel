/*
 * power_seq.h
 *
 * Simple interpreter for power sequences defined as platform data or device
 * tree properties.
 *
 * Power sequences are designed to replace the callbacks typically used in
 * board-specific files that implement board- or device- specific power
 * sequences (such as those of backlights). A power sequence is an array of
 * steps referencing resources (regulators, GPIOs, PWMs, ...) with an action to
 * perform on them. By having the power sequences interpreted, it becomes
 * possible to describe them in the device tree and thus to remove
 * board-specific files from the kernel.
 *
 * See Documentation/power/power_seqs.txt for detailed information.
 *
 * Author: Alexandre Courbot <acourbot@nvidia.com>
 *
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

#ifndef __LINUX_POWER_SEQ_H
#define __LINUX_POWER_SEQ_H

#include <linux/types.h>
#include <linux/list.h>

struct device;
struct regulator;
struct pwm_device;

/**
 * The different kinds of resources that can be controlled by the sequences
 */
enum power_seq_res_type {
	POWER_SEQ_DELAY,
	POWER_SEQ_REGULATOR,
	POWER_SEQ_PWM,
	POWER_SEQ_GPIO,
	POWER_SEQ_NUM_TYPES,
};

/**
 * struct power_seq_regulator_resource
 * @id:		name of the regulator
 * @regulator:	resolved regulator. Written during resource resolution.
 */
struct power_seq_regulator_resource {
	const char *id;
	struct regulator *regulator;
};

/**
 * struct power_seq_pwm_resource
 * @id:		name of the PWM
 * @regulator:	resolved PWM. Written during resource resolution.
 */
struct power_seq_pwm_resource {
	const char *id;
	struct pwm_device *pwm;
};

/**
 * struct power_seq_gpio_resource
 * @gpio:	number of the GPIO
 * @is_set:	track GPIO state to set its direction at first use
 */
struct power_seq_gpio_resource {
	int gpio;
	bool is_set;
};

/**
 * struct power_seq_resource - resource used by power sequences
 * @type:	type of the resource. This decides which member of the union is
 *		used for this resource
 * @list:	link resources together in power_seq_set
 * @regulator:	used if @type == POWER_SEQ_REGULATOR
 * @pwm:	used if @type == POWER_SEQ_PWM
 * @gpio:	used if @type == POWER_SEQ_GPIO
 */
struct power_seq_resource {
	enum power_seq_res_type type;
	struct list_head list;
	union {
		struct power_seq_regulator_resource regulator;
		struct power_seq_pwm_resource pwm;
		struct power_seq_gpio_resource gpio;
	};
};
#define power_seq_for_each_resource(pos, set)			\
	list_for_each_entry(pos, &(set)->resources, list)

/**
 * struct power_seq_delay_step - action data for delay steps
 * @delay:	amount of time to wait, in microseconds
 */
struct power_seq_delay_step {
	unsigned int delay;
};

/**
 * struct power_seq_regulator_step - platform data for regulator steps
 * @enable:	whether to enable or disable the regulator during this step
 */
struct power_seq_regulator_step {
	bool enable;
};

/**
 * struct power_seq_pwm_step - action data for PWM steps
 * @enable:	whether to enable or disable the PWM during this step
 */
struct power_seq_pwm_step {
	bool enable;
};

/**
 * struct power_seq_gpio_step - action data for GPIO steps
 * @enable:	whether to enable or disable the GPIO during this step
 */
struct power_seq_gpio_step {
	int value;
};

/**
 * struct power_seq_step - data for power sequences steps
 * @resource:	resource used by this step
 * @delay:	used if resource->type == POWER_SEQ_DELAY
 * @regulator:	used if resource->type == POWER_SEQ_REGULATOR
 * @pwm:	used if resource->type == POWER_SEQ_PWN
 * @gpio:	used if resource->type == POWER_SEQ_GPIO
 */
struct power_seq_step {
	struct power_seq_resource *resource;
	union {
		struct power_seq_delay_step delay;
		struct power_seq_regulator_step regulator;
		struct power_seq_pwm_step pwm;
		struct power_seq_gpio_step gpio;
	};
};

struct power_seq_set;

/**
 * struct power_seq - single power sequence
 * @id:		name of this sequence
 * @list:	link sequences together in power_seq_set. Leave as-is
 * @set:	set this sequence belongs to. Written when added to a set
 * @num_steps:	number of steps in the sequence
 * @steps:	array of steps that make the sequence
 */
struct power_seq {
	const char *id;
	struct list_head list;
	struct power_seq_set *set;
	unsigned int num_steps;
	struct power_seq_step steps[];
};

/**
 * struct power_seq_set - power sequences and resources used by a device
 * @dev:	device this set belongs to
 * @resources:	list of resources used by power sequences
 * @seqs:	list of power sequences
 */
struct power_seq_set {
	struct device *dev;
	struct list_head resources;
	struct list_head seqs;
};

/**
 * struct platform_power_seq_set - define power sequences as platform data
 * @num_seqs:	number of sequences defined
 * @seqs:	array of num_seqs power sequences
 */
struct platform_power_seq_set {
	unsigned int num_seqs;
	struct power_seq *seqs[];
};

struct platform_power_seq_set *devm_of_parse_power_seq_set(struct device *dev);
void power_seq_set_init(struct power_seq_set *set, struct device *dev);
int power_seq_set_add_sequence(struct power_seq_set *set,
			       struct power_seq *seq);
int power_seq_set_add_sequences(struct power_seq_set *set,
				struct platform_power_seq_set *seqs);
struct power_seq *power_seq_lookup(struct power_seq_set *seqs, const char *id);
int power_seq_run(struct power_seq *seq);

#endif
