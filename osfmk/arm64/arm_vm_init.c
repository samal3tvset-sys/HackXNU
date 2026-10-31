#include <mach_debug.h>
#include <mach_kdp.h>
#include <debug.h>

#include <kern/assert.h>
#include <kern/misc_protos.h>
#include <kern/monotonic.h>
#include <mach/vm_types.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>
#include <vm/vm_page.h>
#include <vm/pmap.h>

#include <machine/atomic.h>
#include <arm64/proc_reg.h>
#include <arm64/lowglobals.h>
#include <arm/cpu_data_internal.h>
#include <arm/misc_protos.h>
#include <pexpert/arm64/boot.h>
#include <pexpert/device_tree.h>

#include <libkern/kernel_mach_header.h>
#include <libkern/section_keywords.h>

#include <san/kasan.h>

#define ARM_DYNAMIC_TABLE_XN (ARM_TTE_TABLE_PXN | ARM_TTE_TABLE_XN)

#if KASAN
extern vm_offset_t shadow_pbase;
extern vm_offset_t shadow_ptop;
extern vm_offset_t physmap_vbase;
extern vm_offset_t physmap_vtop;
#endif

SECURITY_READ_ONLY_LATE(uint8_t) bootstrap_pagetables[BOOTSTRAP_TABLE_SIZE] __attribute__((aligned(ARM_PGBYTES)));

extern void *last_kernel_symbol;
extern void arm64_replace_bootstack(cpu_data_t*);
extern void PE_slide_devicetree(vm_offset_t);

/* KASLR parameters */
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kernel_base;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kernel_top;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kext_base;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kext_top;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kernel_stext;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kernel_etext;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kernel_slide;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kernel_slid_base;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kernel_slid_top;

SECURITY_READ_ONLY_LATE(vm_offset_t) vm_prelink_stext;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_prelink_etext;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_prelink_sdata;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_prelink_edata;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_prelink_sinfo;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_prelink_einfo;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_slinkedit;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_elinkedit;

SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kernel_builtinkmod_text;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kernel_builtinkmod_text_end;

SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kernelcache_base;
SECURITY_READ_ONLY_LATE(vm_offset_t) vm_kernelcache_top;

SECURITY_READ_ONLY_LATE(unsigned long) gVirtBase;
SECURITY_READ_ONLY_LATE(unsigned long) gPhysBase;
SECURITY_READ_ONLY_LATE(unsigned long) gPhysSize;
SECURITY_READ_ONLY_LATE(unsigned long) gT0Sz = T0SZ_BOOT;
SECURITY_READ_ONLY_LATE(unsigned long) gT1Sz = T1SZ_BOOT;

vm_offset_t mem_size;
uint64_t    mem_actual;
uint64_t    max_mem;
uint64_t    max_mem_actual;
uint64_t    sane_size;
addr64_t    vm_last_addr = VM_MAX_KERNEL_ADDRESS;

SECURITY_READ_ONLY_LATE(vm_offset_t)   segEXTRADATA;
SECURITY_READ_ONLY_LATE(unsigned long) segSizeEXTRADATA;

SECURITY_READ_ONLY_LATE(vm_offset_t)   segLOWESTTEXT;
SECURITY_READ_ONLY_LATE(vm_offset_t)   segLOWEST;
SECURITY_READ_ONLY_LATE(vm_offset_t)   segLOWESTRO;
SECURITY_READ_ONLY_LATE(vm_offset_t)   segHIGHESTRO;

SECURITY_READ_ONLY_LATE(static vm_offset_t)   segTEXTB;
SECURITY_READ_ONLY_LATE(static unsigned long) segSizeTEXT;

SECURITY_READ_ONLY_LATE(static vm_offset_t)   segDATACONSTB;
SECURITY_READ_ONLY_LATE(static unsigned long) segSizeDATACONST;

SECURITY_READ_ONLY_LATE(vm_offset_t)   segTEXTEXECB;
SECURITY_READ_ONLY_LATE(unsigned long) segSizeTEXTEXEC;

