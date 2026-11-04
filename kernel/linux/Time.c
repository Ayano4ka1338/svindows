/* Linux timespec is two signed 64-bit values on x86-64. */
#include "Internal.h"
#include <svindows/Rtl.h>
#include <svindows/Io.h>

#define FILETIME_UNIX_EPOCH 116444736000000000ULL

typedef struct _LINUX_TIMESPEC {
    INT64 Seconds;
    INT64 Nanoseconds;
} LINUX_TIMESPEC;

typedef struct _LINUX_TIMEVAL {
    INT64 Seconds;
    INT64 Microseconds;
} LINUX_TIMEVAL;

static void linux_unix_to_fat_time(INT64 seconds, UINT16 *date, UINT16 *time)
{
    /* FAT's representable range is 1980 through 2107. Clamp timestamps
     * outside it rather than wrapping an unsigned Unix time into nonsense. */
    const UINT64 min_seconds = 315532800ULL;
    const UINT64 max_seconds = 4354819199ULL;
    if (seconds < (INT64)min_seconds) seconds = (INT64)min_seconds;
    if (seconds > (INT64)max_seconds) seconds = (INT64)max_seconds;
    UINT64 days_since_epoch = (UINT64)seconds / 86400ULL;
    UINT32 day_seconds = (UINT32)((UINT64)seconds % 86400ULL);
    UINT64 z = days_since_epoch + 719468ULL;
    UINT64 era = z / 146097ULL;
    UINT32 doe = (UINT32)(z - era * 146097ULL);
    UINT32 yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    INT64 year = (INT64)yoe + (INT64)(era * 400ULL);
    UINT32 doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    UINT32 mp = (5u * doy + 2u) / 153u;
    UINT32 day = doy - (153u * mp + 2u) / 5u + 1u;
    UINT32 month = mp + (mp < 10u ? 3u : (UINT32)-9);
    year += month <= 2u;
    UINT32 hour = day_seconds / 3600u;
    UINT32 minute = (day_seconds / 60u) % 60u;
    UINT32 second = day_seconds % 60u;
    *date = (UINT16)(((year - 1980) << 9) | (month << 5) | day);
    *time = (UINT16)((hour << 11) | (minute << 5) | (second / 2u));
}

static UINT64 linux_update_mtime(PKPROCESS process, UINT64 directory_fd,
                                 UINT64 path, INT64 seconds,
                                 INT64 subseconds, BOOLEAN microseconds,
                                 BOOLEAN now)
{
    if (!now && (subseconds < 0 || subseconds >=
                 (microseconds ? 1000000 : 1000000000)))
        return LinuxError(LINUX_EINVAL);
    char dos_path[260];
    UINT64 error = LinuxCapturePathAt(process, directory_fd, path, dos_path);
    if (error) return error;
    FAT_FIND_DATA info;
    NTSTATUS status = FatQueryPath(dos_path, &info);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND || status == STATUS_NO_SUCH_FILE)
        return LinuxError(LINUX_ENOENT);
    if (!NT_SUCCESS(status)) return LinuxError(LINUX_EIO);
    if (info.Attributes & 0x10u) return LinuxError(LINUX_EISDIR);
    if (now) {
        UINT64 file_time = KeQuerySystemTimeFileTime();
        if (file_time < FILETIME_UNIX_EPOCH) return LinuxError(LINUX_EINVAL);
        seconds = (INT64)((file_time - FILETIME_UNIX_EPOCH) / 10000000ULL);
    }
    UINT16 date, time;
    linux_unix_to_fat_time(seconds, &date, &time);
    status = FatSetFileDateTime(dos_path, date, time);
    return NT_SUCCESS(status) ? 0 : LinuxError(LINUX_EIO);
}

UINT64 LinuxUtimensAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                      UINT64 times, UINT64 flags)
{
    const UINT64 AT_SYMLINK_NOFOLLOW = 0x100u;
    const INT64 UTIME_NOW = 0x3fffffff;
    const INT64 UTIME_OMIT = 0x3ffffffe;
    if (flags & ~AT_SYMLINK_NOFOLLOW) return LinuxError(LINUX_EINVAL);
    if (!times) return linux_update_mtime(process, directory_fd, path,
                                          0, 0, FALSE, TRUE);
    if (!MmProbeForRead(times, sizeof(LINUX_TIMESPEC) * 2))
        return LinuxError(LINUX_EFAULT);
    LINUX_TIMESPEC values[2];
    memcpy(values, (const void *)(ULONG_PTR)times, sizeof(values));
    for (UINT32 i = 0; i < 2; i++) {
        if (values[i].Nanoseconds != UTIME_NOW &&
            values[i].Nanoseconds != UTIME_OMIT &&
            (values[i].Nanoseconds < 0 || values[i].Nanoseconds >= 1000000000))
            return LinuxError(LINUX_EINVAL);
    }
    if (values[1].Nanoseconds == UTIME_OMIT) {
        char dos_path[260];
        UINT64 error = LinuxCapturePathAt(process, directory_fd, path, dos_path);
        if (error) return error;
        FAT_FIND_DATA info;
        NTSTATUS status = FatQueryPath(dos_path, &info);
        if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
            status == STATUS_OBJECT_PATH_NOT_FOUND || status == STATUS_NO_SUCH_FILE)
            return LinuxError(LINUX_ENOENT);
        return NT_SUCCESS(status) ? 0 : LinuxError(LINUX_EIO);
    }
    return linux_update_mtime(process, directory_fd, path, values[1].Seconds,
        values[1].Nanoseconds, FALSE,
        values[1].Nanoseconds == UTIME_NOW);
}

UINT64 LinuxFutimesAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                      UINT64 times)
{
    if (!times) return linux_update_mtime(process, directory_fd, path,
                                          0, 0, TRUE, TRUE);
    if (!MmProbeForRead(times, sizeof(LINUX_TIMEVAL) * 2))
        return LinuxError(LINUX_EFAULT);
    LINUX_TIMEVAL values[2];
    memcpy(values, (const void *)(ULONG_PTR)times, sizeof(values));
    if (values[1].Microseconds < 0 || values[1].Microseconds >= 1000000)
        return LinuxError(LINUX_EINVAL);
    return linux_update_mtime(process, directory_fd, path,
        values[1].Seconds, values[1].Microseconds, TRUE, FALSE);
}

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

UINT64 LinuxClockGetres(UINT64 clock_id, UINT64 destination)
{
    if (clock_id != 0 && clock_id != 1)
        return LinuxError(LINUX_EINVAL);
    if (!destination) return 0;
    if (!MmProbeForWrite(destination, sizeof(LINUX_TIMESPEC)))
        return LinuxError(LINUX_EFAULT);

    /* Both realtime and monotonic clocks advance from the 100 Hz PIT tick. */
    LINUX_TIMESPEC *resolution =
        (LINUX_TIMESPEC *)(ULONG_PTR)destination;
    resolution->Seconds = 0;
    resolution->Nanoseconds = 10000000;
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
