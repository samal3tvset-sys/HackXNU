#include <sys/types.h>
#include <sys/kdebug.h>
#include <mach/vm_param.h>
#include <pexpert/protos.h>
#include <pexpert/pexpert.h>
#include <pexpert/boot.h>
#include <pexpert/device_tree.h>
#include <pexpert/pe_images.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <kern/debug.h>
#include <libkern/section_keywords.h>

#if defined __arm__
#include <pexpert/arm/board_config.h>
#elif defined __arm64__
#include <pexpert/arm64/board_config.h>
#endif

/* Extern references */
extern void pe_identify_machine(boot_args *bootArgs);

/* Static references */
static void pe_prepare_images(void);

/* Private globals */
SECURITY_READ_ONLY_LATE(PE_state_t) PE_state;
#define FW_VERS_LEN 128
char firmware_version[FW_VERS_LEN] = "HackXNU-CoreStrap-v0.1";

SECURITY_READ_ONLY_SPECIAL_SECTION(volatile uint32_t, "__TEXT,__const") debug_enabled = FALSE;

uint8_t  gPlatformECID[8];
uint32_t gPlatformMemoryID;
static boolean_t vc_progress_initialized = FALSE;
uint64_t last_hwaccess_thread = 0;
char gTargetTypeBuffer[16] = "exynos7884";
char gModelTypeBuffer[32]  = "Samsung Galaxy A10";

/* Clock Frequency Info */
clock_frequency_info_t gPEClockFrequencyInfo;

vm_offset_t gPanicBase = 0;
unsigned int gPanicSize = 0;
struct embedded_panic_header *panic_info = NULL;

static unsigned int panic_text_len = 0;
static boolean_t panic_console_available = FALSE;

extern uint32_t crc32(uint32_t crc, const void *buf, size_t size);

void PE_slide_devicetree(vm_offset_t);

static void
check_for_panic_log(void)
{
#ifdef PLATFORM_PANIC_LOG_PADDR
        gPanicBase = ml_io_map_wcomb(PLATFORM_PANIC_LOG_PADDR, PLATFORM_PANIC_LOG_SIZE);
        panic_text_len = PLATFORM_PANIC_LOG_SIZE - sizeof(struct embedded_panic_header);
        gPanicSize = PLATFORM_PANIC_LOG_SIZE;
#else
        DTEntry entry, chosen;
        unsigned int size;
        uintptr_t const *reg_prop;
        uint32_t const *panic_region_length;

        /* Безопасный поиск panic-региона в DeviceTree от CoreStrap */
        if (kSuccess != SecureDTLookupEntry(0, "pram", &entry)) {
                return;
        }

        if (kSuccess != SecureDTGetProperty(entry, "reg", (void const **)&reg_prop, &size)) {
                return;
        }

        if (kSuccess != SecureDTLookupEntry(0, "/chosen", &chosen)) {
                return;
        }

        if (kSuccess != SecureDTGetProperty(chosen, "embedded-panic-log-size", (void const **) &panic_region_length, &size)) {
                return;
        }

        gPanicBase = ml_io_map_wcomb(reg_prop[0], panic_region_length[0]);
        panic_text_len = panic_region_length[0] - sizeof(struct embedded_panic_header);
        gPanicSize = panic_region_length[0];
#endif

        if (!gPanicBase) {
                return;
        }

        panic_info = (struct embedded_panic_header *)gPanicBase;

        if (panic_info->eph_magic == 'SHMC') {
                panic_console_available = TRUE;
                return;
        }

        if (panic_info->eph_magic == 'BTRC') {
                return;
        }

        if (panic_info->eph_magic == EMBEDDED_PANIC_MAGIC) {
                printf("[HackXNU] Found previous panic log in memory.\n");
        }

        bzero((void *)gPanicBase, gPanicSize);
}

