/* UP execution levels. PIC has no usable per-CPU priority threshold: mask
 * all IRQs at DISPATCH_LEVEL+, retaining the caller's original IF state. */
#include <svindows/Ke.h>

static KIRQL g_irql;
static UINT64 g_dispatch_flags;
static ULONG g_held_locks;

KIRQL KeGetCurrentIrql(void) { return g_irql; }

void KeRaiseIrql(KIRQL level, KIRQL *old)
{
    if (!old || level < g_irql || level > HIGH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_GREATER_OR_EQUAL, "invalid IRQL raise");
    *old = g_irql;
    if (g_irql < DISPATCH_LEVEL && level >= DISPATCH_LEVEL)
        g_dispatch_flags = KiIrqSave();
    g_irql = level;
    __asm__ volatile("" : : : "memory");
}

void KeLowerIrql(KIRQL level)
{
    if (level > g_irql ||
        ((g_held_locks || KiIsExecutingDpc()) && level < DISPATCH_LEVEL))
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "invalid IRQL lower or held spin lock");
    BOOLEAN unmask = g_irql >= DISPATCH_LEVEL && level < DISPATCH_LEVEL;
    /* Retire while still masked. Lowering first and raising again would lose
     * the original IF snapshot, or run DPCs with the wrong execution level. */
    if (unmask) {
        g_irql = DISPATCH_LEVEL;
        KiRetireDpcList();
        if (g_held_locks)
            KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "DPC leaked spin lock");
    }
    g_irql = level;
    __asm__ volatile("" : : : "memory");
    if (unmask)
        KiIrqRestore(g_dispatch_flags);
}

KIRQL KiSwitchThreadIrql(KIRQL incoming)
{
    if (incoming >= DISPATCH_LEVEL || g_irql >= DISPATCH_LEVEL || g_held_locks)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "context switch at elevated IRQL");
    KIRQL outgoing = g_irql;
    g_irql = incoming;
    return outgoing;
}

void KeInitializeSpinLock(PKSPIN_LOCK lock) { lock->Held = 0; }

void KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK lock)
{
    if (g_irql < DISPATCH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "spin lock below DISPATCH_LEVEL");
    /* A held lock on one CPU cannot become free while IRQs are masked. Do
     * not spin forever on recursive acquisition or an inconsistent owner. */
    if (__atomic_exchange_n(&lock->Held, 1, __ATOMIC_ACQUIRE))
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "recursive UP spin lock acquisition");
    g_held_locks++;
}

void KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK lock)
{
    if (g_irql < DISPATCH_LEVEL || !lock->Held || !g_held_locks)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "invalid spin lock release");
    g_held_locks--;
    __atomic_store_n(&lock->Held, 0, __ATOMIC_RELEASE);
}

void KeAcquireSpinLock(PKSPIN_LOCK lock, KIRQL *old)
{
    KeRaiseIrql(DISPATCH_LEVEL, old);
    KeAcquireSpinLockAtDpcLevel(lock);
}

void KeReleaseSpinLock(PKSPIN_LOCK lock, KIRQL old)
{
    if (g_irql != DISPATCH_LEVEL || old > DISPATCH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "invalid spin lock IRQL restore");
    KeReleaseSpinLockFromDpcLevel(lock);
    KeLowerIrql(old);
}
