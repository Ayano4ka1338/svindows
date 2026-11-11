/*
 * ke/Shared.c - KUSER_SHARED_DATA, the read-only page user mode reads directly.
 *
 * Windows maps a shared page at the fixed user address 0x7FFE0000 that the
 * kernel keeps up to date (tick count, system time, ...) so ring 3 can read
 * those values without a system call. We reproduce it: the page is mapped
 * read-only into user space, and the kernel updates it through the direct map
 * on every clock tick. kernel32's GetTickCount/GetSystemTimeAsFileTime read it
 * exactly the way the real ones do.
 */
#include <svindows/Ke.h>
#include <svindows/Mm.h>
#include <svindows/Rtl.h>
#include <svindows/Hal.h>

#define KUSER_SHARED_DATA_USER_VA 0x000000007FFE0000ULL

/* Field offsets within the page (match the Windows KUSER_SHARED_DATA layout). */
#define KUSD_TickCountMultiplier 0x004 /* ULONG                              */
#define KUSD_SystemTime          0x014 /* KSYSTEM_TIME (Low, High1, High2)   */
#define KUSD_TickCount           0x320 /* KSYSTEM_TIME                       */

/* One tick is 10 ms (the PIT runs at 100 Hz). GetTickCount computes
 * (TickCount * Multiplier) >> 24, so the multiplier is (ms per tick) << 24. */
#define MS_PER_TICK      10u
#define HUNDRED_NS_PER_TICK (MS_PER_TICK * 10000ULL) /* 10 ms in 100 ns units */

static volatile UINT8 *g_kusd; /* kernel (direct-map) alias of the page */
static UINT64 g_boot_filetime;

static UINT8 rtc_register(UINT8 index)
{
    __outbyte(0x70, index | 0x80); /* keep NMI masked during the sample */
    return __inbyte(0x71);
}

static UINT8 rtc_decimal(UINT8 value, BOOLEAN binary)
{
    return binary ? value : (UINT8)((value >> 4) * 10 + (value & 15));
}

static UINT64 rtc_filetime(void)
{
    UINT8 second, minute, hour, day, month, year, century, mode;
    /* Read a stable RTC sample rather than crossing a second boundary. */
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        while (rtc_register(0x0A) & 0x80) __asm__ volatile("pause");
        second = rtc_register(0); minute = rtc_register(2);
        hour = rtc_register(4); day = rtc_register(7);
        month = rtc_register(8); year = rtc_register(9);
        century = rtc_register(0x32); mode = rtc_register(0x0B);
        if (!(rtc_register(0x0A) & 0x80) && second == rtc_register(0))
            break;
    }
    BOOLEAN binary = (mode & 4) != 0;
    second = rtc_decimal(second, binary);
    minute = rtc_decimal(minute, binary);
    day = rtc_decimal(day, binary);
    month = rtc_decimal(month, binary);
    year = rtc_decimal(year, binary);
    century = rtc_decimal(century, binary);
    BOOLEAN pm = (hour & 0x80) != 0;
    hour = rtc_decimal(hour & 0x7F, binary);
    if (!(mode & 2)) hour = (UINT8)((hour % 12) + (pm ? 12 : 0));
    unsigned full_year = (century >= 16 && century <= 30 ?
                          (unsigned)century * 100 : 2000) + year;
    if (full_year < 1601 || full_year > 9999 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
        return 0;
    static const UINT16 days_before_month[] =
        {0,31,59,90,120,151,181,212,243,273,304,334};
    UINT64 days = 0;
    for (unsigned y = 1601; y < full_year; y++)
        days += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    days += days_before_month[month - 1] + day - 1;
    if (month > 2 && full_year % 4 == 0 &&
        (full_year % 100 != 0 || full_year % 400 == 0)) days++;
    return (((days * 24 + hour) * 60 + minute) * 60 + second) * 10000000ULL;
}

static ALWAYS_INLINE void wr32(UINT32 off, UINT32 v)
{
    *(volatile UINT32 *)(g_kusd + off) = v;
}

void KeInitializeSharedData(void)
{
    UINT64 pa = MmAllocatePage();
    if (pa == MM_INVALID_PHYS)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "no memory for shared user data");

    /* Read-only in user space (no PTE_WRITE); the kernel writes via the
     * direct map, which is writable. */
    if (!MmMapPage(KUSER_SHARED_DATA_USER_VA, pa, PTE_USER)) {
        MmFreePages(pa, 1);
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "cannot map shared user data");
    }
    g_kusd = (volatile UINT8 *)MmPhysToVirt(pa);
    memset((void *)g_kusd, 0, PAGE_SIZE);

    g_boot_filetime = rtc_filetime();

    wr32(KUSD_TickCountMultiplier, MS_PER_TICK << 24);
    KeUpdateSharedData(KeGetTickCount());

    KeLog("[ke]   KUSER_SHARED_DATA mapped at 0x%lx\n",
          (unsigned long)KUSER_SHARED_DATA_USER_VA);
}

/* Called from the clock tick with the current 64-bit tick count. A KSYSTEM_TIME
 * is written High1, Low, High2 so a lock-free reader that sees High1 == High2
 * has a consistent pair. */
void KeUpdateSharedData(UINT64 ticks)
{
    if (!g_kusd)
        return;

    UINT32 tlow = (UINT32)ticks;
    UINT32 thigh = (UINT32)(ticks >> 32);
    wr32(KUSD_TickCount + 0x4, thigh);       /* High1Time */
    wr32(KUSD_TickCount + 0x0, tlow);        /* LowPart   */
    wr32(KUSD_TickCount + 0x8, thigh);       /* High2Time */

    /* Windows FILETIME counts 100 ns intervals from 1601-01-01 UTC. */
    UINT64 systime = g_boot_filetime + ticks * HUNDRED_NS_PER_TICK;
    UINT32 slow = (UINT32)systime;
    UINT32 shigh = (UINT32)(systime >> 32);
    wr32(KUSD_SystemTime + 0x4, shigh);
    wr32(KUSD_SystemTime + 0x0, slow);
    wr32(KUSD_SystemTime + 0x8, shigh);
}

UINT64 KeQuerySystemTimeFileTime(void)
{
    return g_boot_filetime + KeGetTickCount() * HUNDRED_NS_PER_TICK;
}

UINT64 KeQueryBootTimeFileTime(void)
{
    return g_boot_filetime;
}
