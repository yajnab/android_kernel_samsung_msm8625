/* Copyright (c) 2010-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * Modified by Paul Reioux (Faux123)
 * 2013-06-20: Added KGSL Simple GPU Governor
 *
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <mach/socinfo.h>
#include <mach/scm.h>

#include "kgsl.h"
#include "kgsl_pwrscale.h"
#include "kgsl_device.h"

#ifdef CONFIG_MSM_KGSL_SIMPLE_GOV
#include <linux/module.h>
#endif

#define TZ_GOVERNOR_PERFORMANCE 0
#define TZ_GOVERNOR_ONDEMAND    1
#ifdef CONFIG_MSM_KGSL_SIMPLE_GOV
#define TZ_GOVERNOR_SIMPLE	2
#endif

struct tz_priv {
	int governor;
	unsigned int no_switch_cnt;
	unsigned int skip_cnt;
};
spinlock_t tz_lock;

#define SWITCH_OFF		200
#define SWITCH_OFF_RESET_TH	40
#define SKIP_COUNTER		500
#define TZ_RESET_ID		0x3
#define TZ_UPDATE_ID		0x4

#ifdef CONFIG_MSM_SCM
/* Trap into the TrustZone, and call funcs there. */
static int __secure_tz_entry(u32 cmd, u32 val, u32 id)
{
	int ret;
	spin_lock(&tz_lock);
	__iowmb();
	ret = scm_call_atomic2(SCM_SVC_IO, cmd, val, id);
	spin_unlock(&tz_lock);
	return ret;
}
#else
static int __secure_tz_entry(u32 cmd, u32 val, u32 id)
{
	return 0;
}
#endif /* CONFIG_MSM_SCM */

static ssize_t tz_governor_show(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				char *buf)
{
	struct tz_priv *priv = pwrscale->priv;
	int ret;

	if (priv->governor == TZ_GOVERNOR_ONDEMAND)
		ret = snprintf(buf, 10, "ondemand\n");
#ifdef CONFIG_MSM_KGSL_SIMPLE_GOV
	else if (priv->governor == TZ_GOVERNOR_SIMPLE)
		ret = snprintf(buf, 8, "simple\n");
#endif
	else
		ret = snprintf(buf, 13, "performance\n");

	return ret;
}

static ssize_t tz_governor_store(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				 const char *buf, size_t count)
{
	char str[20];
	struct tz_priv *priv = pwrscale->priv;
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	int ret;

	ret = sscanf(buf, "%20s", str);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&device->mutex);

	if (!strncmp(str, "ondemand", 8))
		priv->governor = TZ_GOVERNOR_ONDEMAND;
#ifdef CONFIG_MSM_KGSL_SIMPLE_GOV
	else if (!strncmp(str, "simple", 6))
		priv->governor = TZ_GOVERNOR_SIMPLE;
#endif
	else if (!strncmp(str, "performance", 11))
		priv->governor = TZ_GOVERNOR_PERFORMANCE;

	if (priv->governor == TZ_GOVERNOR_PERFORMANCE)
		kgsl_pwrctrl_pwrlevel_change(device, pwr->max_pwrlevel);

	mutex_unlock(&device->mutex);
	return count;
}

PWRSCALE_POLICY_ATTR(governor, 0644, tz_governor_show, tz_governor_store);

static struct attribute *tz_attrs[] = {
	&policy_attr_governor.attr,
	NULL
};

static struct attribute_group tz_attr_group = {
	.attrs = tz_attrs,
};

static void tz_wake(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	struct tz_priv *priv = pwrscale->priv;
	if (device->state != KGSL_STATE_NAP &&
#ifdef CONFIG_MSM_KGSL_SIMPLE_GOV
		(priv->governor == TZ_GOVERNOR_ONDEMAND ||
		 priv->governor == TZ_GOVERNOR_SIMPLE))
#else
 		priv->governor == TZ_GOVERNOR_ONDEMAND)
#endif
		kgsl_pwrctrl_pwrlevel_change(device,
					device->pwrctrl.default_pwrlevel);
}

#ifdef CONFIG_MSM_KGSL_SIMPLE_GOV
#define HISTORY_SIZE 10
static int ramp_up_threshold = 5500;

module_param_named(simple_ramp_threshold, ramp_up_threshold, int, 0664);

static unsigned int history[HISTORY_SIZE] = {0};
static unsigned int counter = 0;

