// SPDX-License-Identifier: GPL-2.0-only
/*
 * Code for Kernel probes Jump optimization.
 *
 * Copyright (C) 2025 HiSilicon Limited
 */

#include <linux/jump_label.h>
#include <linux/kprobes.h>
#include <linux/wordpart.h>

#include <asm/cacheflush.h>
#include <asm/compiler.h>
#include <asm/insn.h>
#include <asm/kprobes.h>
#include <asm/text-patching.h>

#define OPTPROBE_BATCH_SIZE 64

#define TMPL_VAL_IDX \
	(optprobe_template_val - optprobe_template_entry)
#define TMPL_ORIGN_ADDR \
	(optprobe_template_orig_addr - optprobe_template_entry)
#define TMPL_CALL_COMMON \
	(optprobe_template_common - optprobe_template_entry)
#define TMPL_RESTORE_ORIGN_INSN \
	(optprobe_template_restore_orig_insn - optprobe_template_entry)
#define TMPL_RESTORE_END \
	(optprobe_template_restore_end - optprobe_template_entry)

#define OPT_SLOT_SIZE			65536
#define OPT_INSN_PAGES			(OPT_SLOT_SIZE / PAGE_SIZE)

static bool insn_page_in_use[OPT_INSN_PAGES];

void *alloc_optinsn_page(void)
{
	int i;

	for (i = 0; i < OPT_INSN_PAGES; i++) {
		if (!insn_page_in_use[i]) {
			insn_page_in_use[i] = true;
			return (void *)((unsigned long)optinsn_slot + PAGE_SIZE * i);
		}
	}

	return NULL;
}

void free_optinsn_page(void *page)
{
	unsigned long idx = (unsigned long)page - (unsigned long)optinsn_slot;

	WARN_ONCE(idx & (PAGE_SIZE - 1), "Invalid idx with wrong align\n");
	idx >>= PAGE_SHIFT;
	if (WARN_ONCE(idx >= OPT_INSN_PAGES, "Invalid idx with wrong size\n"))
		return;
	insn_page_in_use[idx] = false;
}

/*
 * In ARM ISA, kprobe opt always replace one instruction (4 bytes
 * aligned and 4 bytes long). It is impossible to encounter another
 * kprobe in the address range. So always return 0.
 */
int arch_check_optimized_kprobe(struct optimized_kprobe *op)
{
	return 0;
}

int arch_prepared_optinsn(struct arch_optimized_insn *optinsn)
{
	return optinsn->trampoline != NULL;
}

int arch_within_optimized_kprobe(struct optimized_kprobe *op, kprobe_opcode_t *addr)
{
	return op->kp.addr == addr;
}

static int optprobe_check_branch_limit(unsigned long pc, unsigned long addr)
{
	long offset;

	if ((pc & 0x3) || (addr & 0x3))
		return -ERANGE;

	offset = (long)addr - (long)pc;
	if (offset < -SZ_128M || offset >= SZ_128M)
		return -ERANGE;

	return 0;
}

