#include <arm64/proc_reg.h>
#include <arm/machine_cpu.h>
#include <arm/cpu_internal.h>
#include <arm/cpuid.h>
#include <arm/io_map_entries.h>
#include <arm/cpu_data.h>
#include <arm/cpu_data_internal.h>
#include <arm/caches_internal.h>
#include <arm/misc_protos.h>
#include <arm/machdep_call.h>
#include <arm/machine_routines.h>
#include <arm/rtclock.h>
#include <arm/cpuid_internal.h>
#include <arm/cpu_capabilities.h>
#include <console/serial_protos.h>
#include <kern/machine.h>
#include <kern/misc_protos.h>
#include <prng/random.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <kern/timer_queue.h>
#include <mach/machine.h>
#include <machine/atomic.h>
#include <machine/config.h>
#include <vm/pmap.h>
#include <vm/vm_page.h>
#include <vm/vm_shared_region.h>
#include <vm/vm_map.h>
#include <sys/codesign.h>
#include <sys/kdebug.h>
#include <kern/coalition.h>
#include <pexpert/device_tree.h>

#include <IOKit/IOPlatformExpert.h>
#if HIBERNATION
#include <IOKit/IOHibernatePrivate.h>
#endif

#include <libkern/section_keywords.h>

#if KPC
#include <kern/kpc.h>
#endif

#define MPIDR_CPU_ID(mpidr_el1_val)             (((mpidr_el1_val) & MPIDR_AFF0_MASK) >> MPIDR_AFF0_SHIFT)
#define MPIDR_CLUSTER_ID(mpidr_el1_val)         (((mpidr_el1_val) & MPIDR_AFF1_MASK) >> MPIDR_AFF1_SHIFT)

#if HAS_CLUSTER
static uint8_t cluster_initialized = 0;
#endif

uint32_t LockTimeOut;
uint32_t LockTimeOutUsec;
uint64_t TLockTimeOut;
uint64_t MutexSpin;
uint64_t low_MutexSpin;
int64_t high_MutexSpin;

static uint64_t ml_wfe_hint_max_interval;
#define MAX_WFE_HINT_INTERVAL_US (500ULL)

TUNABLE(uint32_t, yield_delay_us, "yield_delay_us", 0);

extern vm_offset_t   segLOWEST;
extern vm_offset_t   segLOWESTTEXT;
extern vm_offset_t   segLASTB;
extern unsigned long segSizeLAST;

extern vm_offset_t   vm_kernelcache_base;
extern vm_offset_t   vm_kernelcache_top;

thread_t Idle_context(void);

SECURITY_READ_ONLY_LATE(static ml_topology_cpu_t) topology_cpu_array[MAX_CPUS];
SECURITY_READ_ONLY_LATE(static ml_topology_cluster_t) topology_cluster_array[MAX_CPU_CLUSTERS];
SECURITY_READ_ONLY_LATE(static ml_topology_info_t) topology_info = {
        .version = CPU_TOPOLOGY_VERSION,
        .cpus = topology_cpu_array,
        .clusters = topology_cluster_array,
};

SECURITY_READ_ONLY_LATE(int64_t) cluster_offsets[MAX_CPU_CLUSTER_PHY_ID + 1];
SECURITY_READ_ONLY_LATE(static uint32_t) arm64_eventi = UINT32_MAX;

extern uint32_t lockdown_done;

SECURITY_READ_ONLY_LATE(static struct vm_reserved_region) vm_reserved_regions[] = {
        {
                .vmrr_name = "GPU Carveout",
                .vmrr_addr = MACH_VM_MIN_GPU_CARVEOUT_ADDRESS,
                .vmrr_size = (vm_map_size_t)(MACH_VM_MAX_GPU_CARVEOUT_ADDRESS - MACH_VM_MIN_GPU_CARVEOUT_ADDRESS)
        },
        {
                .vmrr_name = "commpage nesting",
                .vmrr_addr = _COMM_PAGE64_NESTING_START,
                .vmrr_size = _COMM_PAGE64_NESTING_SIZE
        }
};

uint32_t get_arm_cpu_version(void);

/* IPI Заглушки под стандартный ARM64 (без ACC Fast IPI) */
void
ml_cpu_signal(unsigned int cpu_mpidr __unused)
{
        /* TODO: Подвязать к стандартному ARM GICv2/v3 IPI при расширении SMP */
}

void
ml_cpu_signal_deferred_adjust_timer(uint64_t nanosecs __unused)
{
}

uint64_t
ml_cpu_signal_deferred_get_timer(void)
{
        return 0;
}

void
ml_cpu_signal_deferred(unsigned int cpu_mpidr __unused)
{
}

void
ml_cpu_signal_retract(unsigned int cpu_mpidr __unused)
{
}