SECURITY_READ_ONLY_LATE(static vm_offset_t)   segDATAB;
SECURITY_READ_ONLY_LATE(static unsigned long) segSizeDATA;

SECURITY_READ_ONLY_LATE(vm_offset_t)          segBOOTDATAB;
SECURITY_READ_ONLY_LATE(unsigned long)        segSizeBOOTDATA;
extern vm_offset_t                            intstack_low_guard;
extern vm_offset_t                            intstack_high_guard;
extern vm_offset_t                            excepstack_high_guard;

SECURITY_READ_ONLY_LATE(vm_offset_t)          segLINKB;
SECURITY_READ_ONLY_LATE(static unsigned long) segSizeLINK;

SECURITY_READ_ONLY_LATE(static vm_offset_t)   segKLDB;
SECURITY_READ_ONLY_LATE(unsigned long)        segSizeKLD;
SECURITY_READ_ONLY_LATE(static vm_offset_t)   segKLDDATAB;
SECURITY_READ_ONLY_LATE(static unsigned long) segSizeKLDDATA;
SECURITY_READ_ONLY_LATE(vm_offset_t)          segLASTB;
SECURITY_READ_ONLY_LATE(unsigned long)        segSizeLAST;
SECURITY_READ_ONLY_LATE(vm_offset_t)          segLASTDATACONSTB;
SECURITY_READ_ONLY_LATE(unsigned long)        segSizeLASTDATACONST;

SECURITY_READ_ONLY_LATE(vm_offset_t)          sectHIBTEXTB;
SECURITY_READ_ONLY_LATE(unsigned long)        sectSizeHIBTEXT;
SECURITY_READ_ONLY_LATE(vm_offset_t)          segHIBDATAB;
SECURITY_READ_ONLY_LATE(unsigned long)        segSizeHIBDATA;
SECURITY_READ_ONLY_LATE(vm_offset_t)          sectHIBDATACONSTB;
SECURITY_READ_ONLY_LATE(unsigned long)        sectSizeHIBDATACONST;

SECURITY_READ_ONLY_LATE(vm_offset_t)          segPRELINKTEXTB;
SECURITY_READ_ONLY_LATE(unsigned long)        segSizePRELINKTEXT;

SECURITY_READ_ONLY_LATE(static vm_offset_t)   segPLKTEXTEXECB;
SECURITY_READ_ONLY_LATE(static unsigned long) segSizePLKTEXTEXEC;

SECURITY_READ_ONLY_LATE(static vm_offset_t)   segPLKDATACONSTB;
SECURITY_READ_ONLY_LATE(static unsigned long) segSizePLKDATACONST;

SECURITY_READ_ONLY_LATE(static vm_offset_t)   segPRELINKDATAB;
SECURITY_READ_ONLY_LATE(static unsigned long) segSizePRELINKDATA;

SECURITY_READ_ONLY_LATE(static vm_offset_t)   segPLKLLVMCOVB = 0;
SECURITY_READ_ONLY_LATE(static unsigned long) segSizePLKLLVMCOV = 0;

SECURITY_READ_ONLY_LATE(static vm_offset_t)   segPLKLINKEDITB;
SECURITY_READ_ONLY_LATE(static unsigned long) segSizePLKLINKEDIT;

SECURITY_READ_ONLY_LATE(static vm_offset_t)   segPRELINKINFOB;
SECURITY_READ_ONLY_LATE(static unsigned long) segSizePRELINKINFO;

SECURITY_READ_ONLY_LATE(static boolean_t) use_contiguous_hint = TRUE;

SECURITY_READ_ONLY_LATE(vm_offset_t) end_kern;
SECURITY_READ_ONLY_LATE(vm_offset_t) etext;
SECURITY_READ_ONLY_LATE(vm_offset_t) sdata;
SECURITY_READ_ONLY_LATE(vm_offset_t) edata;