int
PE_initialize_console(PE_Video * info, int op)
{
        static int last_console = -1;

        if (info && (info != &PE_state.video)) {
                info->v_scale = PE_state.video.v_scale;
        }

        switch (op) {
        case kPEDisableScreen:
                initialize_screen(info, op);
                last_console = switch_to_serial_console();
                break;

        case kPEEnableScreen:
                initialize_screen(info, op);
                if (info) {
                        PE_state.video = *info;
                }
                if (last_console != -1) {
                        switch_to_old_console(last_console);
                }
                break;

        case kPEReleaseScreen:
                if (!vc_progress_initialized) {
                        default_progress.dx = 0;
                        default_progress.dy = 0;
                        vc_progress_initialize(&default_progress,
                            default_progress_data1x,
                            default_progress_data2x,
                            default_progress_data3x,
                            (unsigned char *) appleClut8);
                        vc_progress_initialized = TRUE;
                }
                initialize_screen(info, op);
                break;

        default:
                initialize_screen(info, op);
                break;
        }

        return 0;
}

void
PE_init_iokit(void)
{
        PE_init_printf(TRUE);

        printf("[HackXNU] Bootloader / Firmware version: %s\n", firmware_version);

        pe_prepare_images();

        InitIOKit(PE_state.deviceTreeHead);
        ConfigureIOKit();
}

void
PE_lockdown_iokit(void)
{
        /*
         * Старт матчинга IOKit после инициализации базовых подсистем.
         */
        StartIOKitMatching();
}

void
PE_slide_devicetree(vm_offset_t slide)
{
        assert(PE_state.initialized);
        PE_state.deviceTreeHead += slide;
        SecureDTInit(PE_state.deviceTreeHead, PE_state.deviceTreeSize);
}

void
PE_init_platform(boolean_t vm_initialized, void *args)
{
        DTEntry         entry;
        unsigned int    size;
        void * const    *prop;
        boot_args      *boot_args_ptr = (boot_args *) args;

        if (PE_state.initialized == FALSE) {
                PE_state.initialized = TRUE;
                PE_state.bootArgs = boot_args_ptr;
                PE_state.deviceTreeHead = boot_args_ptr->deviceTreeP;
                PE_state.deviceTreeSize = boot_args_ptr->deviceTreeLength;
                PE_state.video.v_baseAddr = boot_args_ptr->Video.v_baseAddr;
                PE_state.video.v_rowBytes = boot_args_ptr->Video.v_rowBytes;
                PE_state.video.v_width = boot_args_ptr->Video.v_width;
                PE_state.video.v_height = boot_args_ptr->Video.v_height;
                PE_state.video.v_depth = (boot_args_ptr->Video.v_depth >> kBootVideoDepthDepthShift) & kBootVideoDepthMask;
                PE_state.video.v_rotate = 0;
                PE_state.video.v_scale = 1;
                PE_state.video.v_display = boot_args_ptr->Video.v_display;
                strlcpy(PE_state.video.v_pixelFormat, "BBBBBBBBGGGGGGGGRRRRRRRR", sizeof(PE_state.video.v_pixelFormat));
        }

        if (!vm_initialized) {
                SecureDTInit(PE_state.deviceTreeHead, PE_state.deviceTreeSize);
                pe_identify_machine(boot_args_ptr);
        } else {
                pe_arm_init_interrupts(args);
                pe_arm_init_debug(args);
        }

        if (!vm_initialized) {
                if (kSuccess == (SecureDTFindEntry("name", "device-tree", &entry))) {
                        if (kSuccess == SecureDTGetProperty(entry, "target-type", (void const **)&prop, &size)) {
                                if (size > sizeof(gTargetTypeBuffer)) size = sizeof(gTargetTypeBuffer);
                                bcopy(prop, gTargetTypeBuffer, size);
                                gTargetTypeBuffer[size - 1] = '\0';
                        }
                        if (kSuccess == SecureDTGetProperty(entry, "model", (void const **)&prop, &size)) {
                                if (size > sizeof(gModelTypeBuffer)) size = sizeof(gModelTypeBuffer);
                                bcopy(prop, gModelTypeBuffer, size);
                                gModelTypeBuffer[size - 1] = '\0';
                        }
                }
                pe_init_debug();
        }
}

