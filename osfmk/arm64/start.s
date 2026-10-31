#include <arm/proc_reg.h>
#include <arm64/asm.h>
#include <arm64/proc_reg.h>
#include <pexpert/arm64/board_config.h>
#include <mach_assert.h>
#include <machine/asm.h>
#include "assym.s"
#include <arm64/tunables/tunables.s>
#include <arm64/exception_asm.h>

#if __ARM_KERNEL_PROTECT__
#include <arm/pmap.h>
#endif /* __ARM_KERNEL_PROTECT__ */

/*
 * Сброс векторов и первичная инициализация CPU
 */
        .text
        .align 12
        .globl EXT(LowResetVectorBase)
LEXT(LowResetVectorBase)
        b                EXT(reset_vector)

        /* EL1 SP1: Векторы ошибок раннего старта */
        .align        9
        b                .
        .align        7
        b                .
        .align        7
        b                .
        .align        7
        b                .

        .align        7
        .globl EXT(reset_vector)
LEXT(reset_vector)
        msr                OSLAR_EL1, xzr
        msr                DAIFSet, #(DAIFSC_ALL)

        adrp    x0, EXT(LowExceptionVectorBase)@page
        add     x0, x0, EXT(LowExceptionVectorBase)@pageoff
        msr     VBAR_EL1, x0

        // Обработка таблицы хэндлеров сброса
        adrp        x19, EXT(ResetHandlerData)@page
        add                x19, x19, EXT(ResetHandlerData)@pageoff
        mrs                x15, MPIDR_EL1
        and                x0, x15, #0xFF

        ldr                x1, [x19, CPU_DATA_ENTRIES]
        add                x3, x1, MAX_CPUS * 16
Lcheck_cpu_data_entry:
        ldr                x21, [x1, CPU_DATA_PADDR]
        cbz                x21, Lnext_cpu_data_entry
        ldr                w2, [x21, CPU_PHYS_ID]
        cmp                x0, x2
        b.eq        Lfound_cpu_data_entry
Lnext_cpu_data_entry:
        add                x1, x1, #16
        cmp                x1, x3
        b.eq        Lskip_cpu_reset_handler
        b                Lcheck_cpu_data_entry
Lfound_cpu_data_entry:
        adrp        x20, EXT(const_boot_args)@page
        add                x20, x20, EXT(const_boot_args)@pageoff
        ldr                x0, [x21, CPU_RESET_HANDLER]
        cbz                x0, Lskip_cpu_reset_handler

        adrp        x2, EXT(resume_idle_cpu)@page
        add                x2, x2, EXT(resume_idle_cpu)@pageoff
        cmp                x0, x2
        beq                1f
        adrp        x2, EXT(start_cpu)@page
        add                x2, x2, EXT(start_cpu)@pageoff
        cmp                x0, x2
        bne                Lskip_cpu_reset_handler
1:

        blr                x0
Lskip_cpu_reset_handler:
        b                .

        .align 3
        .global EXT(LowResetVectorEnd)
LEXT(LowResetVectorEnd)

        .section __DATA_CONST,__const
        .align        3
        .globl  EXT(ResetHandlerData)
LEXT(ResetHandlerData)
        .space  (rhdSize_NUM),0
        .text

/*
 * Точка входа ядра __start
 */
        .align        3
        .globl EXT(_start)
LEXT(_start)
        b        EXT(start_first_cpu)

        .align 12, 0
        .global        EXT(LowExceptionVectorBase)
LEXT(LowExceptionVectorBase)
        /* EL1 SP0 */
        b                .
        .align        7
        b                .
        .align        7
        b                .
        .align        7
        b                .
        /* EL1 SP1 */
        .align        7
        b                .
        .align        7
        b                .
        .align        7
        b                .
        .align        7
        b                .
        /* EL0 64 */
        .align        7
        b                .
        .align        7
        b                .
        .align        7
        b                .
        .align        7
        b                .
        /* EL0 32 */
        .align        7
        b                .
        .align        7
        b                .
        .align        7
        b                .
        .align        7
        b                .
        .align 12, 0

        .align 2
        .globl EXT(resume_idle_cpu)
LEXT(resume_idle_cpu)
        adrp        lr, EXT(arm_init_idle_cpu)@page
        add                lr, lr, EXT(arm_init_idle_cpu)@pageoff
        b                start_cpu

        .align 2
        .globl EXT(start_cpu)
LEXT(start_cpu)
        adrp        lr, EXT(arm_init_cpu)@page
        add                lr, lr, EXT(arm_init_cpu)@pageoff
        b                start_cpu

        .align 2
