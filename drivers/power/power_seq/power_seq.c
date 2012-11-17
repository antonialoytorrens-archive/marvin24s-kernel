/*
 * power_seq.c - power sequence interpreter for platform devices and device tree
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

#include <linux/power_seq.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/device.h>

#include <linux/of.h>

#define power_seq_err(seq, step_nbr, format, ...)			\
	dev_err(seq->set->dev, "%s[%d]: " format, seq->id, step_nbr,	\
	##__VA_ARGS__);

/**
 * struct power_seq_res_ops - operators for power sequences resources
 * @name:		Name of the resource type. Set to null when a resource
 *			type support is not compiled in
 * @of_parse:		Parse a step for this kind of resource from a device
 *			tree node. The result of parsing must be written into
 *			step step_nbr of seq
 * @step_run:		Run a step for this kind of resource
 * @res_compare:	Return true if the resource used by the resource is the
 *			same as the one referenced by the step, false otherwise.
 * @res_alloc:		Resolve and allocate a resource. Return error code if
 *			the resource cannot be allocated, 0 otherwise
 */
struct power_seq_res_ops {
	const char *name;
	int (*of_parse)(struct device_node *node, struct power_seq *seq,
			unsigned int step_nbr, struct power_seq_resource *res);
	int (*step_run)(struct power_seq_step *step);
	bool (*res_compare)(struct power_seq_resource *res,
			    struct power_seq_resource *res2);
	int (*res_alloc)(struct device *dev,
			 struct power_seq_resource *res);
};

static const struct power_seq_res_ops power_seq_ops[POWER_SEQ_NUM_TYPES];

#ifdef CONFIG_OF
static int of_power_seq_parse_enable_properties(struct device_node *node,
						struct power_seq *seq,
						unsigned int step_nbr,
						bool *enable)
{
	if (of_find_property(node, "enable", NULL)) {
		*enable = true;
	} else if (of_find_property(node, "disable", NULL)) {
		*enable = false;
	} else {
		power_seq_err(seq, step_nbr,
			      "missing enable or disable property\n");
		return -EINVAL;
	}

	return 0;
}

static int of_power_seq_parse_step(struct device *dev,
				   struct device_node *node,
				   struct power_seq *seq,
				   unsigned int step_nbr,
				   struct list_head *resources)
{
	struct power_seq_step *step = &seq->steps[step_nbr];
	struct power_seq_resource res, *res2;
	const char *type;
	int i, err;

	err = of_property_read_string(node, "type", &type);
	if (err < 0) {
		power_seq_err(seq, step_nbr, "cannot read type property\n");
		return err;
	}
	for (i = 0; i < POWER_SEQ_NUM_TYPES; i++) {
		if (power_seq_ops[i].name == NULL)
			continue;
		if (!strcmp(type, power_seq_ops[i].name))
			break;
	}
	if (i >= POWER_SEQ_NUM_TYPES) {
		power_seq_err(seq, step_nbr, "unknown type %s\n", type);
		return -EINVAL;
	}
	memset(&res, 0, sizeof(res));
	res.type = i;
	err = power_seq_ops[res.type].of_parse(node, seq, step_nbr, &res);
	if (err < 0)
		return err;

	/* Use the same instance of the resource if met before */
	list_for_each_entry(res2, resources, list) {
		if (res.type == res2->type &&
		    power_seq_ops[res.type].res_compare(&res, res2))
			break;
	}
	/* Resource never met before, create it */
	if (&res2->list == resources) {
		res2 = devm_kzalloc(dev, sizeof(*res2), GFP_KERNEL);
		if (!res2)
			return -ENOMEM;
		memcpy(res2, &res, sizeof(res));
		list_add_tail(&res2->list, resources);
	}
	step->resource = res2;

	return 0;
}

static struct power_seq *of_parse_power_seq(struct device *dev,
					    struct device_node *node,
					    struct list_head *resources)
{
	struct device_node *child = NULL;
	struct power_seq *pseq;
	int num_steps, sz;
	int err;

	if (!node)
		return ERR_PTR(-EINVAL);

	num_steps = of_get_child_count(node);
	sz = sizeof(*pseq) + sizeof(pseq->steps[0]) * num_steps;
	pseq = devm_kzalloc(dev, sz, GFP_KERNEL);
	if (!pseq)
		return ERR_PTR(-ENOMEM);
	pseq->id = node->name;
	pseq->num_steps = num_steps;

	for_each_child_of_node(node, child) {
		unsigned int pos;

		/* Check that the name's format is correct and within bounds */
		if (strncmp("step", child->name, 4)) {
			err = -EINVAL;
			goto parse_error;
		}

		err = kstrtouint(child->name + 4, 10, &pos);
		if (err < 0)
			goto parse_error;

		/* Invalid step index or step already parsed? */
		if (pos >= num_steps || pseq->steps[pos].resource != NULL) {
			err = -EINVAL;
			goto parse_error;
		}

		err = of_power_seq_parse_step(dev, child, pseq, pos, resources);
		if (err)
			return ERR_PTR(err);
	}

	return pseq;

parse_error:
	dev_err(dev, "%s: invalid power step name %s!\n", pseq->id,
		child->name);
	return ERR_PTR(err);
}

/**
 * devm_of_parse_power_seq_set - build a power_seq_set from the device tree
 * @dev:	Device to parse the power sequences of
 *
 * Sequences must be contained into a subnode named "power-sequences" of the
 * device root node.
 *
 * Memory for the sequence is allocated using devm_kzalloc on dev. The returned
 * platform_power_seq_set can be freed by devm_kfree after the sequences have
 * been added, but the sequences themselves must be preserved.
 *
 * Returns the built set on success, or an error code in case of failure.
 */
