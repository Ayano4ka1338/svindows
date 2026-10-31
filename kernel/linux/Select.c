/* select(2)/pselect6(2) over the same descriptor readiness as poll(2). */
#include "Internal.h"
#include <svindows/Rtl.h>

typedef struct _LINUX_SELECT_TIME {
    INT64 Seconds, Fraction;
} LINUX_SELECT_TIME;

static UINT64 linux_select_bits(UINT64 user, UINT64 *bits, UINT64 bytes)
{
    memset(bits, 0, LINUX_MAX_FDS / 8);
    if (!user || !bytes) return 0;
    if (!MmProbeForRead(user, bytes) || !MmProbeForWrite(user, bytes))
        return LinuxError(LINUX_EFAULT);
    memcpy(bits, (const void *)(ULONG_PTR)user, bytes);
    return 0;
}

UINT64 LinuxSelect(PKPROCESS process, UINT64 nfds, UINT64 read_set,
                   UINT64 write_set, UINT64 except_set, UINT64 timeout,
                   BOOLEAN pselect)
{
    if (nfds > LINUX_MAX_FDS) return LinuxError(LINUX_EINVAL);
    UINT64 set_bytes = ((nfds + 63) / 64) * sizeof(UINT64);
    UINT64 wanted_read[LINUX_MAX_FDS / 64];
    UINT64 wanted_write[LINUX_MAX_FDS / 64];
    UINT64 wanted_except[LINUX_MAX_FDS / 64];
    UINT64 ready_read[LINUX_MAX_FDS / 64];
    UINT64 ready_write[LINUX_MAX_FDS / 64];
    UINT64 ready_except[LINUX_MAX_FDS / 64];
    UINT64 error = linux_select_bits(read_set, wanted_read, set_bytes);
    if (error) return error;
    error = linux_select_bits(write_set, wanted_write, set_bytes);
    if (error) return error;
    error = linux_select_bits(except_set, wanted_except, set_bytes);
    if (error) return error;

    for (UINT32 fd = 0; fd < nfds; fd++) {
        UINT64 bit = 1ULL << (fd % 64);
        UINT32 word = fd / 64;
        if (((wanted_read[word] | wanted_write[word] |
              wanted_except[word]) & bit) &&
            (fd >= LINUX_MAX_FDS ||
             !process->LinuxContext->Files[fd].Used))
            return LinuxError(LINUX_EBADF);
    }

    UINT64 ticks = ~(UINT64)0;
    if (timeout) {
        if (!MmProbeForRead(timeout, sizeof(LINUX_SELECT_TIME)) ||
            (!pselect && !MmProbeForWrite(timeout, sizeof(LINUX_SELECT_TIME))))
            return LinuxError(LINUX_EFAULT);
        LINUX_SELECT_TIME time = *(const LINUX_SELECT_TIME *)(ULONG_PTR)timeout;
        INT64 limit = pselect ? 1000000000LL : 1000000LL;
        if (time.Seconds < 0 || time.Fraction < 0 || time.Fraction >= limit)
            return LinuxError(LINUX_EINVAL);
        UINT64 ms = (UINT64)time.Seconds * 1000 +
                    ((UINT64)time.Fraction + limit / 1000 - 1) /
                    (limit / 1000);
        ticks = (ms + 9) / 10;
    }
    UINT64 start = KeGetTickCount();
    for (;;) {
        memset(ready_read, 0, sizeof(ready_read));
        memset(ready_write, 0, sizeof(ready_write));
        memset(ready_except, 0, sizeof(ready_except));
        UINT32 count = 0;
        for (UINT32 fd = 0; fd < nfds; fd++) {
            UINT32 word = fd / 64;
            UINT64 bit = 1ULL << (fd % 64);
            if (!((wanted_read[word] | wanted_write[word] |
                   wanted_except[word]) & bit)) continue;
            UINT32 ready = LinuxPollDescriptor(process, fd);
            BOOLEAN hit = FALSE;
            if ((wanted_read[word] & bit) && (ready & (1u | 8u | 16u))) {
                ready_read[word] |= bit;
                hit = TRUE;
            }
            if ((wanted_write[word] & bit) && (ready & (4u | 8u | 16u))) {
                ready_write[word] |= bit;
                hit = TRUE;
            }
            if ((wanted_except[word] & bit) && (ready & 2u)) {
                ready_except[word] |= bit;
                hit = TRUE;
            }
            if (hit) count++;
        }
        UINT64 elapsed = KeGetTickCount() - start;
        if (count || ticks == 0 || (ticks != ~(UINT64)0 && elapsed >= ticks)) {
            if (read_set && set_bytes)
                memcpy((void *)(ULONG_PTR)read_set, ready_read, set_bytes);
            if (write_set && set_bytes)
                memcpy((void *)(ULONG_PTR)write_set, ready_write, set_bytes);
            if (except_set && set_bytes)
                memcpy((void *)(ULONG_PTR)except_set, ready_except, set_bytes);
            if (timeout && !pselect) {
                UINT64 remaining = ticks == ~(UINT64)0 || elapsed >= ticks ?
                                   0 : (ticks - elapsed) * 10;
                LINUX_SELECT_TIME *time = (void *)(ULONG_PTR)timeout;
                time->Seconds = (INT64)(remaining / 1000);
                time->Fraction = (INT64)((remaining % 1000) * 1000);
            }
            return count;
        }
        KEVENT event;
        KeInitializeEvent(&event, FALSE, FALSE);
        KeWaitForSingleObjectTimeout(&event.Header, 1);
    }
}
