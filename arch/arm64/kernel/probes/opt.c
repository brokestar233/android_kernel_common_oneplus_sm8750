// SPDX-License-Identifier: GPL-2.0-only
//
// Kernel Probes Jump Optimization (Optprobes)
//
// Copyright (C) 2021, Unisoc Inc.
// Author: Janet Liu <janet.liu@unisoc.com>
#include <linux/kprobes.h>
#include <linux/jump_label.h>
#include <linux/slab.h>
#include <asm/patching.h>
#include <asm/ptrace.h>
#include <asm/kprobes.h>
#include <asm/cacheflush.h>
#include <asm/insn.h>

#define TMPL_VAL_IDX \
	((kprobe_opcode_t *)&optprobe_template_val - (kprobe_opcode_t *)&optprobe_template_entry)
#define TMPL_CALL_IDX \
	((kprobe_opcode_t *)&optprobe_template_call - (kprobe_opcode_t *)&optprobe_template_entry)
#define TMPL_END_IDX \
	((kprobe_opcode_t *)&optprobe_template_end - (kprobe_opcode_t *)&optprobe_template_entry)
#define TMPL_RESTORE_ORIGN_INSN \
	((kprobe_opcode_t *)&optprobe_template_restore_orig_insn \
	- (kprobe_opcode_t *)&optprobe_template_entry)
#define TMPL_RESTORE_END \
	((kprobe_opcode_t *)&optprobe_template_restore_end \
	- (kprobe_opcode_t *)&optprobe_template_entry)


#define MAX_OPTINSN_PAGES 16  /* 64KB / 4KB(PAGE_SIZE) = 16页 */
static int optinsn_pages_allocated = 0;

void *alloc_optinsn_page(void)
{
    /* 如果已经分配了超过16页，就停止分配 */
	if (optinsn_pages_allocated >= MAX_OPTINSN_PAGES)
		return NULL;

    /* 每次返回 &optinsn_slot 起始地址 + 偏移量 */
	return (void *)((unsigned long)&optinsn_slot + (optinsn_pages_allocated++ * PAGE_SIZE));
}

void free_optinsn_page(void *page __maybe_unused)
{
	return;
}

int arch_prepared_optinsn(struct arch_optimized_insn *optinsn)
{
	return optinsn->insn != NULL;
}

/*
 * In ARM64 ISA, kprobe opt always replace one instruction (4 bytes
 * aligned and 4 bytes long). It is impossible to encounter another
 * kprobe in the address range. So always return 0.
 */
int arch_check_optimized_kprobe(struct optimized_kprobe *op)
{
	return 0;
}

/* only optimize steppable insn */
static int can_optimize(struct kprobe *kp)
{
	if (!kp->ainsn.api.insn)
		return 0;
	return 1;
}

/* Free optimized instruction slot */
static void
__arch_remove_optimized_kprobe(struct optimized_kprobe *op, int dirty)
{
	if (op->optinsn.insn) {
		free_optinsn_slot(op->optinsn.insn, dirty);
		op->optinsn.insn = NULL;
	}
}

extern void kprobe_handler(struct pt_regs *regs);

static void
optimized_callback(struct optimized_kprobe *op, struct pt_regs *regs)
{
	unsigned long flags;
	struct kprobe_ctlblk *kcb;

	if (kprobe_disabled(&op->kp))
		return;

	/* Save skipped registers */
	regs->pc = (unsigned long)op->kp.addr;
	regs->orig_x0 = ~0UL;
	regs->stackframe[1] = (unsigned long)op->kp.addr + 4;

	local_irq_save(flags);
	kcb = get_kprobe_ctlblk();

	if (kprobe_running()) {
		kprobes_inc_nmissed_count(&op->kp);
	} else {
		__this_cpu_write(current_kprobe, &op->kp);
		kcb->kprobe_status = KPROBE_HIT_ACTIVE;
		opt_pre_handler(&op->kp, regs);
		__this_cpu_write(current_kprobe, NULL);
	}

	local_irq_restore(flags);
}
NOKPROBE_SYMBOL(optimized_callback)

