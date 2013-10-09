/*
 * arch/arm/common/bL_switcher.c -- big.LITTLE cluster switcher core driver
 *
 * Created by:	Nicolas Pitre, March 2012
 * Copyright:	(C) 2012-2013  Linaro Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/cpu_pm.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/clockchips.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/irqchip/arm-gic.h>
#include <linux/moduleparam.h>

#include <asm/smp_plat.h>
#include <asm/suspend.h>
#include <asm/mcpm.h>
#include <asm/bL_switcher.h>


/*
 * Use our own MPIDR accessors as the generic ones in asm/cputype.h have
 * __attribute_const__ and we don't want the compiler to assume any
 * constness here as the value _does_ change along some code paths.
 */

static int read_mpidr(void)
{
	unsigned int id;
	asm volatile ("mrc p15, 0, %0, c0, c0, 5" : "=r" (id));
	return id & MPIDR_HWID_BITMASK;
}

/*
 * bL switcher core code.
 */

static void bL_do_switch(void *_unused)
{
	unsigned ib_mpidr, ib_cpu, ib_cluster;

	pr_debug("%s\n", __func__);

	ib_mpidr = cpu_logical_map(smp_processor_id());
	ib_cpu = MPIDR_AFFINITY_LEVEL(ib_mpidr, 0);
	ib_cluster = MPIDR_AFFINITY_LEVEL(ib_mpidr, 1);

	/*
	 * Our state has been saved at this point.  Let's release our
	 * inbound CPU.
	 */
	mcpm_set_entry_vector(ib_cpu, ib_cluster, cpu_resume);
	sev();

	/*
	 * From this point, we must assume that our counterpart CPU might
	 * have taken over in its parallel world already, as if execution
	 * just returned from cpu_suspend().  It is therefore important to
	 * be very careful not to make any change the other guy is not
	 * expecting.  This is why we need stack isolation.
	 *
	 * Fancy under cover tasks could be performed here.  For now
	 * we have none.
	 */

	/* Let's put ourself down. */
	mcpm_cpu_power_down();

	/* should never get here */
	BUG();
}

/*
 * Stack isolation.  To ensure 'current' remains valid, we just use another
 * piece of our thread's stack space which should be fairly lightly used.
 * The selected area starts just above the thread_info structure located
 * at the very bottom of the stack, aligned to a cache line, and indexed
 * with the cluster number.
 */
#define STACK_SIZE 512
extern void call_with_stack(void (*fn)(void *), void *arg, void *sp);
static int bL_switchpoint(unsigned long _arg)
{
	unsigned int mpidr = read_mpidr();
	unsigned int clusterid = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	void *stack = current_thread_info() + 1;
	stack = PTR_ALIGN(stack, L1_CACHE_BYTES);
	stack += clusterid * STACK_SIZE + STACK_SIZE;
	call_with_stack(bL_do_switch, (void *)_arg, stack);
	BUG();
}

/*
 * Generic switcher interface
 */

static unsigned int bL_gic_id[MAX_CPUS_PER_CLUSTER][MAX_NR_CLUSTERS];
static int bL_switcher_cpu_pairing[NR_CPUS];

/*
 * bL_switch_to - Switch to a specific cluster for the current CPU
 * @new_cluster_id: the ID of the cluster to switch to.
 *
 * This function must be called on the CPU to be switched.
 * Returns 0 on success, else a negative status code.
 */