start_cpu:
        adrp        x0, EXT(LowExceptionVectorBase)@page
        add                x0, x0, EXT(LowExceptionVectorBase)@pageoff
        msr         VBAR_EL1, x0

        ldr                x22, [x20, BA_VIRT_BASE]
        ldr                x23, [x20, BA_PHYS_BASE]
        ldr                x24, [x20, BA_MEM_SIZE]
        adrp        x25, EXT(bootstrap_pagetables)@page
        ldr                x26, [x20, BA_BOOT_FLAGS]

        ldr                x0, [x21, CPU_NUMBER_GS]
        msr                TPIDRRO_EL0, x0

        ldr                x0, [x21, CPU_EXCEPSTACK_TOP]
        msr                SPSel, #1
        mov                sp, x0

        ldr                x0, [x21, CPU_INTSTACK_TOP]
        msr                SPSel, #0
        mov                sp, x0

        add                lr, lr, x22
        sub                lr, lr, x23

        b                common_start

/* Макросы построения таблиц страниц L1/L2 */
.macro create_l1_table_entry
        and                $3,        $0, #(ARM_TT_L1_INDEX_MASK)
        lsr                $3, $3, #(ARM_TT_L1_SHIFT)
        lsl                $3, $3, #(TTE_SHIFT)
        add                $3, $1, $3
        mov                $4, #(ARM_TTE_BOOT_TABLE)
        and                $5, $2, #(ARM_TTE_TABLE_MASK)
        orr                $5, $4, $5
        str                $5, [$3]
.endmacro

.macro create_l2_block_entries
        and                $4,        $0, #(ARM_TT_L2_INDEX_MASK)
        lsr                $4, $4, #(ARM_TTE_BLOCK_L2_SHIFT)
        lsl                $4, $4, #(TTE_SHIFT)
        add                $4, $2, $4
        mov                $5, #(ARM_TTE_BOOT_BLOCK)
        and                $6, $1, #(ARM_TTE_BLOCK_L2_MASK)
        orr                $6, $5, $6
        mov                $5, $3
        mov                $7, #(ARM_TT_L2_SIZE)
1:
        str                $6, [$4], #(1 << TTE_SHIFT)
        add                $6, $6, $7
        subs        $5, $5, #1
        b.ne        1b
.endmacro

.macro create_bootstrap_mapping
        and        $5, $0, #(ARM_TT_L2_INDEX_MASK)
        lsr        $5, $5, #(ARM_TT_L2_SHIFT)
        mov        $6, #(TTE_PGENTRIES)
        sub        $5, $6, $5

3:      add        $4, $4, PGBYTES
        create_l1_table_entry        $0, $3, $4, $6, $7, $8

        cmp        $2, $5
        csel        $5, $2, $5, lt

        create_l2_block_entries        $0, $1, $4, $5, $6, $7, $8, $9

        subs        $2, $2, $5
        beq        2f

        add         $0, $0, $5, lsl #(ARM_TT_L2_SHIFT)
        add         $1, $1, $5, lsl #(ARM_TT_L2_SHIFT)

        mov        $5, #(TTE_PGENTRIES)
        b        3b
2:
.endmacro

/*
 * Старт первого ядра (Cold Boot)
 */
        .align 2
        .globl EXT(start_first_cpu)
LEXT(start_first_cpu)
        msr                OSLAR_EL1, xzr
        msr                DAIFSet, #(DAIFSC_ALL)

        mov                x20, x0
        mov                x21, #0

        adrp        x0, EXT(LowExceptionVectorBase)@page
        add                x0, x0, EXT(LowExceptionVectorBase)@pageoff
        msr         VBAR_EL1, x0

        ldr                x22, [x20, BA_VIRT_BASE]
        ldr                x23, [x20, BA_PHYS_BASE]
        ldr                x24, [x20, BA_MEM_SIZE]
        adrp        x25, EXT(bootstrap_pagetables)@page
        ldr                x26, [x20, BA_BOOT_FLAGS]

        msr                TPIDRRO_EL0, x21

        adrp        x0, EXT(excepstack_top)@page
        add                x0, x0, EXT(excepstack_top)@pageoff
        add                x0, x0, x22
        sub                x0, x0, x23

        msr                SPSel, #1
        mov                sp, x0

        adrp        x0, EXT(intstack_top)@page
        add                x0, x0, EXT(intstack_top)@pageoff
        add                x0, x0, x22
        sub                x0, x0, x23
        msr                SPSel, #0
        mov                sp, x0

        adrp        lr, EXT(arm_init)@page
        add                lr, lr, EXT(arm_init)@pageoff
        add                lr, lr, x22
        sub                lr, lr, x23

        // Обнуление первичных таблиц страниц
        mov                x0, #(ARM_TTE_EMPTY)
        mov                x1, x25
        mov                x2, #(TTE_PGENTRIES)
        lsl                x2, x2, #2