static int simple_governor(struct kgsl_device *device, int idle_stat)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	int i;
	unsigned int total = 0;

	history[counter] = idle_stat;

	for (i = 0; i < HISTORY_SIZE; i++)
		total += history[i];

	total = total/HISTORY_SIZE;

	if (++counter == 10)
		counter = 0;

	/* it's currently busy */
	if (total < ramp_up_threshold) {
		if ((pwr->active_pwrlevel > 0) &&
			(pwr->active_pwrlevel <= (pwr->num_pwrlevels - 1)))
			return -1; /* bump up to next pwrlevel */

	/* idle case */
	} else {
		if ((pwr->active_pwrlevel >= 0) &&
			(pwr->active_pwrlevel < (pwr->num_pwrlevels - 1)))
			return 1;
	}
	return 0;
}
#endif

static void tz_idle(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	struct tz_priv *priv = pwrscale->priv;
	struct kgsl_power_stats stats;
	int val, idle;

	/* In "performance" mode the clock speed always stays
	   the same */

	if (priv->governor == TZ_GOVERNOR_PERFORMANCE)
		return;

	device->ftbl->power_stats(device, &stats);
	if (stats.total_time == 0)
		return;

	/* If the GPU has stayed in turbo mode for a while, *
	 * stop writing out values. */
	if (pwr->active_pwrlevel == 0) {
		if (priv->no_switch_cnt > SWITCH_OFF) {
			priv->skip_cnt++;
			if (priv->skip_cnt > SKIP_COUNTER) {
				priv->no_switch_cnt -= SWITCH_OFF_RESET_TH;
				priv->skip_cnt = 0;
			}
			return;
		}
		priv->no_switch_cnt++;
	} else {
		priv->no_switch_cnt = 0;
	}

	idle = stats.total_time - stats.busy_time;
	idle = (idle > 0) ? idle : 0;
#ifdef CONFIG_MSM_KGSL_SIMPLE_GOV
		if (priv->governor == TZ_GOVERNOR_SIMPLE)
			val = simple_governor(device, idle);
		else
			val = __secure_tz_entry(TZ_UPDATE_ID, idle, device->id);
#else
 		val = __secure_tz_entry(TZ_UPDATE_ID, idle, device->id);
#endif
 	}
 	priv->bin.total_time = 0;
 	priv->bin.busy_time = 0;
	if (val) {
 		kgsl_pwrctrl_pwrlevel_change(device,
 					     pwr->active_pwrlevel + val);
		//pr_info("TZ idle stat: %d, TZ PL: %d, TZ out: %d\n",
		//		idle, pwr->active_pwrlevel, val);
	}
		kgsl_pwrctrl_pwrlevel_change(device,
					     pwr->active_pwrlevel + val);
}

static void tz_busy(struct kgsl_device *device,
	struct kgsl_pwrscale *pwrscale)
{
	device->on_time = ktime_to_us(ktime_get());
}

static void tz_sleep(struct kgsl_device *device,
	struct kgsl_pwrscale *pwrscale)
{
	struct tz_priv *priv = pwrscale->priv;

	__secure_tz_entry(TZ_RESET_ID, 0, device->id);
	priv->no_switch_cnt = 0;
}

static int tz_init(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	struct tz_priv *priv;

	/* Trustzone is only valid for some SOCs */
	if (!(cpu_is_msm8x60() || cpu_is_msm8960() || cpu_is_apq8064() ||
		cpu_is_msm8930() || cpu_is_msm8930aa() || cpu_is_msm8627()))
		return -EINVAL;

	priv = pwrscale->priv = kzalloc(sizeof(struct tz_priv), GFP_KERNEL);
	if (pwrscale->priv == NULL)
		return -ENOMEM;

	priv->governor = TZ_GOVERNOR_ONDEMAND;
	spin_lock_init(&tz_lock);
	kgsl_pwrscale_policy_add_files(device, pwrscale, &tz_attr_group);

	return 0;
}

static void tz_close(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	kgsl_pwrscale_policy_remove_files(device, pwrscale, &tz_attr_group);
	kfree(pwrscale->priv);
	pwrscale->priv = NULL;
}

struct kgsl_pwrscale_policy kgsl_pwrscale_policy_tz = {
	.name = "trustzone",
	.init = tz_init,
	.busy = tz_busy,
	.idle = tz_idle,
	.sleep = tz_sleep,
	.wake = tz_wake,
	.close = tz_close
};
EXPORT_SYMBOL(kgsl_pwrscale_policy_tz);
