/* SPDX-License-Identifier: GPL-2.0 */
/*
 * trampoline entry and return code for optprobes.
 */

#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>

#define        OPT_SLOT_SIZE   65536

	.global optinsn_slot
optinsn_slot:
	.space  OPT_SLOT_SIZE

SYM_CODE_START(optprobe_common)
	stp x2, x3, [sp, #S_X2]
	stp x4, x5, [sp, #S_X4]
	stp x6, x7, [sp, #S_X6]
	stp x8, x9, [sp, #S_X8]
	stp x10, x11, [sp, #S_X10]
	stp x12, x13, [sp, #S_X12]
	stp x14, x15, [sp, #S_X14]
	stp x16, x17, [sp, #S_X16]
	stp x18, x19, [sp, #S_X18]
	stp x20, x21, [sp, #S_X20]
	stp x22, x23, [sp, #S_X22]
	stp x24, x25, [sp, #S_X24]
	stp x26, x27, [sp, #S_X26]
	stp x28, x29, [sp, #S_X28]
	add x2, sp, #PT_REGS_SIZE
	str x2, [sp, #S_SP]
	/* Construct a useful saved PSTATE */
	mrs x2, nzcv
	mrs x3, daif
	orr x2, x2, x3
	mrs x3, CurrentEL
	orr x2, x2, x3
	mrs x3, SPSel
	orr x2, x2, x3
	adr x1, 2f
	stp x1, x2, [sp, #S_PC]

	/* set the pt_regs address to x1 */
	mov x1, sp
	/* store lr of optprobe_common temporary */
	stp x29, x30, [sp, #-16]!
	mov x29, sp

	bl optprobe_optimized_callback

	ldp x29, x30, [sp], #16

	ldr x0, [sp, #S_PSTATE]
	and x0, x0, #(PSR_N_BIT | PSR_Z_BIT | PSR_C_BIT | PSR_V_BIT)
	msr nzcv, x0

	ldp x0, x1, [sp, #S_X0]
	ldp x2, x3, [sp, #S_X2]
	ldp x4, x5, [sp, #S_X4]
	ldp x6, x7, [sp, #S_X6]
	ldp x8, x9, [sp, #S_X8]
	ldp x10, x11, [sp, #S_X10]
	ldp x12, x13, [sp, #S_X12]
	ldp x14, x15, [sp, #S_X14]
	ldp x16, x17, [sp, #S_X16]
	ldp x18, x19, [sp, #S_X18]
	ldp x20, x21, [sp, #S_X20]
	ldp x22, x23, [sp, #S_X22]
	ldp x24, x25, [sp, #S_X24]
	ldp x26, x27, [sp, #S_X26]
	ldp x28, x29, [sp, #S_X28]
	ret
SYM_CODE_END(optprobe_common)

	.global optprobe_template_entry
optprobe_template_entry:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	adr x30, 2f
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	sub sp, sp, #PT_REGS_SIZE
	str lr, [sp, #S_LR]
	stp x0, x1, [sp, #S_X0]
	/* Get parameters to optimized_callback() */
	adr x0, 1f
	.global optprobe_template_common
optprobe_template_common:
	nop
	ldr lr, [sp, #S_LR]
	add sp, sp, #PT_REGS_SIZE
	ldp x29, x30, [sp], #16
	ldp x29, x30, [sp], #16
	.global optprobe_template_restore_orig_insn
optprobe_template_restore_orig_insn:
	nop
	.global optprobe_template_restore_end
optprobe_template_restore_end:
	nop
	.balign
	.global optprobe_template_val
optprobe_template_val:
	1:	.long 0
		.long 0
	.balign
	.global optprobe_template_orig_addr
optprobe_template_orig_addr:
	2:	.long 0
		.long 0
	.global optprobe_template_end
optprobe_template_end:
	nop