void
machine_idle(void)
{
        assert((__builtin_arm_rsr("DAIF") & DAIF_IRQF) == DAIF_IRQF);
        Idle_context();
        __builtin_arm_wsr("DAIFClr", (DAIFSC_IRQF | DAIFSC_FIQF));
}

void
OSSynchronizeIO(void)
{
        __builtin_arm_dsb(DSB_SY);
}

uint64_t
get_aux_control(void)
{
        uint64_t value;
        MRS(value, "ACTLR_EL1");
        return value;
}

uint64_t
get_mmu_control(void)
{
        uint64_t value;
        MRS(value, "SCTLR_EL1");
        return value;
}

uint64_t
get_tcr(void)
{
        uint64_t value;
        MRS(value, "TCR_EL1");
        return value;
}

boolean_t
ml_get_interrupts_enabled(void)
{
        uint64_t value;
        MRS(value, "DAIF");
        return (value & DAIF_IRQF) ? FALSE : TRUE;
}

pmap_paddr_t
get_mmu_ttb(void)
{
        pmap_paddr_t value;
        MRS(value, "TTBR0_EL1");
        return value;
}

uint32_t
get_arm_cpu_version(void)
{
        uint32_t value = machine_read_midr();
        return ((value & MIDR_EL1_REV_MASK) >> MIDR_EL1_REV_SHIFT) | ((value & MIDR_EL1_VAR_MASK) >> (MIDR_EL1_VAR_SHIFT - 4));
}

bool
ml_feature_supported(uint32_t feature_bit)
{
        uint64_t aidr_el1_value = 0;
        MRS(aidr_el1_value, "AIDR_EL1");
        return (aidr_el1_value & feature_bit) != 0;
}

boolean_t
user_cont_hwclock_allowed(void)
{
        return FALSE;
}

uint8_t
user_timebase_type(void)
{
        return USER_TIMEBASE_SPEC;
}

void
machine_startup(__unused boot_args * args)
{
        machine_conf();
        kernel_bootstrap();
}

typedef void (*invalidate_fn_t)(void);
static SECURITY_READ_ONLY_LATE(invalidate_fn_t) invalidate_hmac_function = NULL;

void
set_invalidate_hmac_function(invalidate_fn_t fn)
{
        if (invalidate_hmac_function != NULL) {
                panic("Invalidate HMAC function already set");
        }
        invalidate_hmac_function = fn;
}

void
machine_lockdown(void)
{
        arm_vm_prot_finalize(PE_state.bootArgs);

        if (invalidate_hmac_function != NULL) {
                invalidate_hmac_function();
        }

        lockdown_done = 1;
}

char *
machine_boot_info(__unused char *buf, __unused vm_size_t size)
{
        return PE_boot_args();
}

void
slave_machine_init(__unused void *param)
{
        cpu_machine_init();
        clock_init();
}

thread_t
machine_processor_shutdown(__unused thread_t thread, void (*doshutdown)(processor_t), processor_t processor)
{
        return Shutdown_context(doshutdown, processor);
}

void
ml_init_lock_timeout(void)
{
        uint64_t abstime;
        uint64_t mtxspin;
        uint64_t default_timeout_ns = NSEC_PER_SEC >> 2;
        uint32_t slto;

        if (PE_parse_boot_argn("slto_us", &slto, sizeof(slto))) {
                default_timeout_ns = slto * NSEC_PER_USEC;
        }

        nanoseconds_to_absolutetime(default_timeout_ns, &abstime);
        LockTimeOutUsec = (uint32_t)(default_timeout_ns / NSEC_PER_USEC);
        LockTimeOut = (uint32_t)abstime;

        if (PE_parse_boot_argn("tlto_us", &slto, sizeof(slto))) {
                nanoseconds_to_absolutetime(slto * NSEC_PER_USEC, &abstime);
                TLockTimeOut = abstime;
        } else {
                TLockTimeOut = LockTimeOut >> 1;
        }

        if (PE_parse_boot_argn("mtxspin", &mtxspin, sizeof(mtxspin))) {
                if (mtxspin > USEC_PER_SEC >> 4) {
                        mtxspin = USEC_PER_SEC >> 4;
                }
                nanoseconds_to_absolutetime(mtxspin * NSEC_PER_USEC, &abstime);
        } else {
                nanoseconds_to_absolutetime(10 * NSEC_PER_USEC, &abstime);
        }
        MutexSpin = abstime;
        low_MutexSpin = MutexSpin;
        high_MutexSpin = low_MutexSpin;

        nanoseconds_to_absolutetime(MAX_WFE_HINT_INTERVAL_US * NSEC_PER_USEC, &ml_wfe_hint_max_interval);
}

void ml_cpu_init_completed(void) {}

