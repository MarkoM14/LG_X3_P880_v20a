/*
 * drivers/input/input-cfboost.c
 *
 * Copyright (c) 2012-2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 */

#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/kthread.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/pm_runtime.h>
#include <linux/syscalls.h>

/* This module listens to input events and sets a temporary frequency
 * floor upon input event detection. This is based on changes to
 * cpufreq ondemand governor by:
 *
 * Tero Kristo <tero.kristo@nokia.com>
 * Brian Steuer <bsteuer@codeaurora.org>
 * David Ng <dave@codeaurora.org>
 *
 * at git://codeaurora.org/kernel/msm.git tree, commits:
 *
 * 2a6181bc76c6ce46ca0fa8e547be42acd534cf0e
 * 1cca8861d8fda4e05f6b0c59c60003345c15454d
 * 96a9aeb02bf5b3fbbef47e44460750eb275e9f1b
 * b600449501cf15928440f87eff86b1f32d14214e
 * 88a65c7ae04632ffee11f9fc628d7ab017c06b83
 */

MODULE_AUTHOR("Antti P Miettinen <amiettinen@nvidia.com>");
MODULE_DESCRIPTION("Input event CPU frequency booster");
MODULE_LICENSE("GPL v2");


static struct pm_qos_request freq_req, core_req;
static unsigned int boost_freq; /* kHz */
static int boost_freq_set(const char *arg, const struct kernel_param *kp)
{
	unsigned int old_boost = boost_freq;
	int ret = param_set_uint(arg, kp);
	if (ret == 0 && old_boost && !boost_freq)
		pm_qos_update_request(&freq_req,
				      PM_QOS_DEFAULT_VALUE);
	return ret;
}
static int boost_freq_get(char *buffer, const struct kernel_param *kp)
{
	return param_get_uint(buffer, kp);
}
static struct kernel_param_ops boost_freq_ops = {
	.set = boost_freq_set,
	.get = boost_freq_get,
};
module_param_cb(boost_freq, &boost_freq_ops, &boost_freq, 0644);
static unsigned long boost_time = 400; /* ms */
module_param(boost_time, ulong, 0644);
static unsigned long boost_cpus;
module_param(boost_cpus, ulong, 0644);

static unsigned long last_boost_jiffies;
static unsigned int start_delay = 0;

static void cfb_boost(struct kthread_work *w)
{
	if (boost_cpus > 0)
		pm_qos_update_request_timeout(&core_req, boost_cpus,
				boost_time * 1000);

	if (boost_freq > 0)
		pm_qos_update_request_timeout(&freq_req, boost_freq,
				boost_time * 1000);
}

static struct task_struct *boost_kthread;
static DEFINE_KTHREAD_WORKER(boost_worker);
static DEFINE_KTHREAD_WORK(boost_work, &cfb_boost);

static void cfb_input_event(struct input_handle *handle, unsigned int type,
			    unsigned int code, int value)
{
	if (boost_cpus > 0 || boost_freq > 0) {
		if (jiffies < last_boost_jiffies ||
			jiffies > last_boost_jiffies + msecs_to_jiffies(boost_time/2)) {
			//Block it for 15 events on boot.Android min cpu setting is affected
			if (start_delay <= 15) {
				start_delay++;
				pr_info("icfboost:Blocking boost event %d of 15\n", start_delay);
				last_boost_jiffies = jiffies;
				return;
			}
			queue_kthread_work(&boost_worker, &boost_work);
			last_boost_jiffies = jiffies;
		}
	}
}

static int cfb_input_connect(struct input_handler *handler,
			     struct input_dev *dev,
			     const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "icfboost";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void cfb_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

/* XXX make configurable */
static const struct input_device_id cfb_ids[] = {
#if 0
	{ /* touch screens send this at wakeup */
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
				INPUT_DEVICE_ID_MATCH_MSCIT,
		.evbit = { BIT_MASK(EV_MSC) },
		.mscbit = {BIT_MASK(MSC_ACTIVITY)},
	},
#endif
	{ /* trigger on any touch screen events */
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_ABS) },
	},
	/* keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = {[BIT_WORD(KEY_HOME)] = BIT_MASK(KEY_HOME) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = {[BIT_WORD(KEY_VOLUMEUP)] = BIT_MASK(KEY_VOLUMEUP) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = {[BIT_WORD(KEY_POWER)] = BIT_MASK(KEY_POWER) },
	},
	/* terminating entry */
	{ },
};

static struct input_handler cfb_input_handler = {
	.event		= cfb_input_event,
	.connect	= cfb_input_connect,
	.disconnect	= cfb_input_disconnect,
	.name		= "icfboost",
	.id_table	= cfb_ids,
};

static int __init cfboost_init(void)
{
	struct sched_param sparm = {
		/* use the last RT priority */
		.sched_priority = MAX_RT_PRIO - 10
	};
	int ret;

	/* create RT kthread */
	boost_kthread = kthread_run(&kthread_worker_fn, &boost_worker,
			"icfb-kthread");
	if (IS_ERR(boost_kthread)) {
		pr_err("icfboost: error creating worker thread\n");
		return PTR_ERR(boost_kthread);
	}

	sched_setscheduler(boost_kthread, SCHED_RR, &sparm);

	ret = input_register_handler(&cfb_input_handler);
	if (ret) {
		pr_err("icfboost: unable to register input device\n");
		kthread_stop(boost_kthread);
		return ret;
	}

	pm_qos_add_request(&core_req, PM_QOS_MIN_ONLINE_CPUS,
			   PM_QOS_DEFAULT_VALUE);
	pm_qos_add_request(&freq_req, PM_QOS_CPU_FREQ_MIN,
			   PM_QOS_DEFAULT_VALUE);

	return 0;
}

static void __exit cfboost_exit(void)
{
	/* stop input events */
	input_unregister_handler(&cfb_input_handler);
	kthread_stop(boost_kthread);
	pm_qos_remove_request(&freq_req);
	pm_qos_remove_request(&core_req);
}

module_init(cfboost_init);
module_exit(cfboost_exit);
