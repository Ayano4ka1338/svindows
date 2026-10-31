/*
 * ke/Dispatcher.c - waitable (dispatcher) objects and the wait machinery.
 *
 * Events, semaphores, mutants, and threads all embed a DISPATCHER_HEADER. A
 * thread waits by adding its wait block to the header's list and blocking; a
 * signal wakes every waiter to re-test the object (a simple, correct model: for
 * an auto-reset event or a semaphore, only as many waiters as there are signals
 * succeed, and the rest block again).
 *
 * Single-CPU: short critical sections just disable interrupts.
 */
#include <svindows/Ke.h>
#include <svindows/Ob.h>
#include <svindows/Rtl.h>
#include <nt/Ntstatus.h>

/* Try to satisfy a wait without blocking, consuming the signal as appropriate. */
static BOOLEAN KiCanAcquire(PDISPATCHER_HEADER h)
{
    switch (h->Type) {
    case EventNotificationObject:
    case TimerNotificationObject:
    case TimerSynchronizationObject:
    case ThreadObject:
    case EventSynchronizationObject:
    case SemaphoreObject:
        return (BOOLEAN)(h->SignalState > 0);
    case MutantObject: {
        PKMUTANT mutant = (PKMUTANT)h;
        return (BOOLEAN)(h->SignalState > 0 ||
                         mutant->OwnerThread == KeGetCurrentThread());
    }
    }
    return FALSE;
}

static NTSTATUS KiConsumeSignal(PDISPATCHER_HEADER h)
{
    switch (h->Type) {
    case EventNotificationObject:
    case TimerNotificationObject:
    case ThreadObject:
        return STATUS_SUCCESS; /* level-triggered, never consumed */
    case EventSynchronizationObject:
    case TimerSynchronizationObject:
        h->SignalState = 0;
        return STATUS_SUCCESS;
    case SemaphoreObject:
        h->SignalState--;
        return STATUS_SUCCESS;
    case MutantObject: {
        PKMUTANT mutant = (PKMUTANT)h;
        PKTHREAD current = KeGetCurrentThread();
        if (mutant->OwnerThread == current) {
            mutant->RecursionCount++;
            return STATUS_SUCCESS;
        }
        h->SignalState = 0;
        mutant->OwnerThread = current;
        mutant->RecursionCount = 1;
        InsertTailList(&current->OwnedMutantListHead, &mutant->OwnerEntry);
        ObReferenceObject((POBJECT)mutant);
        if (mutant->Abandoned) {
            mutant->Abandoned = FALSE;
            return STATUS_ABANDONED_WAIT_0;
        }
        return STATUS_SUCCESS;
    }
    }
    return STATUS_INVALID_PARAMETER;
}

/* Remove every outstanding wait block owned by a thread. Each entry is made a
 * self-list when detached so cancellation and signaling can safely race on the
 * single CPU without removing the same entry twice. */
static void KiUnlinkThreadWait(PKTHREAD thread)
{
    for (ULONG i = 0; i < thread->WaitBlockCount; i++) {
        PLIST_ENTRY entry = &thread->WaitBlocks[i].WaitListEntry;
        if (entry->Flink != entry) {
            RemoveEntryList(entry);
            InitializeListHead(entry);
        }
    }
}

static void KiWakeWaitingThread(PKTHREAD thread, NTSTATUS status)
{
    if (thread->State != ThreadStateWaiting)
        return;
    KiUnlinkThreadWait(thread);
    thread->WaitStatus = status;
    thread->WaitDeadline = 0;
    thread->WaitAlertable = FALSE;
    KiReadyThread(thread);
}

NTSTATUS KeQueueKernelApc(PKTHREAD thread, const KKERNEL_APC *apc)
{
    if (!thread || !apc || !apc->Routine)
        return STATUS_INVALID_PARAMETER;
    UINT64 flags = KiIrqSave();
    NTSTATUS status = STATUS_SUCCESS;
    if (thread->State == ThreadStateTerminated)
        status = STATUS_INVALID_PARAMETER;
    else if (thread->KernelApcCount == KE_MAXIMUM_KERNEL_APCS)
        status = STATUS_INSUFFICIENT_RESOURCES;
    else {
        UINT8 tail = (UINT8)((thread->KernelApcHead + thread->KernelApcCount) %
                             KE_MAXIMUM_KERNEL_APCS);
        thread->KernelApcs[tail] = *apc;
        thread->KernelApcCount++;
        if (thread->State == ThreadStateWaiting)
            KiWakeWaitingThread(thread, STATUS_SUCCESS);
    }
    KiIrqRestore(flags);
    return status;
}

