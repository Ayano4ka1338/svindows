/*
 * ke/Log.c - the kernel logger (KeLog / DbgPrint) and bugcheck.
 *
 * Formats with the Rtl engine into a fixed stack buffer and pushes the result
 * to the HAL console. Long lines are truncated to the buffer size.
 */
#include <svindows/Ke.h>
#include <svindows/Hal.h>
#include <svindows/Rtl.h>
#include <svindows/Gfx.h>

#define KE_LOG_BUFFER_SIZE 512

/* Interrupt masking only serializes callers on one CPU. Once APs run kernel
 * workers, a second processor can otherwise splice its bytes into this log. */
static volatile UINT32 g_log_lock;
static volatile UINT64 g_dropped_log_records;

void KeLogInit(void)
{
    HalInitializeConsole();
}

int KeVLog(const char *fmt, va_list ap)
{
    char buffer[KE_LOG_BUFFER_SIZE];
    int n = RtlFormatV(buffer, sizeof(buffer), fmt, ap);

    /* Keep the formatted record atomic across processors as well as against
     * local interrupt handlers. This lock is intentionally usable before the
     * per-CPU GS block is initialized. */
    UINT64 flags = KiIrqSave();
    UINT32 spins = 0;
    while (__atomic_exchange_n(&g_log_lock, 1, __ATOMIC_ACQUIRE)) {
        /* A diagnostic must not wedge every interrupt on this CPU waiting
         * indefinitely for another writer. Leave its lock untouched and
         * drop only this record if bounded contention cannot be resolved. */
        if (++spins == 1000000u) {
            __atomic_add_fetch(&g_dropped_log_records, 1, __ATOMIC_RELAXED);
            KiIrqRestore(flags);
            return n;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    HalConsoleWrite(buffer);
    __atomic_store_n(&g_log_lock, 0, __ATOMIC_RELEASE);
    KiIrqRestore(flags);
    return n;
}

int KeLog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = KeVLog(fmt, ap);
    va_end(ap);
    return n;
}

NORETURN void KeBugCheck(ULONG code, const char *message)
{
    __cli();

    /* A UEFI GOP display cannot show the legacy VGA log sink. Paint the
     * fatal error directly, before taking the log lock or touching UARTs. */
    if (GfxAvailable()) {
        char stop[64];
        RtlFormat(stop, sizeof(stop), "STOP: 0x%08x", code);
        UINT32 background = GfxColor(0, 0, 120);
        UINT32 foreground = GfxColor(255, 255, 255);
        GfxFillRect(0, 0, GfxFramebuffer.Width, 3 * GFX_FONT_H, background);
        GfxDrawString(0, 0, stop, foreground, background);
        if (message)
            GfxDrawString(0, GFX_FONT_H, message, foreground, background);
    }

    HalVgaSetColor(VGA_COLOR(VGA_WHITE, VGA_BLUE));
    KeLog("\n*** STOP: 0x%08x\n", code);
    if (message)
        KeLog("*** %s\n", message);
    KeLog("*** The system has halted.\n");

    for (;;)
        __halt();
}