vm_offset_t alloc_ptpage(boolean_t map_static);
SECURITY_READ_ONLY_LATE(vm_offset_t) ropage_next;

SECURITY_READ_ONLY_LATE(vm_offset_t)     first_avail;
SECURITY_READ_ONLY_LATE(vm_offset_t)     static_memory_end;
SECURITY_READ_ONLY_LATE(pmap_paddr_t)    avail_start;
SECURITY_READ_ONLY_LATE(pmap_paddr_t)    avail_end;
SECURITY_READ_ONLY_LATE(pmap_paddr_t)    real_avail_end;
SECURITY_READ_ONLY_LATE(unsigned long)   real_phys_size;
SECURITY_READ_ONLY_LATE(vm_map_address_t) physmap_base = (vm_map_address_t)0;
SECURITY_READ_ONLY_LATE(vm_map_address_t) physmap_end = (vm_map_address_t)0;

typedef struct {
        pmap_paddr_t pa;
        vm_map_address_t va;
        vm_size_t len;
} ptov_table_entry;

#define PTOV_TABLE_SIZE 8
SECURITY_READ_ONLY_LATE(static ptov_table_entry) ptov_table[PTOV_TABLE_SIZE];

vm_map_address_t
phystokv(pmap_paddr_t pa)
{
        for (size_t i = 0; (i < PTOV_TABLE_SIZE) && (ptov_table[i].len != 0); i++) {
                if ((pa >= ptov_table[i].pa) && (pa < (ptov_table[i].pa + ptov_table[i].len))) {
                        return pa - ptov_table[i].pa + ptov_table[i].va;
                }
        }
        assertf((pa - gPhysBase) < real_phys_size, "%s: illegal PA: 0x%llx", __func__, (uint64_t)pa);
        return pa - gPhysBase + gVirtBase;
}

vm_map_address_t
phystokv_range(pmap_paddr_t pa, vm_size_t *max_len)
{
        vm_size_t len;
        for (size_t i = 0; (i < PTOV_TABLE_SIZE) && (ptov_table[i].len != 0); i++) {
                if ((pa >= ptov_table[i].pa) && (pa < (ptov_table[i].pa + ptov_table[i].len))) {
                        len = ptov_table[i].len - (pa - ptov_table[i].pa);
                        if (*max_len > len) {
                                *max_len = len;
                        }
                        return pa - ptov_table[i].pa + ptov_table[i].va;
                }
        }
        len = PAGE_SIZE - (pa & PAGE_MASK);
        if (*max_len > len) {
                *max_len = len;
        }
        assertf((pa - gPhysBase) < real_phys_size, "%s: illegal PA: 0x%llx", __func__, (uint64_t)pa);
        return pa - gPhysBase + gVirtBase;
}

vm_offset_t
ml_static_vtop(vm_offset_t va)
{
        for (size_t i = 0; (i < PTOV_TABLE_SIZE) && (ptov_table[i].len != 0); i++) {
                if ((va >= ptov_table[i].va) && (va < (ptov_table[i].va + ptov_table[i].len))) {
                        return va - ptov_table[i].va + ptov_table[i].pa;
                }
        }
        assertf(((vm_address_t)(va) - gVirtBase) < gPhysSize, "%s: illegal VA: %p", __func__, (void*)va);
        return (vm_address_t)(va) - gVirtBase + gPhysBase;
}

vm_offset_t
alloc_ptpage(boolean_t map_static __unused)
{
        vm_offset_t vaddr = phystokv(avail_start);
        avail_start += ARM_PGBYTES;
        return vaddr;
}

tt_entry_t *
arm_kva_to_tte(vm_offset_t va)
{
        tt_entry_t *tte1, *tte2;
        tte1 = cpu_tte + L1_TABLE_INDEX(va);
        tte2 = L2_TABLE_VA(tte1) + L2_TABLE_INDEX(va);
        return tte2;
}

#define ARM64_GRANULE_ALLOW_BLOCK (1 << 0)
#define ARM64_GRANULE_ALLOW_HINT  (1 << 1)

static void
arm_vm_page_granular_helper(vm_offset_t start, vm_offset_t _end, vm_offset_t va, pmap_paddr_t pa_offset,
    int pte_prot_APX, int pte_prot_XN, unsigned granule,
    pt_entry_t **deferred_pte, pt_entry_t *deferred_ptmp)
{
        if (va & ARM_TT_L2_OFFMASK) {
                tt_entry_t *tte2;
                tt_entry_t tmplate;
                pmap_paddr_t pa;
                pt_entry_t *ppte, ptmp;
                addr64_t ppte_phys;
                unsigned i;

                va &= ~ARM_TT_L2_OFFMASK;
                pa = va - gVirtBase + gPhysBase - pa_offset;

                if (pa >= real_avail_end) return;

                tte2 = arm_kva_to_tte(va);
                tmplate = *tte2;

                if (ARM_TTE_TYPE_TABLE == (tmplate & ARM_TTE_TYPE_MASK)) {
                        ppte = (pt_entry_t *)phystokv((tmplate & ARM_TTE_TABLE_MASK));
                } else {
                        ppte = (pt_entry_t*)alloc_ptpage(FALSE);
                        bzero(ppte, ARM_PGBYTES);
                        ppte_phys = kvtophys((vm_offset_t)ppte);
                        *tte2 = pa_to_tte(ppte_phys) | ARM_TTE_TYPE_TABLE | ARM_TTE_VALID;
                }

                vm_offset_t len = _end - va;
                if ((pa + len) > real_avail_end) {
                        _end -= (pa + len - real_avail_end);
                }

                vm_offset_t rounded_end = round_page(_end);
                for (i = 0; i <= (ARM_TT_L3_INDEX_MASK >> ARM_TT_L3_SHIFT); i++) {
                        if ((start <= va) && (va < rounded_end)) {
                                ptmp = pa | ARM_PTE_AF | ARM_PTE_SH(SH_OUTER_MEMORY) | ARM_PTE_TYPE;
                                ptmp = ptmp | ARM_PTE_ATTRINDX(CACHE_ATTRINDX_DEFAULT);
                                ptmp = ptmp | ARM_PTE_AP(pte_prot_APX);
                                ptmp = ptmp | ARM_PTE_NX;

                                if (pte_prot_XN) {
                                        ptmp = ptmp | ARM_PTE_PNX;
                                }

                                ppte[i] = ptmp;
                        }
                        va += ARM_PGBYTES;
                        pa += ARM_PGBYTES;
                }
        }
}

static void
arm_vm_page_granular_prot(vm_offset_t start, unsigned long size, pmap_paddr_t pa_offset,
    int tte_prot_XN, int pte_prot_APX, int pte_prot_XN,
    unsigned granule)
{
        vm_offset_t _end = start + size;
        vm_offset_t align_start = (start + ARM_TT_L2_OFFMASK) & ~ARM_TT_L2_OFFMASK;

        if (size == 0x0UL) return;

        if (align_start > _end) {
                arm_vm_page_granular_helper(start, _end, start, pa_offset, pte_prot_APX, pte_prot_XN, granule, NULL, NULL);
                return;
        }

        arm_vm_page_granular_helper(start, align_start, start, pa_offset, pte_prot_APX, pte_prot_XN, granule, NULL, NULL);

        while ((_end - align_start) >= ARM_TT_L2_SIZE) {
                if (!(granule & ARM64_GRANULE_ALLOW_BLOCK)) {
                        arm_vm_page_granular_helper(align_start, align_start + ARM_TT_L2_SIZE, align_start + 1, pa_offset,
                            pte_prot_APX, pte_prot_XN, granule, NULL, NULL);
                } else {
                        pmap_paddr_t pa = align_start - gVirtBase + gPhysBase - pa_offset;
                        tt_entry_t *tte2 = arm_kva_to_tte(align_start);

                        if ((pa >= gPhysBase) && (pa < real_avail_end)) {
                                tt_entry_t tmplate = (pa & ARM_TTE_BLOCK_L2_MASK) | ARM_TTE_TYPE_BLOCK
                                    | ARM_TTE_VALID | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_NX
                                    | ARM_TTE_BLOCK_AP(pte_prot_APX) | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY)
                                    | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITEBACK);

                                if (tte_prot_XN) {
                                        tmplate = tmplate | ARM_TTE_BLOCK_PNX;
                                }
                                *tte2 = tmplate;
                        }
                }
                align_start += ARM_TT_L2_SIZE;
        }

        if (align_start < _end) {
                arm_vm_page_granular_helper(align_start, _end, _end, pa_offset, pte_prot_APX, pte_prot_XN, granule, NULL, NULL);
        }
}

static inline void
arm_vm_page_granular_RNX(vm_offset_t start, unsigned long size, unsigned granule)
{
        arm_vm_page_granular_prot(start, size, 0, 1, AP_RONA, 1, granule);
}

static inline void
arm_vm_page_granular_ROX(vm_offset_t start, unsigned long size, unsigned granule)
{
        arm_vm_page_granular_prot(start, size, 0, 0, AP_RONA, 0, granule);
}

static inline void
arm_vm_page_granular_RWNX(vm_offset_t start, unsigned long size, unsigned granule)
{
        arm_vm_page_granular_prot(start, size, 0, 1, AP_RWNA, 1, granule);
}

void
arm_vm_prot_init(__unused boot_args * args)
{
        segLOWESTTEXT = UINT64_MAX;
        if (segSizePRELINKTEXT && (segPRELINKTEXTB < segLOWESTTEXT)) segLOWESTTEXT = segPRELINKTEXTB;
        if (segTEXTB < segLOWESTTEXT) segLOWESTTEXT = segTEXTB;

        segEXTRADATA = segLOWESTTEXT;
        segSizeEXTRADATA = 0;
        segLOWEST = segLOWESTTEXT;
        segLOWESTRO = segLOWESTTEXT;

        if (SecureDTIsLockedDown()) {
                segEXTRADATA = (vm_offset_t)PE_state.deviceTreeHead;
                segSizeEXTRADATA = PE_state.deviceTreeSize;
                arm_vm_page_granular_RNX(segEXTRADATA, segSizeEXTRADATA, ARM64_GRANULE_ALLOW_BLOCK);
        }

        arm_vm_page_granular_RWNX(segPRELINKTEXTB, segSizePRELINKTEXT, ARM64_GRANULE_ALLOW_BLOCK);
        arm_vm_page_granular_RWNX(segPLKDATACONSTB, segSizePLKDATACONST, ARM64_GRANULE_ALLOW_BLOCK);
        arm_vm_page_granular_ROX(segPLKTEXTEXECB, segSizePLKTEXTEXEC, ARM64_GRANULE_ALLOW_BLOCK);

        arm_vm_page_granular_RNX(segTEXTB, segSizeTEXT, ARM64_GRANULE_ALLOW_BLOCK);
        arm_vm_page_granular_RWNX(segDATACONSTB, segSizeDATACONST, ARM64_GRANULE_ALLOW_BLOCK);
        arm_vm_page_granular_ROX(segTEXTEXECB, segSizeTEXTEXEC, ARM64_GRANULE_ALLOW_BLOCK);

        arm_vm_page_granular_RWNX(segDATAB, segSizeDATA, ARM64_GRANULE_ALLOW_BLOCK);
        arm_vm_page_granular_RWNX(segHIBDATAB, segSizeHIBDATA, ARM64_GRANULE_ALLOW_BLOCK);

        arm_vm_page_granular_RWNX(segBOOTDATAB, segSizeBOOTDATA, 0);
        arm_vm_page_granular_RNX((vm_offset_t)&intstack_low_guard, PAGE_MAX_SIZE, 0);
        arm_vm_page_granular_RNX((vm_offset_t)&intstack_high_guard, PAGE_MAX_SIZE, 0);
        arm_vm_page_granular_RNX((vm_offset_t)&excepstack_high_guard, PAGE_MAX_SIZE, 0);

        arm_vm_page_granular_ROX(segKLDB, segSizeKLD, ARM64_GRANULE_ALLOW_BLOCK);
        arm_vm_page_granular_RNX(segKLDDATAB, segSizeKLDDATA, ARM64_GRANULE_ALLOW_BLOCK);
        arm_vm_page_granular_RWNX(segLINKB, segSizeLINK, ARM64_GRANULE_ALLOW_BLOCK);
        arm_vm_page_granular_RWNX(segPLKLINKEDITB, segSizePLKLINKEDIT, ARM64_GRANULE_ALLOW_BLOCK);
        arm_vm_page_granular_ROX(segLASTB, segSizeLAST, ARM64_GRANULE_ALLOW_BLOCK);
        if (segLASTDATACONSTB) {
                arm_vm_page_granular_RWNX(segLASTDATACONSTB, segSizeLASTDATACONST, ARM64_GRANULE_ALLOW_BLOCK);
        }
        arm_vm_page_granular_RWNX(segPRELINKDATAB, segSizePRELINKDATA, ARM64_GRANULE_ALLOW_BLOCK);
        arm_vm_page_granular_RWNX(segPRELINKINFOB, segSizePRELINKINFO, ARM64_GRANULE_ALLOW_BLOCK);

        vm_kernelcache_base = segLOWEST;
        vm_kernelcache_top = end_kern;
}

