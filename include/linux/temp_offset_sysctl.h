/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TEMP_OFFSET_SYSCTL_H
#define _LINUX_TEMP_OFFSET_SYSCTL_H

extern int temperature_offset_celsius;

static inline int thermal_temp_offset_mc(void)
{
	return temperature_offset_celsius * 1000;
}

static inline int power_supply_temp_offset_deci_c(void)
{
	return temperature_offset_celsius * 10;
}

#endif /* _LINUX_TEMP_OFFSET_SYSCTL_H */
