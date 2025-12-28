/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright 2021 Unisoc Inc.
 */
#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>
#include "base_regs.h"
	.align 2
	.global optinsn_slot
	optinsn_slot:
	.space  4096
	.global optprobe_template_entry
	optprobe_template_entry:

	save_all_base_regs

	mov	x1, sp
	ldr	x0, 1f
	ldr	x2, 2f
	blr	x2
	nop

	restore_all_base_regs 1

	.global optprobe_template_restore_orig_insn
	optprobe_template_restore_orig_insn:
	nop
	.global optprobe_template_restore_end
	optprobe_template_restore_end:
	nop
	.align 3
	.global optprobe_template_val
	optprobe_template_val:
1:	.space 8
	.global optprobe_template_call
	optprobe_template_call:
2:	.space 8
	.global optprobe_template_end
	optprobe_template_end:

