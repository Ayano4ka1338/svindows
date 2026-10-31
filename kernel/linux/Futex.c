/* Shared-address-space futex wait/wake over kernel events. */
#include "Internal.h"

#define LINUX_FUTEX_WAIT 0u
#define LINUX_FUTEX_WAKE 1u
#define LINUX_FUTEX_PRIVATE_FLAG 0x80u

typedef struct _LINUX_TIMESPEC {
    INT64 Seconds;
    INT64 Nanoseconds;
} LINUX_TIMESPEC;

UINT64 LinuxFutexWake(PKPROCESS process, UINT64 address, UINT32 maximum)
{
    UINT32 count = 0;
    UINT64 irq = KiIrqSave();
    for (PLIST_ENTRY entry = process->ThreadListHead.Flink;
         entry != &process->ThreadListHead && count < maximum;
         entry = entry->Flink) {
        PKTHREAD thread = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
        LINUX_THREAD_CONTEXT *context = thread->LinuxThread;
        if (!context || thread->State != ThreadStateWaiting ||
            context->WaitingFutexAddress != address)
            continue;
        context->WaitingFutexAddress = 0;
        KeSetEvent(&context->FutexEvent);
        count++;
    }
    KiIrqRestore(irq);
    return count;
}

UINT64 LinuxFutex(PKPROCESS process, UINT64 address, UINT64 operation,
                  UINT64 value, UINT64 timeout)
{
    if (address & 3u || !MmProbeForRead(address, sizeof(UINT32)))
        return LinuxError(LINUX_EFAULT);
    UINT64 command = operation & ~LINUX_FUTEX_PRIVATE_FLAG;
    if (command == LINUX_FUTEX_WAKE)
        return LinuxFutexWake(process, address,
                              value > ~(UINT32)0 ? ~(UINT32)0 : (UINT32)value);
    if (command != LINUX_FUTEX_WAIT)
        return LinuxError(LINUX_ENOSYS);

    UINT64 ticks = ~(UINT64)0;
    if (timeout) {
        if (!MmProbeForRead(timeout, sizeof(LINUX_TIMESPEC)))
            return LinuxError(LINUX_EFAULT);
        const LINUX_TIMESPEC *time = (const void *)(ULONG_PTR)timeout;
        if (time->Seconds < 0 || time->Nanoseconds < 0 ||
            time->Nanoseconds >= 1000000000)
            return LinuxError(LINUX_EINVAL);
        if ((UINT64)time->Seconds > (~(UINT64)0 - 100) / 100)
            ticks = ~(UINT64)0 - 1;
        else
            ticks = (UINT64)time->Seconds * 100 +
                    ((UINT64)time->Nanoseconds + 9999999) / 10000000;
    }

    PKTHREAD thread = KeGetCurrentThread();
    LINUX_THREAD_CONTEXT *context = thread->LinuxThread;
    UINT64 irq = KiIrqSave();
    if (*(volatile UINT32 *)(ULONG_PTR)address != (UINT32)value) {
        KiIrqRestore(irq);
        return LinuxError(LINUX_EAGAIN);
    }
    KeResetEvent(&context->FutexEvent);
    context->WaitingFutexAddress = address;
    KiIrqRestore(irq);
    /* Keep the wait address published until the waiter resumes.  Wakers
     * serialize through the same IRQ lock and signal the event, so a wake
     * between restore and wait is not lost. */
    NTSTATUS status = KeWaitForSingleObjectTimeout(
        &context->FutexEvent.Header, ticks);
    irq = KiIrqSave();
    context->WaitingFutexAddress = 0;
    KiIrqRestore(irq);
    return status == STATUS_TIMEOUT ? LinuxError(LINUX_ETIMEDOUT) :
           NT_SUCCESS(status) ? 0 : LinuxError(LINUX_EINVAL);
}
