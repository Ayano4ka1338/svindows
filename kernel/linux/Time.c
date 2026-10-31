/* Linux timespec is two signed 64-bit values on x86-64. */
#include "Internal.h"
#include <svindows/Rtl.h>

typedef struct _LINUX_TIMESPEC {
    INT64 Seconds;
    INT64 Nanoseconds;
} LINUX_TIMESPEC;

#define FILETIME_UNIX_EPOCH 116444736000000000ULL

UINT64 LinuxClockGettime(UINT64 clock_id, UINT64 destination)
{
    if (clock_id != 0 && clock_id != 1)
        return LinuxError(LINUX_EINVAL);
    if (!MmProbeForWrite(destination, sizeof(LINUX_TIMESPEC)))
        return LinuxError(LINUX_EFAULT);

    UINT64 units;
    if (clock_id == 0) {
        UINT64 now = KeQuerySystemTimeFileTime();
        if (now < FILETIME_UNIX_EPOCH) return LinuxError(LINUX_EINVAL);
        units = now - FILETIME_UNIX_EPOCH;
    } else {
        /* The PIT advances the kernel tick counter every 10 ms. */
        units = KeGetTickCount() * 100000ULL;
    }
    LINUX_TIMESPEC *time = (LINUX_TIMESPEC *)(ULONG_PTR)destination;
    time->Seconds = (INT64)(units / 10000000ULL);
    time->Nanoseconds = (INT64)((units % 10000000ULL) * 100);
    return 0;
}

UINT64 LinuxNanosleep(UINT64 requested, UINT64 remaining)
{
    LINUX_TIMESPEC duration;
    if (!MmProbeForRead(requested, sizeof(duration)))
        return LinuxError(LINUX_EFAULT);
    memcpy(&duration, (const void *)(ULONG_PTR)requested, sizeof(duration));
    if (duration.Seconds < 0 || duration.Nanoseconds < 0 ||
        duration.Nanoseconds >= 1000000000)
        return LinuxError(LINUX_EINVAL);
    if (remaining && !MmProbeForWrite(remaining, sizeof(duration)))
        return LinuxError(LINUX_EFAULT);
    UINT64 seconds = (UINT64)duration.Seconds;
    if (seconds > (~(UINT64)0 - (UINT64)duration.Nanoseconds -
                   9999999) / 1000000000)
        return LinuxError(LINUX_EINVAL);
    UINT64 ticks = (seconds * 1000000000 +
                    (UINT64)duration.Nanoseconds + 9999999) / 10000000;
    if (ticks) {
        KEVENT event;
        KeInitializeEvent(&event, FALSE, FALSE);
        KeWaitForSingleObjectTimeout(&event.Header, ticks);
    }
    return 0;
}

/* Linux getrandom(2). Do not silently fall back to timestamps for TLS keys:
 * only return bytes from the CPU random-number generator. */
UINT64 LinuxGetRandom(UINT64 destination, UINT64 length, UINT64 flags)
{
    if (flags & ~3ULL) return LinuxError(LINUX_EINVAL);
    if (!length) return 0;
    if (length > 256u * 1024u) length = 256u * 1024u;
    if (!MmProbeForWrite(destination, (SIZE_T)length))
        return LinuxError(LINUX_EFAULT);

    UINT32 eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    if (!(ecx & (1u << 30))) return LinuxError(LINUX_EAGAIN);

    UINT8 *output = (UINT8 *)(ULONG_PTR)destination;
    UINT64 completed = 0;
    while (completed < length) {
        UINT64 value = 0;
        UINT8 valid = 0;
        for (UINT32 attempt = 0; attempt < 10 && !valid; attempt++)
            __asm__ volatile("rdrand %0; setc %1"
                             : "=r"(value), "=qm"(valid));
        if (!valid)
            return completed ? completed : LinuxError(LINUX_EAGAIN);
        UINT64 count = length - completed;
        if (count > sizeof(value)) count = sizeof(value);
        memcpy(output + completed, &value, (SIZE_T)count);
        completed += count;
    }
    return completed;
}