int arch_prepare_optimized_kprobe(struct optimized_kprobe *op, struct kprobe *orig)
{
	kprobe_opcode_t *code;
	void **addrs;
	long offset;
	kprobe_opcode_t final_branch;
	u32 insns[8];
	int i;

	if (!can_optimize(orig))
		return -EILSEQ;

	/* Allocate instruction slot */
	code = get_optinsn_slot();
	if (!code)
		return -ENOMEM;

	/* use a 'b' instruction to branch to optinsn.insn.
	 * according armv8 manual, branch range is +/-128MB,
	 * is encoded as "imm26" times 4.
	 *   31  30      26
	 *  +---+-----------+----------------+
	 *  | 0 | 0 0 1 0 1 |      imm26     |
	 *  +---+-----------+----------------+
	 */
	offset = (long)code - (long)orig->addr;

	if (offset > 0x7ffffffL || offset < -0x8000000 || offset & 0x3) {

		free_optinsn_slot(code, 0);
		return -ERANGE;
	}

	addrs = kmalloc(MAX_OPTINSN_SIZE * sizeof(kprobe_opcode_t *), GFP_KERNEL);
	for (i = 0; i < MAX_OPTINSN_SIZE; i++)
		addrs[i] = &code[i];

	/* Copy arch-dep-instance from template. */
	aarch64_insn_patch_text(addrs,
				(kprobe_opcode_t *)&optprobe_template_entry,
				TMPL_RESTORE_ORIGN_INSN);

	/* Set probe information */
	*(unsigned long *)&insns[TMPL_VAL_IDX-TMPL_RESTORE_ORIGN_INSN] = (unsigned long)op;


	/* Set probe function call */
	*(unsigned long *)&insns[TMPL_CALL_IDX-TMPL_RESTORE_ORIGN_INSN] = (unsigned long)optimized_callback;

	final_branch = aarch64_insn_gen_branch_imm((unsigned long)(&code[TMPL_RESTORE_END]),
						   (unsigned long)(op->kp.addr) + 4,
						   AARCH64_INSN_BRANCH_NOLINK);

	/* The original probed instruction */
	if (orig->ainsn.api.insn)
		insns[0] = orig->opcode;
	else
		insns[0] = 0xd503201f; /*nop*/

	/* Jump back to next instruction */
	insns[1] = final_branch;

	aarch64_insn_patch_text(addrs + TMPL_RESTORE_ORIGN_INSN,
				insns,
				TMPL_END_IDX - TMPL_RESTORE_ORIGN_INSN);

	flush_icache_range((unsigned long)code, (unsigned long)(&code[TMPL_END_IDX]));

	/* Set op->optinsn.insn means prepared. */
	op->optinsn.insn = code;

	kfree(addrs);

	return 0;
}

void __kprobes arch_optimize_kprobes(struct list_head *oplist)
{
	struct optimized_kprobe *op, *tmp;

	list_for_each_entry_safe(op, tmp, oplist, list) {
		unsigned long insn;
		void *addrs[] = {0};
		u32 insns[] = {0};

		WARN_ON(kprobe_disabled(&op->kp));

		/*
		 * Backup instructions which will be replaced
		 * by jump address
		 */
		memcpy(op->optinsn.copied_insn, op->kp.addr,
				AARCH64_INSN_SIZE);

		insn = aarch64_insn_gen_branch_imm((unsigned long)op->kp.addr,
						   (unsigned long)op->optinsn.insn,
						   AARCH64_INSN_BRANCH_NOLINK);

		insns[0] = insn;
		addrs[0] = op->kp.addr;

		aarch64_insn_patch_text(addrs, insns, 1);

		list_del_init(&op->list);
	}
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

	list_for_each_entry_safe(op, tmp, oplist, list) {
		arch_unoptimize_kprobe(op);
		list_move(&op->list, done_list);
	}
}

int arch_within_optimized_kprobe(struct optimized_kprobe *op,
				kprobe_opcode_t *addr)
{
	return (op->kp.addr <= addr &&
		op->kp.addr + AARCH64_INSN_SIZE > addr);
}

void arch_remove_optimized_kprobe(struct optimized_kprobe *op)
{
	__arch_remove_optimized_kprobe(op, 1);
}