void
ml_cpu_up(void)
{
        os_atomic_inc(&machine_info.physical_cpu, relaxed);
        os_atomic_inc(&machine_info.logical_cpu, relaxed);
}

void
ml_cpu_down(void)
{
        cpu_data_t *cpu_data_ptr;

        os_atomic_dec(&machine_info.physical_cpu, relaxed);
        os_atomic_dec(&machine_info.logical_cpu, relaxed);

        cpu_data_ptr = getCpuDatap();
        cpu_data_ptr->cpu_running = FALSE;

        if (cpu_data_ptr != &BootCpuData) {
                timer_queue_shutdown(&cpu_data_ptr->rtclock_timer.queue);
                cpu_xcall(BootCpuData.cpu_number, &timer_queue_expire_local, NULL);
        }

        cpu_signal_handler_internal(TRUE);
}

void
ml_cpu_get_info(ml_cpu_info_t * ml_cpu_info)
{
        cache_info_t *cpuid_cache_info = cache_info();
        ml_cpu_info->vector_unit = 0;
        ml_cpu_info->cache_line_size = cpuid_cache_info->c_linesz;
        ml_cpu_info->l1_icache_size = cpuid_cache_info->c_isize;
        ml_cpu_info->l1_dcache_size = cpuid_cache_info->c_dsize;

        ml_cpu_info->l2_settings = 1;
        ml_cpu_info->l2_cache_size = cpuid_cache_info->c_l2size;
        ml_cpu_info->l3_settings = 0;
        ml_cpu_info->l3_cache_size = 0xFFFFFFFF;
}

unsigned int
ml_get_machine_mem(void)
{
        return machine_info.memory_size;
}

__attribute__((noreturn))
void
halt_all_cpus(boolean_t reboot)
{
        if (reboot) {
                printf("MACH Reboot\n");
                PEHaltRestart(kPERestartCPU);
        } else {
                printf("CPU halted\n");
                PEHaltRestart(kPEHaltCPU);
        }
        while (1) ;
}

__attribute__((noreturn))
void
halt_cpu(void)
{
        halt_all_cpus(FALSE);
}

void
machine_signal_idle(processor_t processor)
{
        cpu_signal(processor_to_cpu_datap(processor), SIGPnop, (void *)NULL, (void *)NULL);
        KERNEL_DEBUG_CONSTANT(MACHDBG_CODE(DBG_MACH_SCHED, MACH_REMOTE_AST), processor->cpu_id, 0, 0, 0, 0);
}

void
machine_signal_idle_deferred(processor_t processor)
{
        cpu_signal_deferred(processor_to_cpu_datap(processor));
        KERNEL_DEBUG_CONSTANT(MACHDBG_CODE(DBG_MACH_SCHED, MACH_REMOTE_DEFERRED_AST), processor->cpu_id, 0, 0, 0, 0);
}

void
machine_signal_idle_cancel(processor_t processor)
{
        cpu_signal_cancel(processor_to_cpu_datap(processor));
        KERNEL_DEBUG_CONSTANT(MACHDBG_CODE(DBG_MACH_SCHED, MACH_REMOTE_CANCEL_AST), processor->cpu_id, 0, 0, 0, 0);
}

void
ml_install_interrupt_handler(void *nub, int source, void *target, IOInterruptHandler handler, void *refCon)
{
        cpu_data_t *cpu_data_ptr;
        boolean_t   current_state;

        current_state = ml_set_interrupts_enabled(FALSE);
        cpu_data_ptr = getCpuDatap();

        cpu_data_ptr->interrupt_nub = nub;
        cpu_data_ptr->interrupt_source = source;
        cpu_data_ptr->interrupt_target = target;
        cpu_data_ptr->interrupt_handler = handler;
        cpu_data_ptr->interrupt_refCon = refCon;

        (void) ml_set_interrupts_enabled(current_state);
}

void ml_init_interrupt(void) {}

void
ml_init_timebase(void *args, tbd_ops_t tbd_funcs, vm_offset_t int_address, vm_offset_t int_value __unused)
{
        cpu_data_t *cpu_data_ptr = (cpu_data_t *)args;

        if ((cpu_data_ptr == &BootCpuData) && (rtclock_timebase_func.tbd_fiq_handler == (void *)NULL)) {
                rtclock_timebase_func = *tbd_funcs;
                rtclock_timebase_addr = int_address;
        }
}

#define ML_READPROP_MANDATORY UINT64_MAX

static uint64_t
ml_readprop(const DTEntry entry, const char *propertyName, uint64_t default_value)
{
        void const *prop;
        unsigned int propSize;

        if (SecureDTGetProperty(entry, propertyName, &prop, &propSize) == kSuccess) {
                if (propSize == sizeof(uint8_t)) return *((uint8_t const *)prop);
                if (propSize == sizeof(uint16_t)) return *((uint16_t const *)prop);
                if (propSize == sizeof(uint32_t)) return *((uint32_t const *)prop);
                if (propSize == sizeof(uint64_t)) return *((uint64_t const *)prop);
        }
        return default_value;
}

