/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GAME_PID_H
#define _LINUX_GAME_PID_H
#include <linux/types.h>


enum {
	SYMBOL_GAME_PID,
	SYMBOL_OPLUS_DISPLAY_IS_SCREEN_OFF,
	SYMBOL_GET_CONNECTING_STATE,
	NR_SYMBOLS,
};

struct symbol_entry {
	const char *name;
	unsigned long addr;
	bool found;
};

static struct symbol_entry symbols_status[NR_SYMBOLS] = {
	[SYMBOL_GAME_PID] = {
		.name = "game_pid",
		.addr = 0,
		.found = false,
	},
	[SYMBOL_OPLUS_DISPLAY_IS_SCREEN_OFF] = {
		.name = "oplus_display_is_screen_off",
		.addr = 0,
		.found = false,
	},
	[SYMBOL_GET_CONNECTING_STATE] = {
		.name = "get_connecting_state",
		.addr = 0,
		.found = false,
	},
};

unsigned long lookup_symbol(int symbol_index);
bool check_game_pid(void);
bool check_screen_off_state(void);
bool check_charging_state(void);

#endif /* _LINUX_GAME_PID_H */