void KiDeliverKernelApcs(void)
{
    if (KeGetCurrentIrql() > APC_LEVEL)
        return;
    PKTHREAD thread = KeGetCurrentThread();
    if (!thread || thread->State == ThreadStateTerminated)
        return;
    for (UINT32 delivered = 0; delivered < KE_MAXIMUM_KERNEL_APCS; ++delivered) {
        UINT64 flags = KiIrqSave();
        if (!thread->KernelApcCount) {
            KiIrqRestore(flags);
            break;
        }
        KKERNEL_APC apc = thread->KernelApcs[thread->KernelApcHead];
        thread->KernelApcHead = (UINT8)((thread->KernelApcHead + 1) %
                                        KE_MAXIMUM_KERNEL_APCS);
        thread->KernelApcCount--;
        KIRQL old_irql;
        KeRaiseIrql(APC_LEVEL, &old_irql);
        apc.Routine(apc.Context, apc.Argument1, apc.Argument2);
        KeLowerIrql(old_irql);
        KiIrqRestore(flags);
    }
}

NTSTATUS KeQueueUserApc(PKTHREAD thread, const KUSER_APC *apc)
{
    if (!thread || !apc || !apc->Routine || !thread->UserMode)
        return STATUS_INVALID_PARAMETER;
    UINT64 flags = KiIrqSave();
    NTSTATUS status = STATUS_SUCCESS;
    if (thread->State == ThreadStateTerminated ||
        (thread->Process && thread->Process->Exiting))
        status = STATUS_INVALID_PARAMETER;
    else if (thread->UserApcCount == KE_MAXIMUM_USER_APCS)
        status = STATUS_INSUFFICIENT_RESOURCES;
    else {
        UINT8 tail = (UINT8)((thread->UserApcHead + thread->UserApcCount) %
                             KE_MAXIMUM_USER_APCS);
        thread->UserApcs[tail] = *apc;
        thread->UserApcCount++;
        if (thread->State == ThreadStateWaiting && thread->WaitAlertable)
            KiWakeWaitingThread(thread, STATUS_USER_APC);
    }
    KiIrqRestore(flags);
    return status;
}

NTSTATUS KeDequeueUserApc(PKTHREAD thread, PKUSER_APC apc)
{
    if (!thread || !apc)
        return STATUS_INVALID_PARAMETER;
    UINT64 flags = KiIrqSave();
    if (!thread->UserApcCount) {
        KiIrqRestore(flags);
        return STATUS_NOT_FOUND;
    }
    *apc = thread->UserApcs[thread->UserApcHead];
    thread->UserApcHead = (UINT8)((thread->UserApcHead + 1) %
                                  KE_MAXIMUM_USER_APCS);
    thread->UserApcCount--;
    KiIrqRestore(flags);
    return STATUS_SUCCESS;
}

BOOLEAN KeHasUserApc(PKTHREAD thread)
{
    if (!thread) return FALSE;
    UINT64 flags = KiIrqSave();
    BOOLEAN pending = (BOOLEAN)(thread->UserApcCount != 0);
    KiIrqRestore(flags);
    return pending;
}

/* Wake every thread waiting on this header so it can re-test the object. */
void KiSignalObject(PDISPATCHER_HEADER h)
{
    while (!IsListEmpty(&h->WaitListHead)) {
        PLIST_ENTRY e = RemoveHeadList(&h->WaitListHead);
        InitializeListHead(e);
        PKWAIT_BLOCK wb = CONTAINING_RECORD(e, KWAIT_BLOCK, WaitListEntry);
        KiWakeWaitingThread(wb->Thread, STATUS_SUCCESS);
    }
}

void KiTimeoutThreadWait(PKTHREAD thread)
{
    KiWakeWaitingThread(thread, STATUS_TIMEOUT);
}

void KiCancelThreadWait(PKTHREAD thread, NTSTATUS status)
{
    KiUnlinkThreadWait(thread);
    thread->WaitStatus = status;
    thread->WaitDeadline = 0;
    thread->WaitBlockCount = 0;
    thread->WaitAlertable = FALSE;
}