static boolean_t
ml_read_reg_range(const DTEntry entry, const char *propertyName, uint64_t *pa_ptr, uint64_t *len_ptr)
{
        uint64_t const *prop;
        unsigned int propSize;

        if (SecureDTGetProperty(entry, propertyName, (void const **)&prop, &propSize) != kSuccess) {
                *pa_ptr = 0;
                *len_ptr = 0;
                return FALSE;
        }

        if (propSize != sizeof(uint64_t) * 2) {
                *pa_ptr = 0;
                *len_ptr = 0;
                return FALSE;
        }

        *pa_ptr = prop[0];
        *len_ptr = prop[1];
        return TRUE;
}

static boolean_t
ml_is_boot_cpu(const DTEntry entry)
{
        void const *prop;
        unsigned int propSize;

        if (SecureDTGetProperty(entry, "state", &prop, &propSize) != kSuccess) {
                return TRUE;
        }
        return (strncmp((char const *)prop, "running", propSize) == 0);
}

void
ml_parse_cpu_topology(void)
{
        DTEntry entry, child;
        OpaqueDTEntryIterator iter;
        uint32_t cpu_boot_arg = MAX_CPUS;
        int err;

        int64_t cluster_phys_to_logical[MAX_CPU_CLUSTER_PHY_ID + 1];
        int64_t cluster_max_cpu_phys_id[MAX_CPU_CLUSTER_PHY_ID + 1];
        PE_parse_boot_argn("cpus", &cpu_boot_arg, sizeof(cpu_boot_arg));

        err = SecureDTLookupEntry(NULL, "/cpus", &entry);
        if (err != kSuccess) {
                topology_info.num_cpus = 1;
                topology_info.num_clusters = 1;
                topology_info.cpus[0].cpu_id = 0;
                topology_info.cpus[0].phys_id = 0;
                topology_info.boot_cpu = &topology_info.cpus[0];
                topology_info.boot_cluster = &topology_info.clusters[0];
                return;
        }

        SecureDTInitEntryIterator(entry, &iter);

        for (int i = 0; i <= MAX_CPU_CLUSTER_PHY_ID; i++) {
                cluster_offsets[i] = -1;
                cluster_phys_to_logical[i] = -1;
                cluster_max_cpu_phys_id[i] = 0;
        }

        while (kSuccess == SecureDTIterateEntries(&iter, &child)) {
                boolean_t is_boot_cpu = ml_is_boot_cpu(child);

                if (topology_info.num_cpus >= (cpu_boot_arg - 1) && topology_info.boot_cpu == NULL && !is_boot_cpu) {
                        continue;
                }
                if (topology_info.num_cpus >= cpu_boot_arg) break;

                ml_topology_cpu_t *cpu = &topology_info.cpus[topology_info.num_cpus];

                cpu->cpu_id = topology_info.num_cpus++;
                topology_info.max_cpu_id = MAX(topology_info.max_cpu_id, cpu->cpu_id);

                cpu->die_id = (int)ml_readprop(child, "die-id", 0);
                topology_info.max_die_id = MAX(topology_info.max_die_id, cpu->die_id);
                cpu->phys_id = (uint32_t)ml_readprop(child, "reg", cpu->cpu_id);

                ml_read_reg_range(child, "cpu-uttdbg-reg", &cpu->cpu_UTTDBG_pa, &cpu->cpu_UTTDBG_len);
                ml_read_reg_range(child, "cpu-impl-reg", &cpu->cpu_IMPL_pa, &cpu->cpu_IMPL_len);
                ml_read_reg_range(child, "coresight-reg", &cpu->coresight_pa, &cpu->coresight_len);
                cpu->cluster_type = CLUSTER_TYPE_SMP;

                uint32_t phys_cluster_id = MPIDR_CLUSTER_ID(cpu->phys_id);
                if (phys_cluster_id > MAX_CPU_CLUSTER_PHY_ID) phys_cluster_id = 0;

                cpu->cluster_id = ((cluster_phys_to_logical[phys_cluster_id] == -1) ?
                    topology_info.num_clusters : cluster_phys_to_logical[phys_cluster_id]);

                ml_topology_cluster_t *cluster = &topology_info.clusters[cpu->cluster_id];
                if (cluster->num_cpus == 0) {
                        topology_info.num_clusters++;
                        topology_info.max_cluster_id = MAX(topology_info.max_cluster_id, cpu->cluster_id);

                        cluster->cluster_id = cpu->cluster_id;
                        cluster->cluster_type = cpu->cluster_type;
                        cluster->first_cpu_id = cpu->cpu_id;
                        cluster_phys_to_logical[phys_cluster_id] = cpu->cluster_id;

                        ml_read_reg_range(child, "acc-impl-reg", &cluster->acc_IMPL_pa, &cluster->acc_IMPL_len);
                        ml_read_reg_range(child, "cpm-impl-reg", &cluster->cpm_IMPL_pa, &cluster->cpm_IMPL_len);
                }

                cluster->num_cpus++;
                cluster->cpu_mask |= 1ULL << cpu->cpu_id;

                if (is_boot_cpu || topology_info.boot_cpu == NULL) {
                        topology_info.boot_cpu = cpu;
                        topology_info.boot_cluster = cluster;
                }
        }

        cluster_offsets[0] = 0;
        topology_info.chip_revision = 0;

        __builtin_arm_wsr64("TPIDRRO_EL0", (uint64_t)topology_info.boot_cpu->cpu_id);
}

