/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM hmbird

#if !defined(_TRACE_HMBIRD_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HMBIRD_H

#include <linux/sched.h>
#include <linux/types.h>
#include <linux/tracepoint.h>

#define MAX_FATAL_INFO (64)

TRACE_EVENT(hmbird_fatal_info,

	TP_PROTO(unsigned int type, int pe, int lnr, int bnr, char *info),

	TP_ARGS(type, pe, lnr, bnr, info),

	TP_STRUCT__entry(
		__field(unsigned int, type)
		__field(int, pe)
		__field(int, lnr)
		__field(int, bnr)
		__array(char, info, MAX_FATAL_INFO)),

	TP_fast_assign(
		__entry->type = type;
		__entry->pe = pe;
		__entry->lnr = lnr;
		__entry->bnr = bnr;
		memcpy(__entry->info, info, MAX_FATAL_INFO);),

	TP_printk("hmbird fatal error type=%u pe=%d lnr=%d bnr=%d info=%s",
		__entry->type, __entry->pe, __entry->lnr, __entry->bnr,
		__entry->info)
);

TRACE_EVENT(hmbird_update_history,

	TP_PROTO(struct hmbird_entity *hmbird, struct rq *rq,
		struct task_struct *p, u32 runtime, int samples, int event),

	TP_ARGS(hmbird, rq, p, runtime, samples, event),

	TP_STRUCT__entry(
		__array(char,			comm, TASK_COMM_LEN)
		__field(pid_t,			pid)
		__field(unsigned int,		runtime)
		__field(int,			samples)
		__field(int,	event)
		__field(unsigned int,		demand)
		__array(u32,			hist, RAVG_HIST_SIZE)
		__field(u16,			task_util)
		__field(int,			cpu)),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid		= p->pid;
		__entry->runtime	= runtime;
		__entry->samples	= samples;
		__entry->event		= event;
		__entry->demand		= hmbird->sts.demand;
		memcpy(__entry->hist, hmbird->sts.sum_history,
					RAVG_HIST_SIZE * sizeof(u32));
		__entry->task_util	= hmbird->sts.demand_scaled;
		__entry->cpu		= rq->cpu;),

	TP_printk("comm=%s[%d]: runtime %u samples %d event %d demand %u (hist: %u %u %u %u %u) task_util %u cpu %d",
		__entry->comm, __entry->pid,
		__entry->runtime, __entry->samples,
		__entry->event,
		__entry->demand,
		__entry->hist[0], __entry->hist[1],
		__entry->hist[2], __entry->hist[3],
		__entry->hist[4],
		__entry->task_util,
		__entry->cpu)
);

#endif /*_TRACE_HMBIRD_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../kernel/sched/hmbird

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE hmbird_trace
/* This part must be outside protection */
#include <trace/define_trace.h>
