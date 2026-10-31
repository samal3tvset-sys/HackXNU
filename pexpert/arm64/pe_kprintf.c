#include <stdarg.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include <kern/simple_lock.h>
#include <libkern/section_keywords.h>

/* Globals */
typedef void (*PE_kputc_t)(char);
SECURITY_READ_ONLY_LATE(PE_kputc_t) PE_kputc = NULL;

// Для HackXNU отладочный вывод включен всегда по умолчанию
SECURITY_READ_ONLY_LATE(unsigned int) disable_serial_output = FALSE;
static SECURITY_READ_ONLY_LATE(unsigned int) disable_kprintf_output = FALSE;

static SIMPLE_LOCK_DECLARE(kprintf_lock, 0);

static void serial_putc_crlf(char c);

__startup_func
static void
PE_init_kprintf(void)
{
        disable_serial_output = FALSE;
        disable_kprintf_output = FALSE;

        if (serial_init()) {
                PE_kputc = serial_putc_crlf;
        } else {
                PE_kputc = serial_putc_crlf; // Гарантированный фоллбэк на UART
        }
}
STARTUP(KPRINTF, STARTUP_RANK_FIRST, PE_init_kprintf);

#ifdef MP_DEBUG
static void
_kprintf(const char *format, ...)
{
        va_list listp;

        va_start(listp, format);
        _doprnt_log(format, &listp, PE_kputc, 16);
        va_end(listp);
}
#define MP_DEBUG_KPRINTF(x...) _kprintf(x)
#else
#define MP_DEBUG_KPRINTF(x...)
#endif

#if CONFIG_NO_KPRINTF_STRINGS
#undef kprintf
#endif

static int cpu_last_locked = 0;

__attribute__((noinline, not_tail_called))
void
kprintf(const char *fmt, ...)
{
        va_list   listp;
        boolean_t state;

        if (!PE_kputc) {
                PE_kputc = serial_putc_crlf;
        }

        va_start(listp, fmt);

        state = ml_set_interrupts_enabled(FALSE);
        while (!simple_lock_try(&kprintf_lock, LCK_GRP_NULL)) {
                ml_set_interrupts_enabled(state);
                ml_set_interrupts_enabled(FALSE);
        }

        if (cpu_number() != cpu_last_locked) {
                MP_DEBUG_KPRINTF("[cpu%d...]\n", cpu_number());
                cpu_last_locked = cpu_number();
        }

        _doprnt_log(fmt, &listp, PE_kputc, 16);

        simple_unlock(&kprintf_lock);
        ml_set_interrupts_enabled(state);

        va_end(listp);
}

static void
serial_putc_crlf(char c)
{
        if (c == '\n') {
                uart_putc('\r');
        }
        uart_putc(c);
}

void
serial_putc(char c)
{
        uart_putc(c);
}

int
serial_getc(void)
{
        return uart_getc();
}
