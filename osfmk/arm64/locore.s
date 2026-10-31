#include <machine/asm.h>
#include <arm64/machine_machdep.h>
#include <arm64/machine_routines_asm.h>
#include <arm64/proc_reg.h>
#include <pexpert/arm64/board_config.h>
#include <mach/exception_types.h>
#include <mach_kdp.h>
#include <config_dtrace.h>
#include "assym.s"
#include <arm64/exception_asm.h>
#include "dwarf_unwind.h"

#if __ARM_KERNEL_PROTECT__
#include <arm/pmap.h>
#endif

#define CBF_DISABLE 0
#define CBF_ENABLE  1

/* Заглушка под Compare-Branch Fusion (на Cortex-A53 HID1 отсутствует) */
.macro COMPARE_BRANCH_FUSION
.endmacro

/*
 * MAP_KERNEL
 * Восстановление EL1 маппинга ядра.
 */
.macro MAP_KERNEL
#if __ARM_KERNEL_PROTECT__
        mrs                x18, TTBR0_EL1
        orr                x18, x18, #(1 << TTBR_ASID_SHIFT)
        msr                TTBR0_EL1, x18
        isb                sy
        MOV64              x18, TCR_EL1_BOOT
        msr                TCR_EL1, x18
        isb                sy
#endif /* __ARM_KERNEL_PROTECT__ */
.endmacro

/*
 * BRANCH_TO_KVA_VECTOR
 */