const ml_topology_info_t *
ml_get_topology_info(void)
{
        return &topology_info;
}

void
ml_map_cpu_pio(void)
{
        for (unsigned int i = 0; i < topology_info.num_cpus; i++) {
                ml_topology_cpu_t *cpu = &topology_info.cpus[i];
                if (cpu->cpu_IMPL_pa) {
                        cpu->cpu_IMPL_regs = (vm_offset_t)ml_io_map(cpu->cpu_IMPL_pa, cpu->cpu_IMPL_len);
                        cpu->coresight_regs = (vm_offset_t)ml_io_map(cpu->coresight_pa, cpu->coresight_len);
                }
        }
}

unsigned int ml_get_cpu_count(void) { return topology_info.num_cpus; }
unsigned int ml_get_cluster_count(void) { return topology_info.num_clusters; }
int ml_get_boot_cpu_number(void) { return topology_info.boot_cpu ? topology_info.boot_cpu->cpu_id : 0; }
cluster_type_t ml_get_boot_cluster(void) { return topology_info.boot_cluster ? topology_info.boot_cluster->cluster_type : CLUSTER_TYPE_SMP; }

int
ml_get_cpu_number(uint32_t phys_id)
{
        phys_id &= MPIDR_AFF1_MASK | MPIDR_AFF0_MASK;
        for (unsigned i = 0; i < topology_info.num_cpus; i++) {
                if (topology_info.cpus[i].phys_id == phys_id) return i;
        }
        return 0;
}

int ml_get_cluster_number(uint32_t phys_id) { return 0; }

unsigned int
ml_get_cpu_number_local(void)
{
        uint64_t mpidr_el1_value = 0;
        MRS(mpidr_el1_value, "MPIDR_EL1");
        return ml_get_cpu_number((uint32_t)mpidr_el1_value);
}

int ml_get_cluster_number_local(void) { return 0; }
int ml_get_max_cpu_number(void) { return topology_info.max_cpu_id; }
int ml_get_max_cluster_number(void) { return topology_info.max_cluster_id; }
unsigned int ml_get_first_cpu_id(unsigned int cluster_id) { return topology_info.clusters[cluster_id].first_cpu_id; }

void ml_lockdown_init(void) {}

kern_return_t
ml_lockdown_handler_register(lockdown_handler_t f, void *this)
{
        if (!f) return KERN_FAILURE;
        f(this);
        return KERN_SUCCESS;
}

kern_return_t
ml_processor_register(ml_processor_info_t *in_processor_info, processor_t *processor_out, ipi_handler_t *ipi_handler_out, perfmon_interrupt_handler_func *pmi_handler_out)
{
        cpu_data_t *this_cpu_datap = &BootCpuData;
        processor_set_t pset = pset_find(0, processor_pset(master_processor));

        this_cpu_datap->cpu_id = in_processor_info->cpu_id;
        *processor_out = PERCPU_GET_RELATIVE(processor, cpu_data, this_cpu_datap);
        *ipi_handler_out = cpu_signal_handler;
        *pmi_handler_out = NULL;

        return KERN_SUCCESS;
}

void
ml_init_arm_debug_interface(void * in_cpu_datap, vm_offset_t virt_address)
{
        ((cpu_data_t *)in_cpu_datap)->cpu_debug_interface_map = virt_address;
}

void init_ast_check(__unused processor_t processor) {}

void
cause_ast_check(processor_t processor)
{
        if (current_processor() != processor) {
                cpu_signal(processor_to_cpu_datap(processor), SIGPast, (void *)NULL, (void *)NULL);
        }
}

extern uint32_t cpu_idle_count;

void
ml_get_power_state(boolean_t *icp, boolean_t *pidlep)
{
        *icp = ml_at_interrupt_context();
        *pidlep = (cpu_idle_count == real_ncpus);
}

void ml_cause_interrupt(void) {}

vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size) { return io_map(phys_addr, size, VM_WIMG_IO); }
vm_offset_t ml_io_map_with_prot(vm_offset_t phys_addr, vm_size_t size, vm_prot_t prot) { return io_map_with_prot(phys_addr, size, VM_WIMG_IO, prot); }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size) { return io_map(phys_addr, size, VM_WIMG_WCOMB); }

void
ml_io_unmap(vm_offset_t addr, vm_size_t sz)
{
        pmap_remove(kernel_pmap, addr, addr + sz);
        kmem_free(kernel_map, addr, sz);
}

vm_offset_t ml_static_malloc(__unused vm_size_t size) { return (vm_offset_t) NULL; }
vm_map_address_t ml_map_high_window(vm_offset_t phys_addr, vm_size_t len) { return pmap_map_high_window_bd(phys_addr, len, VM_PROT_READ | VM_PROT_WRITE); }
vm_offset_t ml_static_ptovirt(vm_offset_t paddr) { return phystokv(paddr); }

vm_offset_t
ml_static_slide(vm_offset_t vaddr)
{
        vm_offset_t slid_vaddr = vaddr + vm_kernel_slide;
        if ((slid_vaddr < vm_kernelcache_base) || (slid_vaddr >= vm_kernelcache_top)) return 0;
        return slid_vaddr;
}

vm_offset_t
ml_static_unslide(vm_offset_t vaddr)
{
        if ((vaddr < vm_kernelcache_base) || (vaddr >= vm_kernelcache_top)) return 0;
        return vaddr - vm_kernel_slide;
}

extern tt_entry_t *arm_kva_to_tte(vm_offset_t va);

