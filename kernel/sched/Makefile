# SPDX-License-Identifier: GPL-2.0

# The compilers are complaining about unused variables inside an if(0) scope
# block. This is daft, shut them up.
ccflags-y += $(call cc-disable-warning, unused-but-set-variable)

# These files are disabled because they produce non-interesting flaky coverage
# that is not a function of syscall inputs. E.g. involuntary context switches.
KCOV_INSTRUMENT := n

# Disable KCSAN to avoid excessive noise and performance degradation. To avoid
# false positives ensure barriers implied by sched functions are instrumented.
KCSAN_SANITIZE := n
KCSAN_INSTRUMENT_BARRIERS := y

ifneq ($(CONFIG_SCHED_OMIT_FRAME_POINTER),y)
# According to Alan Modra <alan@linuxcare.com.au>, the -fno-omit-frame-pointer is
# needed for x86 only.  Why this used to be enabled for all architectures is beyond
# me.  I suspect most platforms don't need this, but until we know that for sure
# I turn this off for IA-64 only.  Andreas Schwab says it's also needed on m68k
# to get a correct value for the wait-channel (WCHAN in ps). --davidm
CFLAGS_core.o := $(PROFILING) -fno-omit-frame-pointer
endif

#
# Build efficiency:
#
# These compilation units have roughly the same size and complexity - so their
# build parallelizes well and finishes roughly at once:
#
obj-y += core.o
obj-y += fair.o
obj-y += build_policy.o
obj-y += build_utility.o
obj-y += bore.o
obj-$(CONFIG_ANDROID_VENDOR_HOOKS) += vendor_hooks.o

# Cambyses: cambyses.c is #included from fair.c (needs access to static functions).
# Cambyses SIMD: Separate TUs with ISA-specific flags.
# FPU_KILL_FLAGS must be removed first (kernel builds with -mno-sse etc.),
# then ISA flags added.  Following Nap's pattern (drivers/cpuidle/governors/nap/).
ifdef CONFIG_SCHED_CAMBYSES_SIMD

ifdef CONFIG_X86_64
obj-y += cambyses_simd_ssse3.o cambyses_simd_avx2.o

CAMBYSES_FPU_KILL_FLAGS := -mno-sse -mno-sse2 -mno-mmx -mno-avx -mno-3dnow \
                           -mno-sse4a -msoft-float -mno-80387 -mno-fp-ret-in-387

CFLAGS_REMOVE_cambyses_simd_ssse3.o += $(CAMBYSES_FPU_KILL_FLAGS)
CFLAGS_REMOVE_cambyses_simd_avx2.o  += $(CAMBYSES_FPU_KILL_FLAGS)

CFLAGS_cambyses_simd_ssse3.o += $(CC_FLAGS_FPU) -mssse3
CFLAGS_cambyses_simd_avx2.o  += $(CC_FLAGS_FPU) -mavx -mavx2
endif # CONFIG_X86_64

ifdef CONFIG_ARM64
obj-y += cambyses_simd_neon.o

# Remove -mgeneral-regs-only to allow NEON/FP instructions
CFLAGS_REMOVE_cambyses_simd_neon.o += -mgeneral-regs-only $(CC_FLAGS_NO_FPU)

CLANG_RESOURCE_DIR := $(shell $(CC) -print-resource-dir)

# Add FPU flags: -ffreestanding -isystem <compiler include path>
CFLAGS_cambyses_simd_neon.o += $(CC_FLAGS_FPU) \
                               -I$(CLANG_RESOURCE_DIR)/include \
                               -march=armv8-a+simd \
                               -ffreestanding
endif # CONFIG_ARM64

endif # CONFIG_SCHED_CAMBYSES_SIMD
