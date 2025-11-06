// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Oplus. All rights reserved.
 */
#ifndef __QOS_ARBITER_H__
#define __QOS_ARBITER_H__
#include <linux/pm_qos.h>
#include <linux/seq_file.h>
#include <linux/cpufreq.h>
#include <linux/notifier.h>

#define DEFAULT_QOS_DURATION 8
#define DEFAULT_NR_CPUS (1 << 3)

enum QOS_OWNER {
	QOS_OWNER_UAH = 0,
	QOS_OWNER_OMRG,
	QOS_OWNER_CB,
	QOS_OWNER_SBE,

	QOS_OWNER_MAX
};

enum sbe_event {
	SBE_EVENT_ACTIVATE,
	SBE_EVENT_DEACTIVE,
	QTQ_EVENT_ACTIVATE,
	QTQ_EVENT_DEACTIVE,

	SBE_EVENT_RESERVE
};

extern struct atomic_notifier_head sbe_notifier_chain;
extern s32 gDuration;


struct qos_record {
	struct plist_node node;
	struct freq_qos_request *req;
	enum QOS_OWNER owner;
	enum freq_qos_req_type type;
};

struct qos_manager {
	struct plist_head min_list;
	struct plist_head max_list;
	int min_max_val;
	int max_min_val;
	spinlock_t lock;
};

struct oplus_qos_entry {
	unsigned int min;
	unsigned int max;
	struct freq_qos_request *qos;
};

struct oplus_qos_event_data {
	int duration;
};

ssize_t oplus_update_freq_qos_request(enum QOS_OWNER owner, struct freq_qos_request *req, int val);
ssize_t oplus_add_freq_qos_request(enum QOS_OWNER owner, struct freq_constraints *qos,
									struct freq_qos_request *req, enum freq_qos_req_type type, s32 value);
ssize_t oplus_remove_freq_qos_request(enum QOS_OWNER owner, struct freq_qos_request *req);
ssize_t oplus_restore_freq_qos_request(enum QOS_OWNER owner, struct freq_qos_request *req, int val);

int sbe_register_notifier(struct notifier_block *nb);
int sbe_unregister_notifier(struct notifier_block *nb);
void sbe_notify_event(enum sbe_event, struct oplus_qos_event_data *data);
ssize_t oplus_update_sbe_val(void);

int qos_test_proc_init(void);
void dumplist(void);
#endif /* __QOS_ARBITER_H__ */