typedef int (*cmpfunc_t)(const void *a, const void *b);
extern void qsort(void *a, size_t n, size_t es, cmpfunc_t cmp);

static int
cmp_ptov_entries(const void *a, const void *b)
{
        const ptov_table_entry *entry_a = a;
        const ptov_table_entry *entry_b = b;
        if (entry_a->len < entry_b->len) return 1;
        if (entry_a->len > entry_b->len) return -1;
        return 0;
}

SECURITY_READ_ONLY_LATE(static unsigned int) ptov_index = 0;

static void
arm_vm_physmap_slide(ptov_table_entry *temp_ptov_table, vm_map_address_t orig_va, vm_size_t len, int pte_prot_APX, unsigned granule)
{
        pmap_paddr_t pa_offset;

        assert(ptov_index < PTOV_TABLE_SIZE);
        temp_ptov_table[ptov_index].pa = orig_va - gVirtBase + gPhysBase;
        if (ptov_index == 0) {
                temp_ptov_table[ptov_index].va = physmap_base;
        } else {
                temp_ptov_table[ptov_index].va = temp_ptov_table[ptov_index - 1].va + temp_ptov_table[ptov_index - 1].len;
        }

        temp_ptov_table[ptov_index].len = round_page(len);
        pa_offset = temp_ptov_table[ptov_index].va - orig_va;
        arm_vm_page_granular_prot(temp_ptov_table[ptov_index].va, temp_ptov_table[ptov_index].len, pa_offset, 1, pte_prot_APX, 1, granule);
        ++ptov_index;
}

static void
arm_vm_physmap_init(boot_args *args)
{
        ptov_table_entry temp_ptov_table[PTOV_TABLE_SIZE];
        bzero(temp_ptov_table, sizeof(temp_ptov_table));

        arm_vm_physmap_slide(temp_ptov_table, gVirtBase, segLOWEST - gVirtBase, AP_RWNA,
            ARM64_GRANULE_ALLOW_BLOCK | ARM64_GRANULE_ALLOW_HINT);

        arm_vm_page_granular_RWNX(end_kern, phystokv(args->topOfKernelData) - end_kern,
            ARM64_GRANULE_ALLOW_BLOCK | ARM64_GRANULE_ALLOW_HINT);

        arm_vm_physmap_slide(temp_ptov_table, (args->topOfKernelData - gPhysBase + gVirtBase),
            real_avail_end - args->topOfKernelData, AP_RWNA, ARM64_GRANULE_ALLOW_BLOCK | ARM64_GRANULE_ALLOW_HINT);

        qsort(temp_ptov_table, PTOV_TABLE_SIZE, sizeof(temp_ptov_table[0]), cmp_ptov_entries);
        memcpy(ptov_table, temp_ptov_table, sizeof(ptov_table));
}