static int bL_switch_to(unsigned int new_cluster_id)
{
	unsigned int mpidr, this_cpu, that_cpu;
	unsigned int ob_mpidr, ob_cpu, ob_cluster, ib_mpidr, ib_cpu, ib_cluster;
	struct tick_device *tdev;
	enum clock_event_mode tdev_mode;
	int ret;

	this_cpu = smp_processor_id();
	ob_mpidr = read_mpidr();
	ob_cpu = MPIDR_AFFINITY_LEVEL(ob_mpidr, 0);
	ob_cluster = MPIDR_AFFINITY_LEVEL(ob_mpidr, 1);
	BUG_ON(cpu_logical_map(this_cpu) != ob_mpidr);

	if (new_cluster_id == ob_cluster)
		return 0;

	that_cpu = bL_switcher_cpu_pairing[this_cpu];
	ib_mpidr = cpu_logical_map(that_cpu);
	ib_cpu = MPIDR_AFFINITY_LEVEL(ib_mpidr, 0);
	ib_cluster = MPIDR_AFFINITY_LEVEL(ib_mpidr, 1);

	pr_debug("before switch: CPU %d MPIDR %#x -> %#x\n",
		 this_cpu, ob_mpidr, ib_mpidr);

	/* Close the gate for our entry vectors */
	mcpm_set_entry_vector(ob_cpu, ob_cluster, NULL);
	mcpm_set_entry_vector(ib_cpu, ib_cluster, NULL);

	/*
	 * Let's wake up the inbound CPU now in case it requires some delay
	 * to come online, but leave it gated in our entry vector code.
	 */
	ret = mcpm_cpu_power_up(ib_cpu, ib_cluster);
	if (ret) {
		pr_err("%s: mcpm_cpu_power_up() returned %d\n", __func__, ret);
		return ret;
	}

	/*
	 * From this point we are entering the switch critical zone
	 * and can't take any interrupts anymore.
	 */
	local_irq_disable();
	local_fiq_disable();

	/* redirect GIC's SGIs to our counterpart */
	gic_migrate_target(bL_gic_id[ib_cpu][ib_cluster]);

	/*
	 * Raise a SGI on the inbound CPU to make sure it doesn't stall
	 * in a possible WFI, such as in mcpm_power_down().
	 */
	arch_send_wakeup_ipi_mask(cpumask_of(this_cpu));

	tdev = tick_get_device(this_cpu);
	if (tdev && !cpumask_equal(tdev->evtdev->cpumask, cpumask_of(this_cpu)))
		tdev = NULL;
	if (tdev) {
		tdev_mode = tdev->evtdev->mode;
		clockevents_set_mode(tdev->evtdev, CLOCK_EVT_MODE_SHUTDOWN);
	}

	ret = cpu_pm_enter();

	/* we can not tolerate errors at this point */
	if (ret)
		panic("%s: cpu_pm_enter() returned %d\n", __func__, ret);

	/* Swap the physical CPUs in the logical map for this logical CPU. */
	cpu_logical_map(this_cpu) = ib_mpidr;
	cpu_logical_map(that_cpu) = ob_mpidr;

	/* Let's do the actual CPU switch. */
	ret = cpu_suspend(0, bL_switchpoint);
	if (ret > 0)
		panic("%s: cpu_suspend() returned %d\n", __func__, ret);

	/* We are executing on the inbound CPU at this point */
	mpidr = read_mpidr();
	pr_debug("after switch: CPU %d MPIDR %#x\n", this_cpu, mpidr);
	BUG_ON(mpidr != ib_mpidr);

	mcpm_cpu_powered_up();

	ret = cpu_pm_exit();

	if (tdev) {
		clockevents_set_mode(tdev->evtdev, tdev_mode);
		clockevents_program_event(tdev->evtdev,
					  tdev->evtdev->next_event, 1);
	}

	local_fiq_enable();
	local_irq_enable();

	if (ret)
		pr_err("%s exiting with error %d\n", __func__, ret);
	return ret;
}

struct bL_thread {
	struct task_struct *task;
	wait_queue_head_t wq;
	int wanted_cluster;
	struct completion started;
};

static struct bL_thread bL_threads[NR_CPUS];

static int bL_switcher_thread(void *arg)
{
	struct bL_thread *t = arg;
	struct sched_param param = { .sched_priority = 1 };
	int cluster;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &param);
	complete(&t->started);

	do {
		if (signal_pending(current))
			flush_signals(current);
		wait_event_interruptible(t->wq,
				t->wanted_cluster != -1 ||
				kthread_should_stop());
		cluster = xchg(&t->wanted_cluster, -1);
		if (cluster != -1)
			bL_switch_to(cluster);
	} while (!kthread_should_stop());

	return 0;
}

static struct task_struct *bL_switcher_thread_create(int cpu, void *arg)
{
	struct task_struct *task;

	task = kthread_create_on_node(bL_switcher_thread, arg,
				      cpu_to_node(cpu), "kswitcher_%d", cpu);
	if (!IS_ERR(task)) {
		kthread_bind(task, cpu);
		wake_up_process(task);
	} else
		pr_err("%s failed for CPU %d\n", __func__, cpu);
	return task;
}

/*
 * bL_switch_request - Switch to a specific cluster for the given CPU
 *
 * @cpu: the CPU to switch
 * @new_cluster_id: the ID of the cluster to switch to.
 *
 * This function causes a cluster switch on the given CPU by waking up
 * the appropriate switcher thread.  This function may or may not return
 * before the switch has occurred.
 */
int bL_switch_request(unsigned int cpu, unsigned int new_cluster_id)
{
	struct bL_thread *t;

	if (cpu >= ARRAY_SIZE(bL_threads)) {
		pr_err("%s: cpu %d out of bounds\n", __func__, cpu);
		return -EINVAL;
	}

	t = &bL_threads[cpu];
	if (IS_ERR(t->task))
		return PTR_ERR(t->task);
	if (!t->task)
		return -ESRCH;

	t->wanted_cluster = new_cluster_id;
	wake_up(&t->wq);
	return 0;
}
EXPORT_SYMBOL_GPL(bL_switch_request);

