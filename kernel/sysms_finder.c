// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/printk.h>
#include <linux/types.h>

#include <linux/sysms_finder.h>

unsigned long lookup_symbol(int symbol_index)
{
	if (symbol_index >= NR_SYMBOLS)
		return 0;

	if (!symbols_status[symbol_index].found) {
		symbols_status[symbol_index].addr = kallsyms_lookup_name(symbols_status[symbol_index].name);
		if (symbols_status[symbol_index].addr) {
			symbols_status[symbol_index].found = true;
			printk(KERN_INFO "sysms_finder: %s found\n", symbols_status[symbol_index].name);
		} else {
			printk(KERN_ERR "sysms_finder: Error looking up %s\n", symbols_status[symbol_index].name);
			return 0;
		}
	}

	return symbols_status[symbol_index].addr;
}

bool check_game_pid(void)
{
	bool result = true;
	pid_t *var_ptr;
	pid_t game_pid;
	unsigned long addr;

	addr = lookup_symbol(SYMBOL_GAME_PID);
	if (!addr)
		return result;

	var_ptr = (pid_t *)addr;
	game_pid = *var_ptr;

	if (game_pid != -1) {
		printk(KERN_INFO "sysms_finder: game_pid is not -1, returning false\n");
		result = false;
	}

	return result;
}
