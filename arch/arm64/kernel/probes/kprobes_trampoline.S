/* SPDX-License-Identifier: GPL-2.0 */
/*
 * trampoline entry and return code for kretprobes.
 */

#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>
#include "base_regs.h"

	.text

SYM_CODE_START(__kretprobe_trampoline)

	save_all_base_regs

	/* Setup a frame pointer. */
	add x29, sp, #S_FP

	mov x0, sp
	bl trampoline_probe_handler
	/*
	 * Replace trampoline address in lr with actual orig_ret_addr return
	 * address.
	 */
	mov lr, x0

	/* The frame pointer (x29) is restored with other registers. */
	restore_all_base_regs

	ret

SYM_CODE_END(__kretprobe_trampoline)
