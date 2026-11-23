/* Shared-address-space futex wait/wake over kernel events. */
#include "Internal.h"

#define LINUX_FUTEX_WAIT 0u
#define LINUX_FUTEX_WAKE 1u
#define LINUX_FUTEX_WAIT_BITSET 9u
#define LINUX_FUTEX_WAKE_BITSET 10u
#define LINUX_FUTEX_PRIVATE_FLAG 0x80u
#define LINUX_FUTEX_CLOCK_REALTIME 0x100u
#define LINUX_FUTEX_BITSET_MATCH_ANY 0xffffffffu


typedef struct _LINUX_TIMESPEC {
    INT64 Seconds;
    INT64 Nanoseconds;
} LINUX_TIMESPEC;

static UINT64 linux_futex_wake_bitset(PKPROCESS process, UINT64 address,
                                      UINT32 maximum, UINT32 bitset)
{
    UINT32 count = 0;
    UINT64 irq = KiIrqSave();
    for (PLIST_ENTRY entry = process->ThreadListHead.Flink;
         entry != &process->ThreadListHead && count < maximum;
         entry = entry->Flink) {
        PKTHREAD thread = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
        LINUX_THREAD_CONTEXT *context = thread->LinuxThread;
        if (!context || thread->State != ThreadStateWaiting ||
            context->WaitingFutexAddress != address ||
            !(context->WaitingFutexBitset & bitset))
            continue;
        context->WaitingFutexAddress = 0;
        context->WaitingFutexBitset = 0;
        KeSetEvent(&context->FutexEvent);
        count++;
    }
    KiIrqRestore(irq);
    return count;
}

UINT64 LinuxFutexWake(PKPROCESS process, UINT64 address, UINT32 maximum)
{
    return linux_futex_wake_bitset(process, address, maximum,
                                   LINUX_FUTEX_BITSET_MATCH_ANY);
}

UINT64 LinuxFutex(PKPROCESS process, UINT64 address, UINT64 operation,
                  UINT64 value, UINT64 timeout, UINT64 bitset)
{
    static UINT32 trace_count;
    BOOLEAN trace = process->ProcessId == 36 && trace_count < 100;
    if (trace) trace_count++;
    if (address & 3u || !MmProbeForRead(address, sizeof(UINT32)))
        return LinuxError(LINUX_EFAULT);
    UINT64 command = operation & ~(LINUX_FUTEX_PRIVATE_FLAG |
                                   LINUX_FUTEX_CLOCK_REALTIME);
    if (command == LINUX_FUTEX_WAKE || command == LINUX_FUTEX_WAKE_BITSET) {
        UINT32 wake_bits = command == LINUX_FUTEX_WAKE ?
                           LINUX_FUTEX_BITSET_MATCH_ANY : (UINT32)bitset;
        if (!wake_bits) return LinuxError(LINUX_EINVAL);
        UINT64 woke = linux_futex_wake_bitset(process, address,
            value > ~(UINT32)0 ? ~(UINT32)0 : (UINT32)value, wake_bits);
        if (trace) KeLog("[futex-trace] op=%llx addr=%llx wake=%llu\n",
                         (unsigned long long)operation,
                         (unsigned long long)address,
                         (unsigned long long)woke);
        return woke;
    }
    if (command != LINUX_FUTEX_WAIT && command != LINUX_FUTEX_WAIT_BITSET)
        return LinuxError(LINUX_ENOSYS);
    BOOLEAN absolute = command == LINUX_FUTEX_WAIT_BITSET;
    if (absolute && !bitset) return LinuxError(LINUX_EINVAL);

    UINT64 ticks = ~(UINT64)0;
    if (timeout) {
        if (!MmProbeForRead(timeout, sizeof(LINUX_TIMESPEC)))
            return LinuxError(LINUX_EFAULT);
        const LINUX_TIMESPEC *time = (const void *)(ULONG_PTR)timeout;
        if (time->Seconds < 0 || time->Nanoseconds < 0 ||
            time->Nanoseconds >= 1000000000)
            return LinuxError(LINUX_EINVAL);
        if (absolute) {
            if ((UINT64)time->Seconds > (~(UINT64)0 -
                (UINT64)time->Nanoseconds) / 1000000000ULL)
                ticks = 0;
            else {
                UINT64 deadline = (UINT64)time->Seconds * 1000000000ULL +
                                  (UINT64)time->Nanoseconds;
                UINT64 now;
                if (operation & LINUX_FUTEX_CLOCK_REALTIME) {
                    UINT64 filetime = KeQuerySystemTimeFileTime();
                    now = filetime > 116444736000000000ULL ?
                          (filetime - 116444736000000000ULL) * 100ULL : 0;
                } else {
                    now = KeGetTickCount() * 10000000ULL;
                }
                ticks = deadline <= now ? 0 :
                    (deadline - now + 9999999ULL) / 10000000ULL;
            }
        } else if ((UINT64)time->Seconds > (~(UINT64)0 - 100) / 100)
            ticks = ~(UINT64)0 - 1;
        else
            ticks = (UINT64)time->Seconds * 100 +
                    ((UINT64)time->Nanoseconds + 9999999) / 10000000;
    }

    PKTHREAD thread = KeGetCurrentThread();
    LINUX_THREAD_CONTEXT *context = thread->LinuxThread;
    UINT64 irq = KiIrqSave();
    if (*(volatile UINT32 *)(ULONG_PTR)address != (UINT32)value) {
        UINT32 current = *(volatile UINT32 *)(ULONG_PTR)address;
        KiIrqRestore(irq);
        if (trace) KeLog("[futex-trace] op=%llx addr=%llx expected=%u current=%u -> EAGAIN\n",
                         (unsigned long long)operation,
                         (unsigned long long)address, (UINT32)value, current);
        return LinuxError(LINUX_EAGAIN);
    }
    KeResetEvent(&context->FutexEvent);
    context->WaitingFutexAddress = address;
    context->WaitingFutexBitset = absolute ? (UINT32)bitset :
                                  LINUX_FUTEX_BITSET_MATCH_ANY;
    KiIrqRestore(irq);
    /* Keep the wait address published until the waiter resumes.  Wakers
     * serialize through the same IRQ lock and signal the event, so a wake
     * between restore and wait is not lost. */
    NTSTATUS status = KeWaitForSingleObjectTimeout(
        &context->FutexEvent.Header, ticks);
    irq = KiIrqSave();
    context->WaitingFutexAddress = 0;
    context->WaitingFutexBitset = 0;
    KiIrqRestore(irq);
    if (trace) KeLog("[futex-trace] op=%llx addr=%llx expected=%u ticks=%llu status=%x\n",
                     (unsigned long long)operation,
                     (unsigned long long)address, (UINT32)value,
                     (unsigned long long)ticks, (unsigned)status);
    return status == STATUS_TIMEOUT ? LinuxError(LINUX_ETIMEDOUT) :
           NT_SUCCESS(status) ? 0 : LinuxError(LINUX_EINVAL);
}
