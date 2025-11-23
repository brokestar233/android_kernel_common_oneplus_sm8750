/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GAME_PID_H
#define _LINUX_GAME_PID_H
#include <linux/types.h>


enum {
    SYMBOL_OPLUS_MMS_GET_BY_NAME,
    SYMBOL_OPLUS_MMS_GET_ITEM_DATA,
    SYMBOL_OPLUS_PPS_PDO_SET,
    SYMBOL_OPLUS_UFCS_PDO_SET,
    SYMBOL_OPLUS_CPA_REQUEST,
    SYMBOL_VOTE,
    SYMBOL_OPLUS_MMS_SUBSCRIBE,
    SYMBOL_FIND_VOTABLE,
	SYMBOL_OPLUS_MMS_GET_DRVDATA,
    SYMBOL_GAME_PID,
	NR_SYMBOLS,
};

struct symbol_entry {
	const char *name;
	unsigned long addr;
	bool found;
};

static struct symbol_entry symbols_status[NR_SYMBOLS] = {
    [SYMBOL_OPLUS_MMS_GET_BY_NAME] = {
		.name = "oplus_mms_get_by_name",
		.addr = 0,
		.found = false,
	},
    [SYMBOL_OPLUS_MMS_GET_ITEM_DATA] = {
		.name = "oplus_mms_get_item_data",
		.addr = 0,
		.found = false,
	},
    [SYMBOL_OPLUS_PPS_PDO_SET] = {
		.name = "oplus_pps_pdo_set",
		.addr = 0,
		.found = false,
	},
    [SYMBOL_OPLUS_UFCS_PDO_SET] = {
		.name = "oplus_ufcs_pdo_set",
		.addr = 0,
		.found = false,
	},
    [SYMBOL_OPLUS_CPA_REQUEST] = {
		.name = "oplus_cpa_request",
		.addr = 0,
		.found = false,
	},
    [SYMBOL_VOTE] = {
		.name = "vote",
		.addr = 0,
		.found = false,
	},
    [SYMBOL_OPLUS_MMS_SUBSCRIBE] = {
		.name = "oplus_mms_subscribe",
		.addr = 0,
		.found = false,
	},
    [SYMBOL_FIND_VOTABLE] = {
		.name = "find_votable",
		.addr = 0,
		.found = false,
	},
	[SYMBOL_OPLUS_MMS_GET_DRVDATA] = {
		.name = "oplus_mms_get_drvdata",
		.addr = 0,
		.found = false,
	},
    [SYMBOL_GAME_PID] = {
		.name = "game_pid",
		.addr = 0,
		.found = false,
	},
};

unsigned long lookup_symbol(int symbol_index);
bool check_game_pid(void);

#endif /* _LINUX_GAME_PID_H */