struct platform_power_seq_set *devm_of_parse_power_seq_set(struct device *dev)
{
	struct platform_power_seq_set *set;
	struct device_node *root = dev->of_node;
	struct device_node *seq;
	struct list_head resources;
	int n, sz;

	if (!root)
		return NULL;

	root = of_find_node_by_name(root, "power-sequences");
	if (!root)
		return NULL;

	n = of_get_child_count(root);
	sz = sizeof(*set) + sizeof(struct power_seq *) * n;
	set = devm_kzalloc(dev, sz, GFP_KERNEL);
	if (!set)
		return ERR_PTR(-ENOMEM);
	set->num_seqs = n;

	n = 0;
	INIT_LIST_HEAD(&resources);
	for_each_child_of_node(root, seq) {
		struct power_seq *pseq;

		pseq = of_parse_power_seq(dev, seq, &resources);
		if (IS_ERR(pseq))
			return (void *)pseq;

		set->seqs[n++] = pseq;
	}

	return set;
}
EXPORT_SYMBOL_GPL(devm_of_parse_power_seq_set);
#endif /* CONFIG_OF */

/**
 * power_seq_set_init - initialize a power_seq_set
 * @set:	Set to initialize
 * @dev:	Device this set is going to belong to
 */
void power_seq_set_init(struct power_seq_set *set, struct device *dev)
{
	set->dev = dev;
	INIT_LIST_HEAD(&set->resources);
	INIT_LIST_HEAD(&set->seqs);
}
EXPORT_SYMBOL_GPL(power_seq_set_init);

/**
 * power_seq_add_sequence - add a power sequence to a set
 * @set:	Set to add the sequence to
 * @seq:	Sequence to add
 *
 * This step will check that all the resources used by the sequence are
 * allocated. If they are not, an attempt to allocate them is made. This
 * operation can fail and and return an error code.
 *
 * Returns 0 on success, error code if a resource initialization failed.
 */
int power_seq_add_sequence(struct power_seq_set *set, struct power_seq *seq)
{
	struct power_seq_resource *res;
	int i, err;

	for (i = 0; i < seq->num_steps; i++) {
		struct power_seq_step *step = &seq->steps[i];
		struct power_seq_resource *step_res = step->resource;
		list_for_each_entry(res, &set->resources, list) {
			if (res == step_res)
				break;
		}
		/* resource not allocated yet, allocate and add it */
		if (&res->list == &set->resources) {
			err = power_seq_ops[step_res->type].res_alloc(set->dev,
								      step_res);
			if (err)
				return err;
			list_add_tail(&step->resource->list, &set->resources);
		}
	}

	list_add_tail(&seq->list, &set->seqs);
	seq->set = set;

	return 0;
}
EXPORT_SYMBOL_GPL(power_seq_add_sequence);

/**
 * power_seq_add_sequences - add power sequences defined as platform data
 * @set:	Set to add the sequences to
 * @seqs:	Sequences to add
 *
 * See power_seq_add_sequence for more details.
 *
 * Returns 0 on success, error code if a resource initialization failed.
 */
int power_seq_set_add_sequences(struct power_seq_set *set,
				struct platform_power_seq_set *seqs)
{
	int i, ret;

	for (i = 0; i < seqs->num_seqs; i++) {
		ret = power_seq_add_sequence(set, seqs->seqs[i]);
		if (ret < 0)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(power_seq_set_add_sequences);

/**
 * power_seq_lookup - Lookup a power sequence by name from a set
 * @seqs:	The set to look in
 * @id:		Name to look after
 *
 * Returns a matching power sequence if it exists, NULL if it does not.
 */
struct power_seq *power_seq_lookup(struct power_seq_set *set, const char *id)
{
	struct power_seq *seq;

	list_for_each_entry(seq, &set->seqs, list) {
		if (!strcmp(seq->id, id))
			return seq;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(power_seq_lookup);

/**
 * power_seq_run() - run a power sequence
 * @seq:	The power sequence to run
 *
 * Returns 0 on success, error code in case of failure.
 */
int power_seq_run(struct power_seq *seq)
{
	unsigned int i;
	int err;

	if (!seq)
		return 0;

	if (!seq->set) {
		pr_err("cannot run a sequence not added to a set");
		return -EINVAL;
	}

	for (i = 0; i < seq->num_steps; i++) {
		unsigned int type = seq->steps[i].resource->type;

		err = power_seq_ops[type].step_run(&seq->steps[i]);
		if (err) {
			power_seq_err(seq, i,
				"error %d while running power sequence step\n",
				err);
			return err;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(power_seq_run);

#include "power_seq_delay.c"
#include "power_seq_regulator.c"
#include "power_seq_pwm.c"
#include "power_seq_gpio.c"

static const struct power_seq_res_ops power_seq_ops[POWER_SEQ_NUM_TYPES] = {
	[POWER_SEQ_DELAY] = POWER_SEQ_DELAY_TYPE,
	[POWER_SEQ_REGULATOR] = POWER_SEQ_REGULATOR_TYPE,
	[POWER_SEQ_PWM] = POWER_SEQ_PWM_TYPE,
	[POWER_SEQ_GPIO] = POWER_SEQ_GPIO_TYPE,
};

MODULE_AUTHOR("Alexandre Courbot <acourbot@nvidia.com>");
MODULE_DESCRIPTION("Runtime Interpreted Power Sequences");
MODULE_LICENSE("GPL v2");
