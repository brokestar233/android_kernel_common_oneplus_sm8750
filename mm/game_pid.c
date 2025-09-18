// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/printk.h>
#include <linux/types.h>

#include "internal.h"
#include <linux/game_pid.h>

static bool found = false;
static u64 addr = 0;

bool check_game_pid(void)
{
	bool result = true;
	pid_t *var_ptr;
	pid_t game_pid;

	if (!found) {
		addr = kallsyms_lookup_name("game_pid");
		if (addr) {
			found = true;
		} else {
			printk(KERN_ERR "Error looking up game_pid\n");
		}
	}

	if (found && addr) {
		var_ptr = (pid_t *)addr;
		game_pid = *var_ptr;

		if (game_pid != -1) {
			printk(KERN_INFO "game_pid is not -1, returning false\n");
			result = false;
		}
	}

	return result;
}