/* Shared FIFO DPC queue. Dispatch ownership is per CPU; the queue lock
 * serializes publication/removal while callbacks can run concurrently. */
#include <svindows/Ke.h>
#include <svindows/Rtl.h>

static LIST_ENTRY g_dpc_queue = { &g_dpc_queue, &g_dpc_queue };
static KSPIN_LOCK g_dpc_queue_lock;
static KSPIN_LOCK g_dpc_dispatch_lock;
#define KI_DPC_DRAIN_BUDGET 64

BOOLEAN KiIsExecutingDpc(void)
{ return KeGetCurrentProcessorBlock()->DpcRunning; }

static UINT64 KiAcquireDpcQueueLock(void)
{
    return KiAcquireRawSpinLock(&g_dpc_queue_lock);
}

static void KiReleaseDpcQueueLock(UINT64 interrupt_state)
{
    KiReleaseRawSpinLock(&g_dpc_queue_lock, interrupt_state);
}

void KeInitializeDpc(PKDPC dpc, PKDEFERRED_ROUTINE routine, PVOID context)
{
    InitializeListHead(&dpc->Entry);
    dpc->Routine = routine;
    dpc->Context = context;
    dpc->Argument1 = dpc->Argument2 = NULL;
    dpc->Queued = FALSE;
}

BOOLEAN KeInsertQueueDpc(PKDPC dpc, PVOID arg1, PVOID arg2)
{
    if (!dpc || !dpc->Routine)
        return FALSE;
    UINT64 interrupt_state = KiAcquireDpcQueueLock();
    if (dpc->Queued) {
        KiReleaseDpcQueueLock(interrupt_state);
        return FALSE; /* never replace the arguments of an existing request */
    }
    dpc->Argument1 = arg1;
    dpc->Argument2 = arg2;
    dpc->Queued = TRUE;
    InsertTailList(&g_dpc_queue, &dpc->Entry);
    KiReleaseDpcQueueLock(interrupt_state);
    return TRUE;
}

BOOLEAN KeRemoveQueueDpc(PKDPC dpc)
{
    if (!dpc)
        return FALSE;
    UINT64 interrupt_state = KiAcquireDpcQueueLock();
    BOOLEAN removed = dpc->Queued;
    if (removed) {
        RemoveEntryList(&dpc->Entry);
        InitializeListHead(&dpc->Entry);
        dpc->Queued = FALSE;
    }
    KiReleaseDpcQueueLock(interrupt_state);
    return removed; /* does not cancel a callback which is already executing */
}

void KiRetireDpcList(void)
{
    if (KeGetCurrentIrql() != DISPATCH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "DPC drain outside DISPATCH_LEVEL");
    PKPRCB processor = KeGetCurrentProcessorBlock();
    if (processor->DpcRunning)
        return;
    UINT64 dispatch_state = KiAcquireRawSpinLock(&g_dpc_dispatch_lock);
    processor->DpcRunning = TRUE;
    for (ULONG count = 0; count < KI_DPC_DRAIN_BUDGET; count++) {
        UINT64 interrupt_state = KiAcquireDpcQueueLock();
        if (IsListEmpty(&g_dpc_queue)) {
            KiReleaseDpcQueueLock(interrupt_state);
            break;
        }
        PKDPC dpc = CONTAINING_RECORD(RemoveHeadList(&g_dpc_queue), KDPC, Entry);
        PKDEFERRED_ROUTINE routine = dpc->Routine;
        PVOID context = dpc->Context, arg1 = dpc->Argument1, arg2 = dpc->Argument2;
        InitializeListHead(&dpc->Entry);
        dpc->Queued = FALSE;
        KiReleaseDpcQueueLock(interrupt_state);
        routine(dpc, context, arg1, arg2);
        /* Do not touch dpc after the call: it may have been freed or requeued. */
        if (KeGetCurrentIrql() != DISPATCH_LEVEL)
            KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "DPC leaked execution level");
    }
    processor->DpcRunning = FALSE;
    KiReleaseRawSpinLock(&g_dpc_dispatch_lock, dispatch_state);
}