Linvalidate_bootstrap:
        str                x0, [x1], #(1 << TTE_SHIFT)
        subs        x2, x2, #1
        b.ne        Linvalidate_bootstrap

        adrp        x0, EXT(_mh_execute_header)@page
        add                x0, x0, EXT(_mh_execute_header)@pageoff
        ldr                w1, [x0, #0x18]
        tbz                w1, #0x1f, Lkernelcache_base_found
        ldr                w1, [x0, #0x20]
        cmp                w1, #0x19
        bne                .
        ldr                x1, [x0, #0x38]
        sub                x1, x0, x1
        MOV64        x0, VM_KERNEL_LINK_ADDRESS
        add                x0, x0, x1

Lkernelcache_base_found:
        sub                x18, x0, x23
        sub                x24, x24, x18
        add                x22, x22, x18
        add                x23, x23, x18

        mov                x4, x0
        mov                x14, x22
        mov                x15, x23

        mov                x1, x25
        add                x3, x1, PGBYTES
        mov                x2, x3

        lsr                x5,  x24, #(ARM_TT_L2_SHIFT)
        create_bootstrap_mapping x0,  x4,  x5, x1, x2, x6, x10, x11, x12, x13

        lsr                x5,  x24, #(ARM_TT_L2_SHIFT)
        create_bootstrap_mapping x14, x15, x5, x3, x2, x9, x10, x11, x12, x13

        dsb                ish
        b                common_start

/*
 * Общая инициализация CPU и включение MMU
 */
common_start:
        adrp        x0,     EXT(sysreg_restore)@page
        add                x0, x0, EXT(sysreg_restore)@pageoff
        ldr                x1, [x0, SR_RESTORE_TCR_EL1]
        msr         TCR_EL1, x1

        and                x0, x25, #(TTBR_BADDR_MASK)
        mov                x19, lr
        bl                EXT(set_mmu_ttb)
        mov                lr, x19
        add                x0, x25, PGBYTES
        and                x0, x0, #(TTBR_BADDR_MASK)
        msr         TTBR1_EL1, x0

        // Настройка атрибутов памяти MAIR
        mov                x0, xzr
        mov                x1, #(MAIR_WRITEBACK << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_WRITEBACK))
        orr                x0, x0, x1
        mov                x1, #(MAIR_INNERWRITEBACK << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_INNERWRITEBACK))
        orr                x0, x0, x1
        mov                x1, #(MAIR_DISABLE << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_DISABLE))
        orr                x0, x0, x1
        mov                x1, #(MAIR_WRITETHRU << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_WRITETHRU))
        orr                x0, x0, x1
        mov                x1, #(MAIR_WRITECOMB << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_WRITECOMB))
        orr                x0, x0, x1
        mov                x1, #(MAIR_POSTED << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_POSTED))
        orr                x0, x0, x1
        mov                x1, #(MAIR_POSTED_REORDERED << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_POSTED_REORDERED))
        orr                x0, x0, x1
        mov                x1, #(MAIR_POSTED_COMBINED_REORDERED << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_POSTED_COMBINED_REORDERED))
        orr                x0, x0, x1
        msr                MAIR_EL1, x0
        isb
        tlbi        vmalle1
        dsb                ish

        cbz                x21, 1f
        adrp        x0, EXT(cpu_ttep)@page
        add                x0, x0, EXT(cpu_ttep)@pageoff
        ldr                x0, [x0]
        msr         TTBR1_EL1, x0
1:

        adrp        x0, EXT(ExceptionVectorsBase)@page
        add                x0, x0, EXT(ExceptionVectorsBase)@pageoff
        add                x0, x0, x22
        sub                x0, x0, x23
        msr         VBAR_EL1, x0

        // Включаем MMU и кэши через дефолтный SCTLR_EL1
        MOV64        x0, SCTLR_EL1_DEFAULT
        msr         SCTLR_EL1, x0
        isb                sy

        // Включаем SIMD / NEON
        mov                x0, #(CPACR_FPEN_ENABLE)
        msr                CPACR_EL1, x0

        msr                TPIDR_EL1, xzr

        mrs         x12, MIDR_EL1
        APPLY_TUNABLES x12, x13

        cbnz        x21, Ltrampoline

        add                x0, x20, x22
        sub                x0, x0, x23

#if KASAN
        mov        x20, x0
        mov        x21, lr
        mrs        x1, TTBR1_EL1
        bl        EXT(kasan_bootstrap)
        mov        x0, x20
        mov        lr, x21
#endif

        ret

Ltrampoline:
        adrp        x0, arm_init_tramp@page
        add                x0, x0, arm_init_tramp@pageoff
        add                x0, x0, x22
        sub                x0, x0, x23
        br                x0

        .text
        .align 2
arm_init_tramp:
        mov                x19, lr
#if defined(HAS_VMSA_LOCK)
        bl                EXT(vmsa_lock)
#endif
        mov                x0, x21
        bl                EXT(phystokv)
        mov                lr, x19
        ret
