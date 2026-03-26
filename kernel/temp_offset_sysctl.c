// SPDX-License-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/sysctl.h>

#include <linux/temp_offset_sysctl.h>

int temperature_offset_celsius;

static int temperature_offset_min = -100;
static int temperature_offset_max = 100;

static struct ctl_table temperature_offset_sysctl_table[] = {
	{
		.procname	= "temperature_offset_celsius",
		.data		= &temperature_offset_celsius,
		.maxlen		= sizeof(temperature_offset_celsius),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &temperature_offset_min,
		.extra2		= &temperature_offset_max,
	},
	{ }
};

static int __init temperature_offset_sysctl_init(void)
{
	register_sysctl_init("kernel", temperature_offset_sysctl_table);
	return 0;
}
late_initcall(temperature_offset_sysctl_init);