void
PE_create_console(void)
{
        check_for_panic_log();

        if (PE_state.video.v_display) {
                PE_initialize_console(&PE_state.video, kPEGraphicsMode);
        } else {
                PE_initialize_console(&PE_state.video, kPETextMode);
        }
}

int
PE_current_console(PE_Video * info)
{
        *info = PE_state.video;
        return 0;
}

void
PE_display_icon(__unused unsigned int flags, __unused const char *name)
{
        if (default_noroot_data) {
                vc_display_icon(&default_noroot, default_noroot_data);
        }
}

extern boolean_t
PE_get_hotkey(__unused unsigned char key)
{
        return FALSE;
}

static timebase_callback_func gTimebaseCallback;

void
PE_register_timebase_callback(timebase_callback_func callback)
{
        gTimebaseCallback = callback;
        PE_call_timebase_callback();
}

void
PE_call_timebase_callback(void)
{
        struct timebase_freq_t timebase_freq;

        timebase_freq.timebase_num = gPEClockFrequencyInfo.timebase_frequency_hz;
        timebase_freq.timebase_den = 1;

        if (gTimebaseCallback) {
                gTimebaseCallback(&timebase_freq);
        }
}

int
PE_stub_poll_input(__unused unsigned int options, char *c)
{
        *c = (char)uart_getc();
        return 0;
}

uint32_t
PE_i_can_has_debugger(uint32_t *debug_flags)
{
        if (debug_flags) {
                *debug_flags = debug_enabled ? debug_boot_arg : 0;
        }
        return debug_enabled;
}

boolean_t
PE_panic_debugging_enabled(void)
{
        return panicDebugging;
}

void
PE_save_buffer_to_vram(unsigned char *buf, unsigned int *size)
{
        if (!panic_info || !size || *size == 0 || !buf) {
                return;
        }

        *size = *size > panic_text_len ? panic_text_len : *size;

        panic_info->eph_crc = crc32(0L, &panic_info->eph_version, (panic_text_len +
            sizeof(struct embedded_panic_header) - offsetof(struct embedded_panic_header, eph_version)));
}

uint32_t
PE_get_offset_into_panic_region(char *location)
{
        assert(gPanicBase != 0);
        return (uint32_t)(uintptr_t)(location - gPanicBase);
}

void
PE_init_panicheader(void)
{
        if (!panic_info) {
                return;
        }

        bzero(panic_info, sizeof(struct embedded_panic_header));
        panic_info->eph_panic_log_offset = debug_buf_base ? PE_get_offset_into_panic_region(debug_buf_base) : 0;
        panic_info->eph_magic = EMBEDDED_PANIC_MAGIC;
        panic_info->eph_version = EMBEDDED_PANIC_HEADER_CURRENT_VERSION;
}

void
PE_update_panicheader_nestedpanic(void)
{
        if (!panic_info) {
                return;
        }

        if (panic_info->eph_panic_log_offset == 0) {
                PE_init_panicheader();
                panic_info->eph_panic_flags |= EMBEDDED_PANIC_HEADER_FLAG_NESTED_PANIC;
                return;
        }

        panic_info->eph_panic_flags |= EMBEDDED_PANIC_HEADER_FLAG_NESTED_PANIC;

        if (panic_info->eph_panic_log_len == 0) {
                panic_info->eph_panic_log_len = PE_get_offset_into_panic_region(debug_buf_ptr);
        }

        if (panic_info->eph_other_log_offset == 0) {
                panic_info->eph_other_log_offset = PE_get_offset_into_panic_region(debug_buf_ptr);
        }
}

boolean_t
PE_reboot_on_panic(void)
{
        return TRUE;
}

void
PE_sync_panic_buffers(void)
{
        if (gPanicBase) {
                CleanPoC_DcacheRegion_Force(gPanicBase, gPanicSize);
        }
}

static void
pe_prepare_images(void)
{
        // Базовая подготовка ресурсов экрана при необходимости
}

void
PE_mark_hwaccess(uint64_t thread)
{
        last_hwaccess_thread = thread;
        asm volatile ("dmb ish");
}