NTSTATUS KeWaitForMultipleObjectsAlertable(ULONG count,
                                          PDISPATCHER_HEADER *objects,
                                          BOOLEAN wait_all, UINT64 timeout_ticks,
                                          BOOLEAN alertable)
{
    if (KeGetCurrentIrql() > DISPATCH_LEVEL ||
        (timeout_ticks != 0 && KeGetCurrentIrql() > APC_LEVEL))
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "wait at elevated IRQL");
    if (!count || count > KE_MAXIMUM_WAIT_OBJECTS || !objects)
        return STATUS_INVALID_PARAMETER;
    /* Duplicate wait-all entries would consume a semaphore or auto-reset
     * event twice after checking only one signal. Reject ambiguous lists. */
    for (ULONG i = 0; i < count; i++) {
        if (!objects[i])
            return STATUS_INVALID_PARAMETER;
        for (ULONG j = 0; j < i; j++)
            if (objects[i] == objects[j])
                return STATUS_INVALID_PARAMETER;
    }

    UINT64 flags = KiIrqSave();
    PKTHREAD thread = KeGetCurrentThread();
    UINT64 now = KeGetTickCount();
    UINT64 deadline = timeout_ticks == ~(UINT64)0 ? 0 :
        (timeout_ticks > ~(UINT64)0 - now ? ~(UINT64)0 : now + timeout_ticks);

    for (;;) {
        if (alertable && thread && thread->UserApcCount) {
            KiIrqRestore(flags);
            return STATUS_USER_APC;
        }
        if (wait_all) {
            BOOLEAN ready = TRUE;
            for (ULONG i = 0; i < count; i++)
                if (!KiCanAcquire(objects[i])) {
                    ready = FALSE;
                    break;
                }
            if (ready) {
                NTSTATUS result = STATUS_WAIT_0;
                for (ULONG i = 0; i < count; i++)
                    if (KiConsumeSignal(objects[i]) ==
                        STATUS_ABANDONED_WAIT_0)
                        result = STATUS_ABANDONED_WAIT_0 + i;
                KiIrqRestore(flags);
                return result;
            }
        } else {
            for (ULONG i = 0; i < count; i++) {
                if (KiCanAcquire(objects[i])) {
                    NTSTATUS consumed = KiConsumeSignal(objects[i]);
                    KiIrqRestore(flags);
                    if (consumed == STATUS_ABANDONED_WAIT_0)
                        return STATUS_ABANDONED_WAIT_0 + i;
                    return (NTSTATUS)(STATUS_WAIT_0 + i);
                }
            }
        }

        if (timeout_ticks == 0 || (deadline && KeGetTickCount() >= deadline)) {
            KiIrqRestore(flags);
            return STATUS_TIMEOUT;
        }

        thread->WaitBlockCount = count;
        thread->WaitStatus = STATUS_PENDING;
        thread->WaitDeadline = deadline;
        thread->WaitAlertable = alertable;
        for (ULONG i = 0; i < count; i++) {
            PKWAIT_BLOCK block = &thread->WaitBlocks[i];
            InitializeListHead(&block->WaitListEntry);
            block->Thread = thread;
            block->Object = objects[i];
            InsertTailList(&objects[i]->WaitListHead, &block->WaitListEntry);
        }
        thread->State = ThreadStateWaiting;
        KiBlockCurrentThread(); /* reschedule; returns once signalled */

        /* Signaling and timeout paths normally unlink all blocks before making
         * us ready. This is also safe as defensive cleanup. */
        KiUnlinkThreadWait(thread);
        thread->WaitAlertable = FALSE;
        if (thread->WaitStatus == STATUS_USER_APC) {
            thread->WaitBlockCount = 0;
            KiIrqRestore(flags);
            return STATUS_USER_APC;
        }
        if (thread->WaitStatus == STATUS_TIMEOUT) {
            thread->WaitBlockCount = 0;
            KiIrqRestore(flags);
            return STATUS_TIMEOUT;
        }
        thread->WaitBlockCount = 0;
    }
}

NTSTATUS KeWaitForMultipleObjects(ULONG count, PDISPATCHER_HEADER *objects,
                                  BOOLEAN wait_all, UINT64 timeout_ticks)
{
    return KeWaitForMultipleObjectsAlertable(count, objects, wait_all,
                                             timeout_ticks, FALSE);
}

NTSTATUS KeWaitForSingleObjectTimeoutAlertable(PDISPATCHER_HEADER h,
                                               UINT64 timeout_ticks,
                                               BOOLEAN alertable)
{
    PDISPATCHER_HEADER objects[1] = { h };
    return KeWaitForMultipleObjectsAlertable(1, objects, FALSE,
                                             timeout_ticks, alertable);
}