int arch_prepare_optimized_kprobe(struct optimized_kprobe *op, struct kprobe *orig)
{
	kprobe_opcode_t *code, *buf;
	int ret = -ENOMEM;
	u32 insn;
	int i;

	buf = kzalloc(MAX_OPTINSN_SIZE, GFP_KERNEL);
	if (!buf)
		return ret;

	code = get_optinsn_slot();
	if (!code)
		goto out;

	if (optprobe_check_branch_limit((unsigned long)code, (unsigned long)orig->addr + 8)) {
		ret = -ERANGE;
		goto error;
	}

	memcpy(buf, optprobe_template_entry, MAX_OPTINSN_SIZE);

	insn = aarch64_insn_gen_branch_imm((unsigned long)&code[TMPL_CALL_COMMON],
					   (unsigned long)&optprobe_common,
					   AARCH64_INSN_BRANCH_LINK);
	if (insn == AARCH64_BREAK_FAULT) {
		ret = -ERANGE;
		goto error;
	}

	buf[TMPL_CALL_COMMON] = insn;

	insn = aarch64_insn_gen_branch_imm((unsigned long)&code[TMPL_RESTORE_END],
					   (unsigned long)(op->kp.addr + 1),
					   AARCH64_INSN_BRANCH_NOLINK);
	if (insn == AARCH64_BREAK_FAULT) {
		ret = -ERANGE;
		goto error;
	}

	buf[TMPL_RESTORE_END] = insn;

	buf[TMPL_VAL_IDX] = cpu_to_le32(lower_32_bits((unsigned long)op));
	buf[TMPL_VAL_IDX + 1] = cpu_to_le32(upper_32_bits((unsigned long)op));
	buf[TMPL_ORIGN_ADDR] = cpu_to_le32(lower_32_bits((unsigned long)orig->addr));
	buf[TMPL_ORIGN_ADDR + 1] = cpu_to_le32(upper_32_bits((unsigned long)orig->addr));

	buf[TMPL_RESTORE_ORIGN_INSN] = orig->opcode;

	/* Setup template */
	for (i = 0; i < MAX_OPTINSN_SIZE / MAX_OPTIMIZED_LENGTH; i++)
		aarch64_insn_patch_text_nosync(code + i, buf[i]);

	flush_icache_range((unsigned long)code, (unsigned long)(&code[TMPL_VAL_IDX]));
	/* Set op->optinsn.trampoline means prepared. */
	op->optinsn.trampoline = code;

	return 0;
error:
	free_optinsn_slot(code, 0);

out:
	kfree(buf);
	return ret;
}

void arch_optimize_kprobes(struct list_head *oplist)
{
	struct optimized_kprobe *op, *tmp;
	kprobe_opcode_t insns[OPTPROBE_BATCH_SIZE];
	void *addrs[OPTPROBE_BATCH_SIZE];
	int i = 0;

	list_for_each_entry_safe(op, tmp, oplist, list) {
		WARN_ON(kprobe_disabled(&op->kp));

		/*
		 * Backup instructions which will be replaced
		 * by jump address
		 */
		memcpy(op->optinsn.orig_insn, op->kp.addr, AARCH64_INSN_SIZE);

		addrs[i] = op->kp.addr;
		insns[i] = aarch64_insn_gen_branch_imm((unsigned long)op->kp.addr,
						       (unsigned long)op->optinsn.trampoline,
						       AARCH64_INSN_BRANCH_NOLINK);

		list_del_init(&op->list);
		if (++i == OPTPROBE_BATCH_SIZE)
			break;
	}

	aarch64_insn_patch_text(addrs, insns, i);
}

void arch_unoptimize_kprobe(struct optimized_kprobe *op)
{
	arch_arm_kprobe(&op->kp);
}

/*
 * Recover original instructions and breakpoints from relative jumps.
 * Caller must call with locking kprobe_mutex.
 */
void arch_unoptimize_kprobes(struct list_head *oplist,
			    struct list_head *done_list)
{
	struct optimized_kprobe *op, *tmp;
	kprobe_opcode_t insns[OPTPROBE_BATCH_SIZE];
	void *addrs[OPTPROBE_BATCH_SIZE];
	int i = 0;

	list_for_each_entry_safe(op, tmp, oplist, list) {
		addrs[i] = op->kp.addr;
		insns[i] = BRK64_OPCODE_KPROBES;
		list_move(&op->list, done_list);

		if (++i == OPTPROBE_BATCH_SIZE)
			break;
	}

	aarch64_insn_patch_text(addrs, insns, i);
}

void arch_remove_optimized_kprobe(struct optimized_kprobe *op)
{
	if (op->optinsn.trampoline) {
		free_optinsn_slot(op->optinsn.trampoline, 1);
		op->optinsn.trampoline = NULL;
	}

}

void optprobe_optimized_callback(struct optimized_kprobe *op, struct pt_regs *regs)
{
	if (kprobe_disabled(&op->kp))
		return;

	guard(preempt)();

	if (kprobe_running()) {
		kprobes_inc_nmissed_count(&op->kp);
	} else {
		__this_cpu_write(current_kprobe, &op->kp);
		get_kprobe_ctlblk()->kprobe_status = KPROBE_HIT_ACTIVE;
		opt_pre_handler(&op->kp, regs);
		__this_cpu_write(current_kprobe, NULL);
	}
}
NOKPROBE_SYMBOL(optprobe_optimized_callback)