void
arm_vm_prot_finalize(boot_args * args __unused)
{
        ml_static_mfree(phystokv(gPhysBase), segLOWEST - gVirtBase);

        if (!segSizePLKDATACONST && !segSizePLKTEXTEXEC && segSizePRELINKTEXT) {
                ml_static_mfree(segPRELINKTEXTB + segSizePRELINKTEXT, segTEXTB - (segPRELINKTEXTB + segSizePRELINKTEXT));
        }

        arm_vm_page_granular_RNX(segPRELINKTEXTB, segSizePRELINKTEXT, ARM64_GRANULE_ALLOW_BLOCK);
        arm_vm_page_granular_RNX(segPLKDATACONSTB, segSizePLKDATACONST, ARM64_GRANULE_ALLOW_BLOCK);

        cpu_stack_alloc(&BootCpuData);
        arm64_replace_bootstack(&BootCpuData);
        ml_static_mfree(phystokv(segBOOTDATAB - gVirtBase + gPhysBase), segSizeBOOTDATA);

        arm_vm_page_granular_RNX(segDATACONSTB, segSizeDATACONST, ARM64_GRANULE_ALLOW_BLOCK);

        __builtin_arm_dsb(DSB_ISH);
        flush_mmu_tlb();
}

static void
set_tbi(void)
{
        uint64_t old_tcr, new_tcr;
        old_tcr = new_tcr = get_tcr();
        new_tcr |= TCR_TBI0_TOPBYTE_IGNORED;

        if (old_tcr != new_tcr) {
                set_tcr(new_tcr);
                sysreg_restore.tcr_el1 = new_tcr;
        }
}

static void
init_ptpages(tt_entry_t *tt, vm_map_address_t start, vm_map_address_t end, bool static_map, uint64_t table_attrs)
{
        tt_entry_t *l1_tte;
        vm_offset_t ptpage_vaddr;

        l1_tte = tt + ((start & ARM_TT_L1_INDEX_MASK) >> ARM_TT_L1_SHIFT);

        while (start < end) {
                if (*l1_tte == ARM_TTE_EMPTY) {
                        ptpage_vaddr = alloc_ptpage(static_map);
                        *l1_tte = (kvtophys(ptpage_vaddr) & ARM_TTE_TABLE_MASK) | ARM_TTE_TYPE_TABLE | ARM_TTE_VALID | table_attrs;
                        bzero((void *)ptpage_vaddr, ARM_PGBYTES);
                }

                if ((start + ARM_TT_L1_SIZE) < start) break;

                start += ARM_TT_L1_SIZE;
                l1_tte++;
        }
}