NTSTATUS KeWaitForSingleObjectTimeout(PDISPATCHER_HEADER h,
                                      UINT64 timeout_ticks)
{
    return KeWaitForSingleObjectTimeoutAlertable(h, timeout_ticks, FALSE);
}

NTSTATUS KeWaitForSingleObject(PDISPATCHER_HEADER h)
{
    return KeWaitForSingleObjectTimeout(h, ~(UINT64)0);
}

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

void KeInitializeEvent(PKEVENT event, BOOLEAN notification, BOOLEAN signaled)
{
    event->Header.Type = notification ? EventNotificationObject
                                      : EventSynchronizationObject;
    event->Header.SignalState = signaled ? 1 : 0;
    InitializeListHead(&event->Header.WaitListHead);
}

LONG KeSetEvent(PKEVENT event)
{
    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "event signal above DISPATCH_LEVEL");
    UINT64 flags = KiIrqSave();
    LONG previous = event->Header.SignalState;
    event->Header.SignalState = 1;
    KiSignalObject(&event->Header);
    KiIrqRestore(flags);
    return previous;
}

void KeResetEvent(PKEVENT event)
{
    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "event reset above DISPATCH_LEVEL");
    UINT64 flags = KiIrqSave();
    event->Header.SignalState = 0;
    KiIrqRestore(flags);
}

/* ------------------------------------------------------------------ */
/* Semaphores                                                         */
/* ------------------------------------------------------------------ */

void KeInitializeSemaphore(PKSEMAPHORE sem, LONG initial, LONG limit)
{
    sem->Header.Type = SemaphoreObject;
    sem->Header.SignalState = initial;
    sem->Limit = limit;
    InitializeListHead(&sem->Header.WaitListHead);
}

LONG KeReleaseSemaphore(PKSEMAPHORE sem, LONG count)
{
    UINT64 flags = KiIrqSave();
    LONG previous = sem->Header.SignalState;
    sem->Header.SignalState += count;
    if (sem->Limit && sem->Header.SignalState > sem->Limit)
        sem->Header.SignalState = sem->Limit;
    KiSignalObject(&sem->Header);
    KiIrqRestore(flags);
    return previous;
}

/* ------------------------------------------------------------------ */
/* Mutants (mutexes): owner, recursion, and abandonment are kernel state. */
/* ------------------------------------------------------------------ */

void KeInitializeMutant(PKMUTANT mutant, BOOLEAN initially_owned)
{
    mutant->Header.Type = MutantObject;
    mutant->Header.SignalState = initially_owned ? 0 : 1;
    InitializeListHead(&mutant->Header.WaitListHead);
    InitializeListHead(&mutant->OwnerEntry);
    mutant->OwnerThread = initially_owned ? KeGetCurrentThread() : NULL;
    mutant->RecursionCount = initially_owned ? 1 : 0;
    mutant->Abandoned = FALSE;
    if (initially_owned) {
        InsertTailList(&mutant->OwnerThread->OwnedMutantListHead,
                       &mutant->OwnerEntry);
        ObReferenceObject((POBJECT)mutant);
    }
}

BOOLEAN KeReleaseMutant(PKMUTANT mutant, LONG *previous_count)
{
    UINT64 flags = KiIrqSave();
    if (mutant->OwnerThread != KeGetCurrentThread()) {
        KiIrqRestore(flags);
        return FALSE;
    }
    if (previous_count)
        *previous_count = mutant->RecursionCount;
    if (--mutant->RecursionCount == 0) {
        mutant->OwnerThread = NULL;
        mutant->Header.SignalState = 1;
        RemoveEntryList(&mutant->OwnerEntry);
        InitializeListHead(&mutant->OwnerEntry);
        KiSignalObject(&mutant->Header);
        ObDereferenceObject((POBJECT)mutant);
    }
    KiIrqRestore(flags);
    return TRUE;
}

void KeAbandonThreadMutants(PKTHREAD thread)
{
    while (!IsListEmpty(&thread->OwnedMutantListHead)) {
        PLIST_ENTRY entry = RemoveHeadList(&thread->OwnedMutantListHead);
        PKMUTANT mutant = CONTAINING_RECORD(entry, KMUTANT, OwnerEntry);
        InitializeListHead(&mutant->OwnerEntry);
        mutant->OwnerThread = NULL;
        mutant->RecursionCount = 0;
        mutant->Abandoned = TRUE;
        mutant->Header.SignalState = 1;
        KiSignalObject(&mutant->Header);
        ObDereferenceObject((POBJECT)mutant);
    }
}