.macro BRANCH_TO_KVA_VECTOR
#if __ARM_KERNEL_PROTECT__
        mrs                x18, TPIDR_EL1
        ldr                x18, [x18, ACT_CPUDATAP]
        ldr                x18, [x18, CPU_EXC_VECTORS]
        ldr                x18, [x18, #($1 << 3)]
        br                 x18
#else
        b                  $0
#endif /* __ARM_KERNEL_PROTECT__ */
.endmacro

/*
 * CHECK_KERNEL_STACK
 */
.macro CHECK_KERNEL_STACK unused
        stp                x2, x3, [sp, #-16]!
        and                x1, x1, #ESR_EC_MASK
        mov                x2, #(ESR_EC_SP_ALIGN << ESR_EC_SHIFT)
        cmp                x1, x2
        b.eq               Lcorrupt_stack_\@
        mov                x2, #(ESR_EC_DABORT_EL1 << ESR_EC_SHIFT)
        cmp                x1, x2
        b.ne               Lvalid_stack_\@
        mrs                x0, SP_EL0
        mrs                x1, TPIDR_EL1
Ltest_kstack_\@:
        ldr                x2, [x1, TH_KSTACKPTR]
        sub                x3, x2, KERNEL_STACK_SIZE
        cmp                x0, x2
        b.ge               Ltest_istack_\@
        cmp                x0, x3
        b.gt               Lvalid_stack_\@
Ltest_istack_\@:
        ldr                x1, [x1, ACT_CPUDATAP]
        ldr                x2, [x1, CPU_INTSTACK_TOP]
        sub                x3, x2, INTSTACK_SIZE_NUM
        cmp                x0, x2
        b.ge               Lcorrupt_stack_\@
        cmp                x0, x3
        b.gt               Lvalid_stack_\@
Lcorrupt_stack_\@:
        ldp                x2, x3, [sp], #16
        ldp                x0, x1, [sp], #16
        sub                sp, sp, ARM_CONTEXT_SIZE
        stp                x0, x1, [sp, SS64_X0]
        stp                x2, x3, [sp, SS64_X2]
        mrs                x0, SP_EL0
        str                x0, [sp, SS64_SP]
        INIT_SAVED_STATE_FLAVORS sp, w0, w1
        mov                x0, sp
        adrp               x1, fleh_invalid_stack@page
        add                x1, x1, fleh_invalid_stack@pageoff
        b                  fleh_dispatch64
Lvalid_stack_\@:
        ldp                x2, x3, [sp], #16
.endmacro

#if __ARM_KERNEL_PROTECT__
        .section __DATA_CONST,__const
        .align 3
        .globl EXT(exc_vectors_table)
LEXT(exc_vectors_table)
        .quad el1_sp0_synchronous_vector_long
        .quad el1_sp0_irq_vector_long
        .quad el1_sp0_fiq_vector_long
        .quad el1_sp0_serror_vector_long
        .quad el1_sp1_synchronous_vector_long
        .quad el1_sp1_irq_vector_long
        .quad el1_sp1_fiq_vector_long
        .quad el1_sp1_serror_vector_long
        .quad el0_synchronous_vector_64_long
        .quad el0_irq_vector_64_long
        .quad el0_fiq_vector_64_long
        .quad el0_serror_vector_64_long
#endif /* __ARM_KERNEL_PROTECT__ */

        .text
#if __ARM_KERNEL_PROTECT__
        .align 14
#else
        .align 11
#endif
        .globl EXT(ExceptionVectorsBase)
LEXT(ExceptionVectorsBase)
Lel1_sp0_synchronous_vector:
        BRANCH_TO_KVA_VECTOR el1_sp0_synchronous_vector_long, 0

        .text
        .align 7
Lel1_sp0_irq_vector:
        BRANCH_TO_KVA_VECTOR el1_sp0_irq_vector_long, 1

        .text
        .align 7
Lel1_sp0_fiq_vector:
        BRANCH_TO_KVA_VECTOR el1_sp0_fiq_vector_long, 2

        .text
        .align 7
Lel1_sp0_serror_vector:
        BRANCH_TO_KVA_VECTOR el1_sp0_serror_vector_long, 3

        .text
        .align 7
Lel1_sp1_synchronous_vector:
        BRANCH_TO_KVA_VECTOR el1_sp1_synchronous_vector_long, 4

        .text
        .align 7
Lel1_sp1_irq_vector:
        BRANCH_TO_KVA_VECTOR el1_sp1_irq_vector_long, 5

        .text
        .align 7
Lel1_sp1_fiq_vector:
        BRANCH_TO_KVA_VECTOR el1_sp1_fiq_vector_long, 6

        .text
        .align 7
Lel1_sp1_serror_vector:
        BRANCH_TO_KVA_VECTOR el1_sp1_serror_vector_long, 7

        .text
        .align 7
Lel0_synchronous_vector_64:
        MAP_KERNEL
        BRANCH_TO_KVA_VECTOR el0_synchronous_vector_64_long, 8

        .text
        .align 7
Lel0_irq_vector_64:
        MAP_KERNEL
        BRANCH_TO_KVA_VECTOR el0_irq_vector_64_long, 9

        .text
        .align 7
Lel0_fiq_vector_64:
        MAP_KERNEL
        BRANCH_TO_KVA_VECTOR el0_fiq_vector_64_long, 10

        .text
        .align 7
Lel0_serror_vector_64:
        MAP_KERNEL
        BRANCH_TO_KVA_VECTOR el0_serror_vector_64_long, 11

        .align 11

.macro EL1_SP0_VECTOR
        msr                SPSel, #0
        sub                sp, sp, ARM_CONTEXT_SIZE
        stp                x0, x1, [sp, SS64_X0]
        add                x0, sp, ARM_CONTEXT_SIZE
        str                x0, [sp, SS64_SP]
        INIT_SAVED_STATE_FLAVORS sp, w0, w1
        mov                x0, sp
.endmacro

el1_sp0_synchronous_vector_long:
        stp                x0, x1, [sp, #-16]!
        mrs                x1, ESR_EL1
        tbz                x1, #(5 + ESR_EC_SHIFT), Lkernel_stack_valid
        CHECK_KERNEL_STACK
Lkernel_stack_valid:
        ldp                x0, x1, [sp], #16
        EL1_SP0_VECTOR
        adrp               x1, EXT(fleh_synchronous)@page
        add                x1, x1, EXT(fleh_synchronous)@pageoff
        b                  fleh_dispatch64

el1_sp0_irq_vector_long:
        EL1_SP0_VECTOR
        SWITCH_TO_INT_STACK
        adrp               x1, EXT(fleh_irq)@page
        add                x1, x1, EXT(fleh_irq)@pageoff
        b                  fleh_dispatch64

el1_sp0_fiq_vector_long:
        EL1_SP0_VECTOR
        SWITCH_TO_INT_STACK
        adrp               x1, EXT(fleh_fiq)@page
        add                x1, x1, EXT(fleh_fiq)@pageoff
        b                  fleh_dispatch64

el1_sp0_serror_vector_long:
        EL1_SP0_VECTOR
        adrp               x1, EXT(fleh_serror)@page
        add                x1, x1, EXT(fleh_serror)@pageoff
        b                  fleh_dispatch64

.macro EL1_SP1_VECTOR
        sub                sp, sp, ARM_CONTEXT_SIZE
        stp                x0, x1, [sp, SS64_X0]
        add                x0, sp, ARM_CONTEXT_SIZE
        str                x0, [sp, SS64_SP]
        INIT_SAVED_STATE_FLAVORS sp, w0, w1
        mov                x0, sp
.endmacro

el1_sp1_synchronous_vector_long:
        b                  check_exception_stack
Lel1_sp1_synchronous_valid_stack:
        EL1_SP1_VECTOR
        adrp               x1, fleh_synchronous_sp1@page
        add                x1, x1, fleh_synchronous_sp1@pageoff
        b                  fleh_dispatch64

el1_sp1_irq_vector_long:
        EL1_SP1_VECTOR
        adrp               x1, fleh_irq_sp1@page
        add                x1, x1, fleh_irq_sp1@pageoff
        b                  fleh_dispatch64

el1_sp1_fiq_vector_long:
        EL1_SP1_VECTOR
        adrp               x1, fleh_fiq_sp1@page
        add                x1, x1, fleh_fiq_sp1@pageoff
        b                  fleh_dispatch64

el1_sp1_serror_vector_long:
        EL1_SP1_VECTOR
        adrp               x1, fleh_serror_sp1@page
        add                x1, x1, fleh_serror_sp1@pageoff
        b                  fleh_dispatch64

.macro EL0_64_VECTOR
        stp                x0, x1, [sp, #-16]!
        mrs                x0, TPIDR_EL1
        mrs                x1, SP_EL0
        add                x0, x0, ACT_CONTEXT
        ldr                x0, [x0]
        str                x1, [x0, SS64_SP]
        msr                SP_EL0, x0
        ldp                x0, x1, [sp], #16
        msr                SPSel, #0
        stp                x0, x1, [sp, SS64_X0]
        mrs                x1, TPIDR_EL1
        mov                x0, sp
.endmacro

el0_synchronous_vector_64_long:
        EL0_64_VECTOR      sync
        SWITCH_TO_KERN_STACK
        adrp               x1, EXT(fleh_synchronous)@page
        add                x1, x1, EXT(fleh_synchronous)@pageoff
        b                  fleh_dispatch64

el0_irq_vector_64_long:
        EL0_64_VECTOR      irq
        SWITCH_TO_INT_STACK
        adrp               x1, EXT(fleh_irq)@page
        add                x1, x1, EXT(fleh_irq)@pageoff
        b                  fleh_dispatch64

el0_fiq_vector_64_long:
        EL0_64_VECTOR      fiq
        SWITCH_TO_INT_STACK
        adrp               x1, EXT(fleh_fiq)@page
        add                x1, x1, EXT(fleh_fiq)@pageoff
        b                  fleh_dispatch64

el0_serror_vector_64_long:
        EL0_64_VECTOR      serror
        SWITCH_TO_KERN_STACK
        adrp               x1, EXT(fleh_serror)@page
        add                x1, x1, EXT(fleh_serror)@pageoff
        b                  fleh_dispatch64

        .text
        .align 2
check_exception_stack:
        mrs                x18, TPIDR_EL1
        cbz                x18, Lvalid_exception_stack
        ldr                x18, [x18, ACT_CPUDATAP]
        cbz                x18, .
        ldr                x18, [x18, CPU_EXCEPSTACK_TOP]
        cmp                sp, x18
        b.gt               .
        sub                x18, x18, EXCEPSTACK_SIZE_NUM
        cmp                sp, x18
        b.lt               .
Lvalid_exception_stack:
        mov                x18, #0
        b                  Lel1_sp1_synchronous_valid_stack

        .text
        .align 2
fleh_dispatch64:
        SPILL_REGISTERS KERNEL_MODE

        and                x23, x23, #(PSR64_MODE_EL_MASK)
        cmp                x23, #(PSR64_MODE_EL0)
        bne                1f

        SANITIZE_FPCR x25, x2, 2
2:
        mov                x2, #0
        mov                x3, #0
        mov                x4, #0
        mov                x5, #0
        mov                x6, #0
        mov                x7, #0
        mov                x8, #0
        mov                x9, #0
        mov                x10, #0
        mov                x11, #0
        mov                x12, #0
        mov                x13, #0
        mov                x14, #0
        mov                x15, #0
        mov                x16, #0
        mov                x17, #0
        mov                x18, #0
        mov                x19, #0
        mov                x20, #0
        mov                x23, #0
        mov                x24, #0
        mov                x25, #0
        mov                x27, #0
        mov                x28, #0
        mov                fp, #0
        mov                lr, #0
1:
        mov                x21, x0
        mov                x22, x1

#if !CONFIG_SKIP_PRECISE_USER_KERNEL_TIME || HAS_FAST_CNTVCT
        tst                x23, PSR64_MODE_EL_MASK
        b.ne               1f
        PUSH_FRAME
        bl                 EXT(timer_state_event_user_to_kernel)
        POP_FRAME
        mov                x0, x21
1:
#endif
        br                 x22

        .text
        .align 2
        .global EXT(fleh_synchronous)
LEXT(fleh_synchronous)
UNWIND_PROLOGUE
UNWIND_DIRECTIVES        

        mrs                x1, ESR_EL1
        mrs                x2, FAR_EL1

        and                w3, w1, #(ESR_EC_MASK)
        lsr                w3, w3, #(ESR_EC_SHIFT)
        mov                w4, #(ESR_EC_IABORT_EL1)
        cmp                w3, w4
        b.eq               Lfleh_sync_load_lr
Lvalid_link_register:
        PUSH_FRAME
        bl                 EXT(sleh_synchronous)
        POP_FRAME

        mov                x28, xzr
        b                  exception_return_dispatch

Lfleh_sync_load_lr:
        ldr                lr, [x0, SS64_LR]
        b                  Lvalid_link_register
UNWIND_EPILOGUE

.macro BEGIN_INTERRUPT_HANDLER
        mrs                x22, TPIDR_EL1
        ldr                x23, [x22, ACT_CPUDATAP]
        ldr                w1, [x23, CPU_STAT_IRQ]
        add                w1, w1, #1
        str                w1, [x23, CPU_STAT_IRQ]
        ldr                w1, [x23, CPU_STAT_IRQ_WAKE]
        add                w1, w1, #1
        str                w1, [x23, CPU_STAT_IRQ_WAKE]
        ldr                w1, [x22, ACT_PREEMPT_CNT]
        add                w1, w1, #1
        str                w1, [x22, ACT_PREEMPT_CNT]
        str                x0, [x23, CPU_INT_STATE]
.endmacro

.macro END_INTERRUPT_HANDLER
        str                xzr, [x23, CPU_INT_STATE]
        ldr                w0, [x22, ACT_PREEMPT_CNT]
        cbnz               w0, 1f
        b                  preempt_underflow
1:
        sub                w0, w0, #1
        str                w0, [x22, ACT_PREEMPT_CNT]
        ldr                x0, [x22, TH_KSTACKPTR]
        mov                sp, x0
.endmacro

        .text
        .align 2
        .global EXT(fleh_irq)
LEXT(fleh_irq)
        BEGIN_INTERRUPT_HANDLER
        PUSH_FRAME
        bl                 EXT(sleh_irq)
        POP_FRAME
        END_INTERRUPT_HANDLER

        mov                x28, #1
        b                  exception_return_dispatch

        .text
        .align 2
        .global EXT(fleh_fiq_generic)
LEXT(fleh_fiq_generic)
        PANIC_UNIMPLEMENTED

        .text
        .align 2
        .global EXT(fleh_fiq)
LEXT(fleh_fiq)
        BEGIN_INTERRUPT_HANDLER
        PUSH_FRAME
        bl                 EXT(sleh_fiq)
        POP_FRAME
        END_INTERRUPT_HANDLER

        mov                x28, #1
        b                  exception_return_dispatch

        .text
        .align 2
        .global EXT(fleh_serror)
LEXT(fleh_serror)
        mrs                x1, ESR_EL1
        mrs                x2, FAR_EL1

        PUSH_FRAME
        bl                 EXT(sleh_serror)
        POP_FRAME

        mov                x28, xzr
        b                  exception_return_dispatch

        .text
        .align 2
fleh_invalid_stack:
        mrs                x1, ESR_EL1
        str                x1, [x0, SS64_ESR]
        mrs                x2, FAR_EL1
        str                x2, [x0, SS64_FAR]
        PUSH_FRAME
        bl                 EXT(sleh_invalid_stack)
        b                  .

        .text
        .align 2
fleh_synchronous_sp1:
        mrs                x1, ESR_EL1
        str                x1, [x0, SS64_ESR]
        mrs                x2, FAR_EL1
        str                x2, [x0, SS64_FAR]
        PUSH_FRAME
        bl                 EXT(sleh_synchronous_sp1)
        b                  .

        .text
        .align 2
fleh_irq_sp1:
        mov                x1, x0
        adr                x0, Lsp1_irq_str
        b                  EXT(panic_with_thread_kernel_state)
Lsp1_irq_str:
        .asciz "IRQ exception taken while SP1 selected"

        .text
        .align 2
fleh_fiq_sp1:
        mov                x1, x0
        adr                x0, Lsp1_fiq_str
        b                  EXT(panic_with_thread_kernel_state)
Lsp1_fiq_str:
        .asciz "FIQ exception taken while SP1 selected"

        .text
        .align 2
fleh_serror_sp1:
        mov                x1, x0
        adr                x0, Lsp1_serror_str
        b                  EXT(panic_with_thread_kernel_state)
Lsp1_serror_str:
        .asciz "Asynchronous exception taken while SP1 selected"

        .text
        .align 2
exception_return_dispatch:
        ldr                w0, [x21, SS64_CPSR]
        tst                w0, PSR64_MODE_EL_MASK
        b.ne               EXT(return_to_kernel)
        b                  return_to_user

        .text
        .align 2
        .global EXT(return_to_kernel)
LEXT(return_to_kernel)
        tbnz               w0, #DAIF_IRQF_SHIFT, exception_return
        mrs                x3, TPIDR_EL1
        ldr                w1, [x3, ACT_PREEMPT_CNT]
        msr                DAIFSet, #DAIFSC_ALL
        cbnz               x1, exception_return_unint_tpidr_x3
        ldr                x1, [x3, ACT_CPUDATAP]
        ldr                x2, [x1, CPU_PENDING_AST]
        tst                x2, AST_URGENT
        b.eq               exception_return_unint_tpidr_x3
        mov                sp, x21
        PUSH_FRAME
        bl                 EXT(ast_taken_kernel)
        POP_FRAME
        b                  exception_return

        .text
        .globl EXT(thread_bootstrap_return)
LEXT(thread_bootstrap_return)
#if CONFIG_DTRACE
        bl                 EXT(dtrace_thread_bootstrap)
#endif
        b                  EXT(arm64_thread_exception_return)

        .text
        .globl EXT(arm64_thread_exception_return)
LEXT(arm64_thread_exception_return)
        mrs                x0, TPIDR_EL1
        add                x21, x0, ACT_CONTEXT
        ldr                x21, [x21]
        mov                x28, xzr

return_to_user:
check_user_asts:
        mrs                x3, TPIDR_EL1
        movn               w2, #0
        str                w2, [x3, TH_IOTIER_OVERRIDE]

#if MACH_ASSERT
        ldr                w0, [x3, TH_RWLOCK_CNT]
        cbnz               w0, rwlock_count_notzero
        ldr                w0, [x3, ACT_PREEMPT_CNT]
        cbnz               w0, preempt_count_notzero
#endif
        ldr                w0, [x3, TH_TMP_ALLOC_CNT]
        cbnz               w0, tmp_alloc_count_nozero

        msr                DAIFSet, #DAIFSC_ALL
        ldr                x4, [x3, ACT_CPUDATAP]
        ldr                x0, [x4, CPU_PENDING_AST]
        cbz                x0, no_asts
        cbz                x28, user_take_ast

        mov                x28, xzr
        mov                x19, x0

        ldr                x0, [x21, SS64_PC]
        bl                 EXT(commpage_is_in_pfz64)
        cbz                x0, restore_and_check_ast

        mov                x0, #1
        str                x0, [x21, SS64_X15]
        mov                x0, x19
        b                  no_asts

restore_and_check_ast:
        mov                x0, x19
        b                  user_take_ast
no_asts:

#if !CONFIG_SKIP_PRECISE_USER_KERNEL_TIME || HAS_FAST_CNTVCT
        mov                x19, x3
        PUSH_FRAME
        bl                 EXT(timer_state_event_kernel_to_user)
        POP_FRAME
        mov                x3, x19
#endif

        ldr                x4, [x3, ACT_CPUDATAP]
        ldr                x1, [x4, CPU_USER_DEBUG]
        ldr                x0, [x3, ACT_DEBUGDATA]
        cmp                x0, x1
        beq                L_skip_user_set_debug_state

        PUSH_FRAME
        bl                 EXT(arm_debug_set)
        POP_FRAME
        mrs                x3, TPIDR_EL1
L_skip_user_set_debug_state:

        b                  exception_return_unint_tpidr_x3

exception_return:
        msr                DAIFSet, #DAIFSC_ALL
exception_return_unint:
        mrs                x3, TPIDR_EL1
exception_return_unint_tpidr_x3:
        mov                sp, x21

exception_return_unint_tpidr_x3_dont_trash_x18:

#if __ARM_KERNEL_PROTECT__
        ldr                w1, [sp, SS64_CPSR]
        tbnz               w1, PSR64_MODE_EL_SHIFT, Lskip_el0_eret_mapping

        adrp               x0, EXT(ExceptionVectorsBase)@page
        adrp               x1, Lexception_return_restore_registers@page
        add                x1, x1, Lexception_return_restore_registers@pageoff
        MOV64              x2, ARM_KERNEL_PROTECT_EXCEPTION_START
        sub                x1, x1, x0
        add                x0, x2, x1
        br                 x0

Lskip_el0_eret_mapping:
#endif

Lexception_return_restore_registers:
        mov                x0, sp
        AUTH_THREAD_STATE_IN_X0 x20, x21, x22, x23, x24, el0_state_allowed=1

        ldr                w3, [sp, NS64_FPSR]
        ldr                w4, [sp, NS64_FPCR]

        msr                ELR_EL1, x1
        msr                SPSR_EL1, x2
        msr                FPSR, x3
        mrs                x5, FPCR
        CMSR FPCR, x5, x4, 1
1:
        ldp                q0, q1, [x0, NS64_Q0]
        ldp                q2, q3, [x0, NS64_Q2]
        ldp                q4, q5, [x0, NS64_Q4]
        ldp                q6, q7, [x0, NS64_Q6]
        ldp                q8, q9, [x0, NS64_Q8]
        ldp                q10, q11, [x0, NS64_Q10]
        ldp                q12, q13, [x0, NS64_Q12]
        ldp                q14, q15, [x0, NS64_Q14]
        ldp                q16, q17, [x0, NS64_Q16]
        ldp                q18, q19, [x0, NS64_Q18]
        ldp                q20, q21, [x0, NS64_Q20]
        ldp                q22, q23, [x0, NS64_Q22]
        ldp                q24, q25, [x0, NS64_Q24]
        ldp                q26, q27, [x0, NS64_Q26]
        ldp                q28, q29, [x0, NS64_Q28]
        ldp                q30, q31, [x0, NS64_Q30]

        ldp                x2, x3, [x0, SS64_X2]
        ldp                x4, x5, [x0, SS64_X4]
        ldp                x6, x7, [x0, SS64_X6]
        ldp                x8, x9, [x0, SS64_X8]
        ldp                x10, x11, [x0, SS64_X10]
        ldp                x12, x13, [x0, SS64_X12]
        ldp                x14, x15, [x0, SS64_X14]
        ldp                x18, x19, [x0, SS64_X18]
        ldp                x20, x21, [x0, SS64_X20]
        ldp                x22, x23, [x0, SS64_X22]
        ldp                x24, x25, [x0, SS64_X24]
        ldp                x26, x27, [x0, SS64_X26]
        ldr                x28, [x0, SS64_X28]
        ldr                fp, [x0, SS64_FP]

        ldr                x1, [x0, SS64_SP]
        mov                sp, x1

#if __ARM_KERNEL_PROTECT__
        ldr                w18, [x0, SS64_CPSR]
#endif

        ldp                x0, x1, [x0, SS64_X0]

#if __ARM_KERNEL_PROTECT__
        tbnz               w18, PSR64_MODE_EL_SHIFT, Lskip_ttbr1_switch

        MOV64              x18, TCR_EL1_USER
        msr                TCR_EL1, x18
        isb                sy

        mrs                x18, TTBR0_EL1
        bic                x18, x18, #(1 << TTBR_ASID_SHIFT)
        msr                TTBR0_EL1, x18
        mov                x18, #0
Lskip_ttbr1_switch:
#endif

        ERET_CONTEXT_SYNCHRONIZING

user_take_ast:
        PUSH_FRAME
        bl                 EXT(ast_taken_user)
        POP_FRAME
        b                  check_user_asts

        .text
        .align 2
preempt_underflow:
        mrs                x0, TPIDR_EL1
        str                x0, [sp, #-16]!
        adr                x0, L_underflow_str
        CALL_EXTERN panic

L_underflow_str:
        .asciz "Preemption count negative on thread %p"
.align 2

#if MACH_ASSERT
        .text
        .align 2
rwlock_count_notzero:
        mrs                x0, TPIDR_EL1
        str                x0, [sp, #-16]!
        ldr                w0, [x0, TH_RWLOCK_CNT]
        str                w0, [sp, #8]
        adr                x0, L_rwlock_count_notzero_str
        CALL_EXTERN panic

L_rwlock_count_notzero_str:
        .asciz "RW lock count not 0 on thread %p (%u)"

        .text
        .align 2
preempt_count_notzero:
        mrs                x0, TPIDR_EL1
        str                x0, [sp, #-16]!
        ldr                w0, [x0, ACT_PREEMPT_CNT]
        str                w0, [sp, #8]
        adr                x0, L_preempt_count_notzero_str
        CALL_EXTERN panic

L_preempt_count_notzero_str:
        .asciz "preemption count not 0 on thread %p (%u)"
#endif /* MACH_ASSERT */

        .text
        .align 2
tmp_alloc_count_nozero:
        mrs                x0, TPIDR_EL1
        CALL_EXTERN kheap_temp_leak_panic

#if __ARM_KERNEL_PROTECT__
        .text
        .align 14
        .globl EXT(ExceptionVectorsEnd)
LEXT(ExceptionVectorsEnd)
#endif

        .text
        .align 2
        .globl EXT(ml_panic_trap_to_debugger)
LEXT(ml_panic_trap_to_debugger)
        ret