void
arm_vm_init(uint64_t memory_size, boot_args * args)
{
        vm_map_address_t va_l1, va_l1_end;
        tt_entry_t       *cpu_l1_tte;
        vm_map_address_t va_l2, va_l2_end;
        tt_entry_t       *cpu_l2_tte;
        vm_map_address_t dynamic_memory_begin;

        gVirtBase = args->virtBase;
        gPhysBase = args->physBase;
        real_phys_size = args->memSize;
        gPhysSize = mem_size = ((gPhysBase + args->memSize) & ~PAGE_MASK) - gPhysBase;
        mem_actual = args->memSizeActual ? args->memSizeActual : mem_size;

        if ((memory_size != 0) && (mem_size > memory_size)) {
                mem_size = memory_size;
                max_mem_actual = memory_size;
        } else {
                max_mem_actual = mem_actual;
        }

        physmap_base = phystokv(args->topOfKernelData);
        static_memory_end = physmap_base + mem_size;
        physmap_end = physmap_base + real_phys_size;
        dynamic_memory_begin = static_memory_end;

        avail_start = args->topOfKernelData;

        invalid_tte = (tt_entry_t *)alloc_ptpage(TRUE);
        invalid_ttep = kvtophys((vm_offset_t)invalid_tte);
        bzero(invalid_tte, ARM_PGBYTES);

        cpu_tte = (tt_entry_t *)alloc_ptpage(TRUE);
        cpu_ttep = kvtophys((vm_offset_t)cpu_tte);
        bzero(cpu_tte, ARM_PGBYTES);
        avail_end = gPhysBase + mem_size;
        real_avail_end = avail_end;

        init_ptpages(cpu_tte, gVirtBase, dynamic_memory_begin, TRUE, 0);

        segPRELINKTEXTB  = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__PRELINK_TEXT", &segSizePRELINKTEXT);
        segPLKDATACONSTB = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__PLK_DATA_CONST", &segSizePLKDATACONST);
        segPLKTEXTEXECB  = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__PLK_TEXT_EXEC", &segSizePLKTEXTEXEC);
        segTEXTB         = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__TEXT", &segSizeTEXT);
        segDATACONSTB    = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__DATA_CONST", &segSizeDATACONST);
        segTEXTEXECB     = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__TEXT_EXEC", &segSizeTEXTEXEC);
        segDATAB         = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__DATA", &segSizeDATA);
        segBOOTDATAB     = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__BOOTDATA", &segSizeBOOTDATA);
        segLINKB         = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__LINKEDIT", &segSizeLINK);
        segKLDB          = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__KLD", &segSizeKLD);
        segKLDDATAB      = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__KLDDATA", &segSizeKLDDATA);
        segPRELINKDATAB  = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__PRELINK_DATA", &segSizePRELINKDATA);
        segPRELINKINFOB  = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__PRELINK_INFO", &segSizePRELINKINFO);
        segLASTB         = (vm_offset_t) getsegdatafromheader(&_mh_execute_header, "__LAST", &segSizeLAST);

        etext = (vm_offset_t) segTEXTB + segSizeTEXT;
        sdata = (vm_offset_t) segDATAB;
        edata = (vm_offset_t) segDATAB + segSizeDATA;
        end_kern = round_page(getlastaddr());

        vm_set_page_size();

        vm_kernel_base = segTEXTB;
        vm_kernel_top = (vm_offset_t) &last_kernel_symbol;
        vm_kext_base = segPRELINKTEXTB;
        vm_kext_top = vm_kext_base + segSizePRELINKTEXT;

        arm_vm_prot_init(args);

        set_tbi();

        arm_vm_physmap_init(args);
        set_mmu_ttb_alternate(cpu_ttep & TTBR_BADDR_MASK);

        set_mmu_ttb(invalid_ttep & TTBR_BADDR_MASK);
        flush_mmu_tlb();

        kva_active = TRUE;
        cpu_tte = (tt_entry_t*)(phystokv(cpu_ttep));
        invalid_tte = (tt_entry_t*)(phystokv(invalid_ttep));

        sane_size = mem_size - (avail_start - gPhysBase);
        max_mem = mem_size;
        vm_kernel_slid_base = segLOWESTTEXT;
        vm_kernel_slid_top = vm_prelink_einfo;
        vm_kernel_stext = segTEXTB;
        vm_kernel_etext = segTEXTB + segSizeTEXT + segSizeDATACONST + segSizeTEXTEXEC;

        pmap_bootstrap(dynamic_memory_begin);

        disable_preemption();

        avail_start = (avail_start + PAGE_MASK) & ~PAGE_MASK;
        first_avail = avail_start;
        patch_low_glo_static_region(args->topOfKernelData, avail_start - args->topOfKernelData);
        enable_preemption();
}
