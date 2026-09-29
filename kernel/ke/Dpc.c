/* Single-CPU FIFO DPC queue. PIC masks IRQs at DISPATCH_LEVEL, so callbacks
 * must be short and nonblocking. A bounded drain prevents requeue starvation. */
#include <svindows/Ke.h>
#include <svindows/Rtl.h>

static LIST_ENTRY g_dpc_queue = { &g_dpc_queue, &g_dpc_queue };
static BOOLEAN g_dpc_running;
#define KI_DPC_DRAIN_BUDGET 64

BOOLEAN KiIsExecutingDpc(void) { return g_dpc_running; }

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
    UINT64 flags = KiIrqSave();
    if (dpc->Queued) {
        KiIrqRestore(flags);
        return FALSE; /* never replace the arguments of an existing request */
    }
    dpc->Argument1 = arg1;
    dpc->Argument2 = arg2;
    dpc->Queued = TRUE;
    InsertTailList(&g_dpc_queue, &dpc->Entry);
    KiIrqRestore(flags);
    return TRUE;
}

BOOLEAN KeRemoveQueueDpc(PKDPC dpc)
{
    if (!dpc)
        return FALSE;
    UINT64 flags = KiIrqSave();
    BOOLEAN removed = dpc->Queued;
    if (removed) {
        RemoveEntryList(&dpc->Entry);
        InitializeListHead(&dpc->Entry);
        dpc->Queued = FALSE;
    }
    KiIrqRestore(flags);
    return removed; /* does not cancel a callback which is already executing */
}

void KiRetireDpcList(void)
{
    if (KeGetCurrentIrql() != DISPATCH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "DPC drain outside DISPATCH_LEVEL");
    if (g_dpc_running)
        return;
    g_dpc_running = TRUE;
    for (ULONG count = 0; count < KI_DPC_DRAIN_BUDGET &&
         !IsListEmpty(&g_dpc_queue); count++) {
        PKDPC dpc = CONTAINING_RECORD(RemoveHeadList(&g_dpc_queue), KDPC, Entry);
        PKDEFERRED_ROUTINE routine = dpc->Routine;
        PVOID context = dpc->Context, arg1 = dpc->Argument1, arg2 = dpc->Argument2;
        InitializeListHead(&dpc->Entry);
        dpc->Queued = FALSE;
        routine(dpc, context, arg1, arg2);
        /* Do not touch dpc after the call: it may have been freed or requeued. */
        if (KeGetCurrentIrql() != DISPATCH_LEVEL)
            KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "DPC leaked execution level");
    }
    g_dpc_running = FALSE;
}