kern_return_t
ml_static_protect(vm_offset_t vaddr, vm_size_t size, vm_prot_t new_prot)
{
        pt_entry_t  arm_prot = 0;
        vm_offset_t vaddr_cur;
        ppnum_t     ppn;

        if (vaddr < VM_MIN_KERNEL_ADDRESS) return KERN_FAILURE;

        if (new_prot & VM_PROT_WRITE) arm_prot |= ARM_PTE_AP(AP_RWNA);
        else arm_prot |= ARM_PTE_AP(AP_RONA);

        arm_prot |= ARM_PTE_NX;
        if (!(new_prot & VM_PROT_EXECUTE)) arm_prot |= ARM_PTE_PNX;

        for (vaddr_cur = vaddr; vaddr_cur < trunc_page_64(vaddr + size); vaddr_cur += PAGE_SIZE) {
                ppn = pmap_find_phys(kernel_pmap, vaddr_cur);
                if (ppn != (vm_offset_t) NULL) {
                        tt_entry_t *tte2 = arm_kva_to_tte(vaddr_cur);
                        if (((*tte2) & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_TABLE) {
                                pt_entry_t *pte_p = (pt_entry_t *)&((tt_entry_t*)(phystokv((*tte2) & ARM_TTE_TABLE_MASK)))[(((vaddr_cur) & ARM_TT_L3_INDEX_MASK) >> ARM_TT_L3_SHIFT)];
                                *pte_p = (*pte_p & ~(ARM_PTE_APMASK | ARM_PTE_PNXMASK | ARM_PTE_NXMASK)) | arm_prot;
                        }
                }
        }

        if (vaddr_cur > vaddr) {
                flush_mmu_tlb_region(vaddr, (uint32_t)(vaddr_cur - vaddr));
        }

        return KERN_SUCCESS;
}

void
ml_static_mfree(vm_offset_t vaddr, vm_size_t size)
{
        vm_offset_t vaddr_cur;
        ppnum_t     ppn;
        uint32_t    freed_pages = 0;

        if (vaddr < VM_MIN_KERNEL_ADDRESS) return;

        for (vaddr_cur = vaddr; vaddr_cur < trunc_page_64(vaddr + size); vaddr_cur += PAGE_SIZE) {
                ppn = pmap_find_phys(kernel_pmap, vaddr_cur);
                if (ppn != (vm_offset_t) NULL) {
                        ml_static_protect(vaddr_cur, PAGE_SIZE, VM_PROT_WRITE | VM_PROT_READ);
                        vm_page_create(ppn, (ppn + 1));
                        freed_pages++;
                }
        }
        vm_page_lockspin_queues();
        vm_page_wire_count -= freed_pages;
        vm_page_wire_count_initial -= freed_pages;
        vm_page_unlock_queues();
}

vm_offset_t ml_vtophys(vm_offset_t vaddr) { return kvtophys(vaddr); }

vm_size_t
ml_nofault_copy(vm_offset_t virtsrc, vm_offset_t virtdst, vm_size_t size)
{
        addr64_t  cur_phys_dst, cur_phys_src;
        vm_size_t count, nbytes = 0;

        while (size > 0) {
                if (!(cur_phys_src = kvtophys(virtsrc))) break;
                if (!(cur_phys_dst = kvtophys(virtdst))) break;

                count = PAGE_SIZE - (cur_phys_src & PAGE_MASK);
                if (count > (PAGE_SIZE - (cur_phys_dst & PAGE_MASK))) count = PAGE_SIZE - (cur_phys_dst & PAGE_MASK);
                if (count > size) count = size;

                bcopy_phys(cur_phys_src, cur_phys_dst, count);

                nbytes += count;
                virtsrc += count;
                virtdst += count;
                size -= count;
        }
        return nbytes;
}

boolean_t
ml_validate_nofault(vm_offset_t virtsrc, vm_size_t size)
{
        addr64_t cur_phys_src;
        uint32_t count;

        while (size > 0) {
                if (!(cur_phys_src = kvtophys(virtsrc))) return FALSE;
                count = (uint32_t)(PAGE_SIZE - (cur_phys_src & PAGE_MASK));
                if (count > size) count = (uint32_t)size;
                virtsrc += count;
                size -= count;
        }
        return TRUE;
}

void ml_get_bouncepool_info(vm_offset_t * phys_addr, vm_size_t * size) { *phys_addr = 0; *size = 0; }
void active_rt_threads(__unused boolean_t active) {}

static void cpu_qos_cb_default(__unused int urgency, __unused uint64_t qos_param1, __unused uint64_t qos_param2) {}
cpu_qos_update_t cpu_qos_update = cpu_qos_cb_default;

void
cpu_qos_update_register(cpu_qos_update_t cpu_qos_cb)
{
        cpu_qos_update = (cpu_qos_cb != NULL) ? cpu_qos_cb : cpu_qos_cb_default;
}

void
thread_tell_urgency(thread_urgency_t urgency, uint64_t rt_period, uint64_t rt_deadline, uint64_t sched_latency __unused, __unused thread_t nthread)
{
        cpu_qos_update((int)urgency, rt_period, rt_deadline);
}

void machine_run_count(__unused uint32_t count) {}
processor_t machine_choose_processor(__unused processor_set_t pset, processor_t processor) { return processor; }

boolean_t machine_timeout_suspended(void) { return FALSE; }
kern_return_t ml_interrupt_prewarm(__unused uint64_t deadline) { return KERN_FAILURE; }

void
ml_set_decrementer(uint32_t dec_value)
{
        cpu_data_t *cdp = getCpuDatap();
        cdp->cpu_decrementer = dec_value;
        if (cdp->cpu_set_decrementer_func) {
                cdp->cpu_set_decrementer_func(dec_value);
        } else {
                __builtin_arm_wsr64("CNTV_TVAL_EL0", (uint64_t)dec_value);
        }
}

uint64_t
ml_get_hwclock(void)
{
        uint64_t timebase;
        __builtin_arm_isb(ISB_SY);
        timebase = __builtin_arm_rsr64("CNTVCT_EL0");
        return timebase;
}

uint64_t ml_get_timebase(void) { return ml_get_hwclock() + getCpuDatap()->cpu_base_timebase; }
uint64_t ml_get_speculative_timebase(void) { return __builtin_arm_rsr64("CNTVCT_EL0") + getCpuDatap()->cpu_base_timebase; }
uint64_t ml_get_timebase_entropy(void) { return ml_get_speculative_timebase(); }

uint32_t
ml_get_decrementer(void)
{
        cpu_data_t *cdp = getCpuDatap();
        if (cdp->cpu_get_decrementer_func) return cdp->cpu_get_decrementer_func();
        return (uint32_t)__builtin_arm_rsr64("CNTV_TVAL_EL0");
}

boolean_t
ml_get_timer_pending(void)
{
        return (__builtin_arm_rsr64("CNTV_CTL_EL0") & CNTV_CTL_EL0_ISTATUS) != 0;
}

static void cache_trap_error(thread_t thread, vm_map_address_t fault_addr) {}

static void
cache_trap_recover(void)
{
        vm_map_address_t fault_addr;
        __asm__ volatile ("mrs %0, FAR_EL1" : "=r"(fault_addr));
        cache_trap_error(current_thread(), fault_addr);
}

static void
set_cache_trap_recover(thread_t thread)
{
        thread->recover = (vm_address_t)cache_trap_recover;
}

static void
dcache_flush_trap(vm_map_address_t start, vm_map_size_t size)
{
        thread_t thread = current_thread();
        vm_offset_t old_recover = thread->recover;
        set_cache_trap_recover(thread);
        FlushPoC_DcacheRegion(start, (uint32_t)size);
        thread->recover = old_recover;
}

static void
icache_invalidate_trap(vm_map_address_t start, vm_map_size_t size)
{
        thread_t thread = current_thread();
        vm_offset_t old_recover = thread->recover;
        set_cache_trap_recover(thread);
        InvalidatePoU_IcacheRegion(start, (uint32_t)size);
        thread->recover = old_recover;
}

__attribute__((noreturn))
void
platform_syscall(arm_saved_state_t *state)
{
        uint32_t code = (uint32_t)get_saved_state_reg(state, 3);
        switch (code) {
        case 0: icache_invalidate_trap(get_saved_state_reg(state, 0), get_saved_state_reg(state, 1)); break;
        case 1: dcache_flush_trap(get_saved_state_reg(state, 0), get_saved_state_reg(state, 1)); break;
        case 2: thread_set_cthread_self(get_saved_state_reg(state, 0)); break;
        case 3: set_saved_state_reg(state, 0, thread_get_cthread_self()); break;
        }
        thread_exception_return();
}

static void
_enable_timebase_event_stream(uint32_t bit_index)
{
        uint64_t cntkctl;
        __asm__ volatile ("mrs %0, CNTKCTL_EL1" : "=r"(cntkctl));
        cntkctl |= (bit_index << CNTKCTL_EL1_EVENTI_SHIFT) | CNTKCTL_EL1_EVNTEN | CNTKCTL_EL1_EVENTDIR;
        cntkctl |= (CNTKCTL_EL1_PL0PCTEN | CNTKCTL_EL1_PL0VCTEN);
        __builtin_arm_wsr64("CNTKCTL_EL1", cntkctl);
}

static void
_enable_virtual_timer(void)
{
        __builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_ENABLE);
        __builtin_arm_wsr64("CNTP_CTL_EL0", CNTP_CTL_EL0_IMASKED);
}