/*
 * Activation and configuration code.
 */

static unsigned int bL_switcher_active;
static unsigned int bL_switcher_cpu_original_cluster[NR_CPUS];
static cpumask_t bL_switcher_removed_logical_cpus;

static void bL_switcher_restore_cpus(void)
{
	int i;

	for_each_cpu(i, &bL_switcher_removed_logical_cpus)
		cpu_up(i);
}

static int bL_switcher_halve_cpus(void)
{
	int i, j, cluster_0, gic_id, ret;
	unsigned int cpu, cluster, mask;
	cpumask_t available_cpus;

	/* First pass to validate what we have */
	mask = 0;
	for_each_online_cpu(i) {
		cpu = MPIDR_AFFINITY_LEVEL(cpu_logical_map(i), 0);
		cluster = MPIDR_AFFINITY_LEVEL(cpu_logical_map(i), 1);
		if (cluster >= 2) {
			pr_err("%s: only dual cluster systems are supported\n", __func__);
			return -EINVAL;
		}
		if (WARN_ON(cpu >= MAX_CPUS_PER_CLUSTER))
			return -EINVAL;
		mask |= (1 << cluster);
	}
	if (mask != 3) {
		pr_err("%s: no CPU pairing possible\n", __func__);
		return -EINVAL;
	}

	/*
	 * Now let's do the pairing.  We match each CPU with another CPU
	 * from a different cluster.  To get a uniform scheduling behavior
	 * without fiddling with CPU topology and compute capacity data,
	 * we'll use logical CPUs initially belonging to the same cluster.
	 */
	memset(bL_switcher_cpu_pairing, -1, sizeof(bL_switcher_cpu_pairing));
	cpumask_copy(&available_cpus, cpu_online_mask);
	cluster_0 = -1;
	for_each_cpu(i, &available_cpus) {
		int match = -1;
		cluster = MPIDR_AFFINITY_LEVEL(cpu_logical_map(i), 1);
		if (cluster_0 == -1)
			cluster_0 = cluster;
		if (cluster != cluster_0)
			continue;
		cpumask_clear_cpu(i, &available_cpus);
		for_each_cpu(j, &available_cpus) {
			cluster = MPIDR_AFFINITY_LEVEL(cpu_logical_map(j), 1);
			/*
			 * Let's remember the last match to create "odd"
			 * pairings on purpose in order for other code not
			 * to assume any relation between physical and
			 * logical CPU numbers.
			 */
			if (cluster != cluster_0)
				match = j;
		}
		if (match != -1) {
			bL_switcher_cpu_pairing[i] = match;
			cpumask_clear_cpu(match, &available_cpus);
			pr_info("CPU%d paired with CPU%d\n", i, match);
		}
	}

	/*
	 * Now we disable the unwanted CPUs i.e. everything that has no
	 * pairing information (that includes the pairing counterparts).
	 */
	cpumask_clear(&bL_switcher_removed_logical_cpus);
	for_each_online_cpu(i) {
		cpu = MPIDR_AFFINITY_LEVEL(cpu_logical_map(i), 0);
		cluster = MPIDR_AFFINITY_LEVEL(cpu_logical_map(i), 1);

		/* Let's take note of the GIC ID for this CPU */
		gic_id = gic_get_cpu_id(i);
		if (gic_id < 0) {
			pr_err("%s: bad GIC ID for CPU %d\n", __func__, i);
			bL_switcher_restore_cpus();
			return -EINVAL;
		}
		bL_gic_id[cpu][cluster] = gic_id;
		pr_info("GIC ID for CPU %u cluster %u is %u\n",
			cpu, cluster, gic_id);

		if (bL_switcher_cpu_pairing[i] != -1) {
			bL_switcher_cpu_original_cluster[i] = cluster;
			continue;
		}

		ret = cpu_down(i);
		if (ret) {
			bL_switcher_restore_cpus();
			return ret;
		}
		cpumask_set_cpu(i, &bL_switcher_removed_logical_cpus);
	}

	return 0;
}

static int bL_switcher_enable(void)
{
	int cpu, ret;

	cpu_hotplug_driver_lock();
	if (bL_switcher_active) {
		cpu_hotplug_driver_unlock();
		return 0;
	}

	pr_info("big.LITTLE switcher initializing\n");

	ret = bL_switcher_halve_cpus();
	if (ret) {
		cpu_hotplug_driver_unlock();
		return ret;
	}

	for_each_online_cpu(cpu) {
		struct bL_thread *t = &bL_threads[cpu];
		init_waitqueue_head(&t->wq);
		init_completion(&t->started);
		t->wanted_cluster = -1;
		t->task = bL_switcher_thread_create(cpu, t);
	}

	bL_switcher_active = 1;
	cpu_hotplug_driver_unlock();

	pr_info("big.LITTLE switcher initialized\n");
	return 0;
}

