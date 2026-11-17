// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/reboot.h>
#include <linux/pm.h>
#include <linux/pstore_ram.h>

static void __iomem *msm_ps_hold;

static int do_msm_poweroff(struct sys_off_data *data)
{
	ramoops_reset_mark(RAMOOPS_RESET_PSHOLD, NULL);
	writel(0, msm_ps_hold);
	mdelay(10000);

	return NOTIFY_DONE;
}

/*
 * Register from an early initcall instead of a platform driver: on some
 * firmware PSCI SYSTEM_RESET returns without resetting, and a panic raised
 * before device initcalls would otherwise find no restart handler at all.
 */
static int __init msm_restart_init(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "qcom,pshold");
	if (!np)
		return 0;

	msm_ps_hold = of_iomap(np, 0);
	of_node_put(np);
	if (!msm_ps_hold)
		return -ENOMEM;

	register_sys_off_handler(SYS_OFF_MODE_RESTART, 128, do_msm_poweroff,
				 NULL);
	register_sys_off_handler(SYS_OFF_MODE_POWER_OFF, SYS_OFF_PRIO_DEFAULT,
				 do_msm_poweroff, NULL);

	return 0;
}
early_initcall(msm_restart_init);
