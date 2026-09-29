/* Resident UP dispatcher timers, ordered by tick deadline. Absolute times
 * currently use the shared since-boot clock, not a real NT wall clock. */
#include <svindows/Ke.h>
#include <svindows/Rtl.h>

static LIST_ENTRY g_timers = { &g_timers, &g_timers };

UINT64 KeDueTimeToTicks(INT64 due)
{
    UINT64 units = due < 0 ? 0 - (UINT64)due : (UINT64)due;
    UINT64 ticks = units / KE_TIME_UNITS_PER_TICK +
                   (units % KE_TIME_UNITS_PER_TICK != 0);
    if (due < 0) return ticks;
    UINT64 now = KeGetTickCount();
    return ticks > now ? ticks - now : 0;
}

static UINT64 KepAddTicks(UINT64 now, UINT64 delta)
{ return delta > ~(UINT64)0 - now ? ~(UINT64)0 : now + delta; }

static void KepInsertTimer(PKTIMER timer)
{
    PLIST_ENTRY entry = g_timers.Flink;
    while (entry != &g_timers &&
           CONTAINING_RECORD(entry, KTIMER, Entry)->DueTick <= timer->DueTick)
        entry = entry->Flink;
    timer->Entry.Flink = entry;
    timer->Entry.Blink = entry->Blink;
    entry->Blink->Flink = &timer->Entry;
    entry->Blink = &timer->Entry;
    timer->Inserted = TRUE;
}

static void KepDetachTimer(PKTIMER timer)
{
    if (timer->Inserted) RemoveEntryList(&timer->Entry);
    InitializeListHead(&timer->Entry);
    timer->Inserted = FALSE;
}

void KeInitializeTimerEx(PKTIMER timer, BOOLEAN notification)
{
    timer->Header.Type = notification ? TimerNotificationObject : TimerSynchronizationObject;
    timer->Header.SignalState = 0;
    InitializeListHead(&timer->Header.WaitListHead);
    InitializeListHead(&timer->Entry);
    timer->Inserted = FALSE;
    timer->DueTick = timer->PeriodTicks = 0;
    timer->Dpc = NULL;
    timer->StackOwner = NULL;
}

static void KepFireTimer(PKTIMER timer, UINT64 now)
{
    KepDetachTimer(timer);
    if (timer->PeriodTicks && now != ~(UINT64)0) {
        /* Skip missed periods without producing a catch-up callback storm. */
        UINT64 delta = timer->PeriodTicks - (now - timer->DueTick) % timer->PeriodTicks;
        timer->DueTick = KepAddTicks(now, delta);
        KepInsertTimer(timer);
    }
    timer->Header.SignalState = 1;
    KiSignalObject(&timer->Header);
    if (timer->Dpc) {
        UINT64 time = now * KE_TIME_UNITS_PER_TICK;
        KeInsertQueueDpc(timer->Dpc, (PVOID)(ULONG_PTR)(ULONG)time,
                         (PVOID)(ULONG_PTR)(ULONG)(time >> 32));
    }
}

BOOLEAN KeSetTimerEx(PKTIMER timer, INT64 due, ULONG period_ms, PKDPC dpc)
{
    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "timer set above DISPATCH_LEVEL");
    UINT64 flags = KiIrqSave();
    BOOLEAN previous = timer->Inserted;
    KepDetachTimer(timer);
    timer->Header.SignalState = 0;
    timer->PeriodTicks = period_ms / 10 + (period_ms % 10 != 0);
    timer->Dpc = dpc;
    UINT64 now = KeGetTickCount();
    timer->DueTick = KepAddTicks(now, KeDueTimeToTicks(due));
    if (timer->DueTick <= now) KepFireTimer(timer, now);
    else KepInsertTimer(timer);
    KiIrqRestore(flags);
    return previous;
}

BOOLEAN KeCancelTimer(PKTIMER timer)
{
    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "timer cancel above DISPATCH_LEVEL");
    UINT64 flags = KiIrqSave();
    BOOLEAN previous = timer->Inserted;
    KepDetachTimer(timer);
    /* Cancellation neither clears an existing signal nor retracts a DPC. */
    KiIrqRestore(flags);
    return previous;
}

void KiExpireTimers(UINT64 now)
{
    if (KeGetCurrentIrql() != DISPATCH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "timer expiry outside DPC context");
    while (!IsListEmpty(&g_timers)) {
        PKTIMER timer = CONTAINING_RECORD(g_timers.Flink, KTIMER, Entry);
        if (timer->DueTick > now) break;
        KepFireTimer(timer, now);
    }
}

void KiCancelThreadTimers(PKTHREAD thread)
{
    if (!thread) return;
    UINT64 flags = KiIrqSave();
    for (PLIST_ENTRY entry = g_timers.Flink; entry != &g_timers; ) {
        PKTIMER timer = CONTAINING_RECORD(entry, KTIMER, Entry);
        entry = entry->Flink;
        if (timer->StackOwner == thread) KepDetachTimer(timer);
    }
    KiIrqRestore(flags);
}