#ifdef CONFIG_SYSFS

static void bL_switcher_disable(void)
{
	unsigned int cpu, cluster;
	struct bL_thread *t;
	struct task_struct *task;

	cpu_hotplug_driver_lock();
	if (!bL_switcher_active) {
		cpu_hotplug_driver_unlock();
		return;
	}
	bL_switcher_active = 0;

	/*
	 * To deactivate the switcher, we must shut down the switcher
	 * threads to prevent any other requests from being accepted.
	 * Then, if the final cluster for given logical CPU is not the
	 * same as the original one, we'll recreate a switcher thread
	 * just for the purpose of switching the CPU back without any
	 * possibility for interference from external requests.
	 */
	for_each_online_cpu(cpu) {
		t = &bL_threads[cpu];
		task = t->task;
		t->task = NULL;
		if (!task || IS_ERR(task))
			continue;
		kthread_stop(task);
		/* no more switch may happen on this CPU at this point */
		cluster = MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1);
		if (cluster == bL_switcher_cpu_original_cluster[cpu])
			continue;
		init_completion(&t->started);
		t->wanted_cluster = bL_switcher_cpu_original_cluster[cpu];
		task = bL_switcher_thread_create(cpu, t);
		if (!IS_ERR(task)) {
			wait_for_completion(&t->started);
			kthread_stop(task);
			cluster = MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1);
			if (cluster == bL_switcher_cpu_original_cluster[cpu])
				continue;
		}
		/* If execution gets here, we're in trouble. */
		pr_crit("%s: unable to restore original cluster for CPU %d\n",
			__func__, cpu);
		pr_crit("%s: CPU %d can't be restored\n",
			__func__, bL_switcher_cpu_pairing[cpu]);
		cpumask_clear_cpu(bL_switcher_cpu_pairing[cpu],
				  &bL_switcher_removed_logical_cpus);
	}

	bL_switcher_restore_cpus();
	cpu_hotplug_driver_unlock();
}

static ssize_t bL_switcher_active_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", bL_switcher_active);
}

static ssize_t bL_switcher_active_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret;

	switch (buf[0]) {
	case '0':
		bL_switcher_disable();
		ret = 0;
		break;
	case '1':
		ret = bL_switcher_enable();
		break;
	default:
		ret = -EINVAL;
	}

	return (ret >= 0) ? count : ret;
}

static struct kobj_attribute bL_switcher_active_attr =
	__ATTR(active, 0644, bL_switcher_active_show, bL_switcher_active_store);

static struct attribute *bL_switcher_attrs[] = {
	&bL_switcher_active_attr.attr,
	NULL,
};

static struct attribute_group bL_switcher_attr_group = {
	.attrs = bL_switcher_attrs,
};

static struct kobject *bL_switcher_kobj;

static int __init bL_switcher_sysfs_init(void)
{
	int ret;

	bL_switcher_kobj = kobject_create_and_add("bL_switcher", kernel_kobj);
	if (!bL_switcher_kobj)
		return -ENOMEM;
	ret = sysfs_create_group(bL_switcher_kobj, &bL_switcher_attr_group);
	if (ret)
		kobject_put(bL_switcher_kobj);
	return ret;
}

#endif  /* CONFIG_SYSFS */

/*
 * Veto any CPU hotplug operation on those CPUs we've removed
 * while the switcher is active.
 * We're just not ready to deal with that given the trickery involved.
 */
static int bL_switcher_hotplug_callback(struct notifier_block *nfb,
					unsigned long action, void *hcpu)
{
	if (bL_switcher_active) {
		int pairing = bL_switcher_cpu_pairing[(unsigned long)hcpu];
		switch (action & 0xf) {
		case CPU_UP_PREPARE:
		case CPU_DOWN_PREPARE:
			if (pairing == -1)
				return NOTIFY_BAD;
		}
	}
	return NOTIFY_DONE;
}

static bool no_bL_switcher;
core_param(no_bL_switcher, no_bL_switcher, bool, 0644);

static int __init bL_switcher_init(void)
{
	int ret;

	if (MAX_NR_CLUSTERS != 2) {
		pr_err("%s: only dual cluster systems are supported\n", __func__);
		return -EINVAL;
	}

	cpu_notifier(bL_switcher_hotplug_callback, 0);

	if (!no_bL_switcher) {
		ret = bL_switcher_enable();
		if (ret)
			return ret;
	}

#ifdef CONFIG_SYSFS
	ret = bL_switcher_sysfs_init();
	if (ret)
		pr_err("%s: unable to create sysfs entry\n", __func__);
#endif

	return 0;
}

late_initcall(bL_switcher_init);