void
fiq_context_init(boolean_t enable_fiq __unused)
{
        _enable_virtual_timer();
}

void wfe_timeout_init(void) { _enable_timebase_event_stream(arm64_eventi); }

void
wfe_timeout_configure(void)
{
        arm64_eventi = 10;
        wfe_timeout_init();
}

boolean_t ml_delay_should_spin(uint64_t interval) { return FALSE; }
boolean_t ml_thread_is64bit(thread_t thread) { return thread_is_64bit_addr(thread); }
void ml_delay_on_yield(void) {}
void ml_timer_evaluate(void) {}
boolean_t ml_timer_forced_evaluation(void) { return FALSE; }
uint64_t ml_energy_stat(thread_t t) { return t->machine.energy_estimate_nj; }
void ml_gpu_stat_update(__unused uint64_t gpu_ns_delta) {}
uint64_t ml_gpu_stat(__unused thread_t t) { return 0; }

thread_t current_act(void) { return current_thread_fast(); }
thread_t current_thread(void) { return current_thread_fast(); }

ex_cb_info_t ex_cb_info[EXCB_CLASS_MAX];

kern_return_t
ex_cb_register(ex_cb_class_t cb_class, ex_cb_t cb, void *refcon)
{
        if ((NULL == cb) || (cb_class >= EXCB_CLASS_MAX)) return KERN_INVALID_VALUE;
        if (NULL == ex_cb_info[cb_class].cb) {
                ex_cb_info[cb_class].cb = cb;
                ex_cb_info[cb_class].refcon = refcon;
                return KERN_SUCCESS;
        }
        return KERN_FAILURE;
}

ex_cb_action_t
ex_cb_invoke(ex_cb_class_t cb_class, vm_offset_t far)
{
        ex_cb_state_t state = {far};
        if (ex_cb_info[cb_class].cb) {
                return ex_cb_info[cb_class].cb(cb_class, ex_cb_info[cb_class].refcon, &state);
        }
        return EXCB_ACTION_NONE;
}

void ml_hibernate_active_pre(void) {}
void ml_hibernate_active_post(void) {}

size_t
ml_get_vm_reserved_regions(bool vm_is64bit, struct vm_reserved_region **regions)
{
        if (vm_is64bit) {
                *regions = vm_reserved_regions;
                return ARRAY_COUNT(vm_reserved_regions);
        }
        *regions = NULL;
        return 0;
}

static uint64_t arm64_cluster_wfe_recs[MAX_CPU_CLUSTERS];

uint32_t
ml_update_cluster_wfe_recommendation(uint32_t wfe_cluster_id, uint64_t wfe_timeout_abstime_interval, __unused uint64_t wfe_hint_flags)
{
        os_atomic_store(&arm64_cluster_wfe_recs[wfe_cluster_id], wfe_timeout_abstime_interval, relaxed);
        return 0;
}

uint64_t
ml_cluster_wfe_timeout(uint32_t wfe_cluster_id)
{
        return os_atomic_load(&arm64_cluster_wfe_recs[wfe_cluster_id], relaxed);
}
