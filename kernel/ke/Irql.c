/* The current scheduler is still UP, but spin locks use inter-CPU atomic
 * exclusion so their contract remains valid as processors are brought online. */
#include <svindows/Ke.h>

KIRQL KeGetCurrentIrql(void)
{ return KeGetCurrentProcessorBlock()->Irql; }

void KeRaiseIrql(KIRQL level, KIRQL *old)
{
    PKPRCB processor = KeGetCurrentProcessorBlock();
    if (!old || level < processor->Irql || level > HIGH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_GREATER_OR_EQUAL, "invalid IRQL raise");
    *old = processor->Irql;
    if (processor->Irql < DISPATCH_LEVEL && level >= DISPATCH_LEVEL)
        processor->DispatchFlags = KiIrqSave();
    processor->Irql = level;
    __asm__ volatile("" : : : "memory");
}

void KeLowerIrql(KIRQL level)
{
    PKPRCB processor = KeGetCurrentProcessorBlock();
    if (level > processor->Irql ||
        ((processor->HeldLocks || KiIsExecutingDpc()) &&
         level < DISPATCH_LEVEL))
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "invalid IRQL lower or held spin lock");
    BOOLEAN unmask = processor->Irql >= DISPATCH_LEVEL && level < DISPATCH_LEVEL;
    /* Retire while still masked. Lowering first and raising again would lose
     * the original IF snapshot, or run DPCs with the wrong execution level. */
    if (unmask) {
        processor->Irql = DISPATCH_LEVEL;
        KiRetireDpcList();
        if (processor->HeldLocks)
            KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "DPC leaked spin lock");
    }
    processor->Irql = level;
    __asm__ volatile("" : : : "memory");
    if (unmask)
        KiIrqRestore(processor->DispatchFlags);
}

KIRQL KiSwitchThreadIrql(KIRQL incoming)
{
    PKPRCB processor = KeGetCurrentProcessorBlock();
    if (incoming >= DISPATCH_LEVEL || processor->Irql >= DISPATCH_LEVEL ||
        processor->HeldLocks)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "context switch at elevated IRQL");
    KIRQL outgoing = processor->Irql;
    processor->Irql = incoming;
    return outgoing;
}

void KeInitializeSpinLock(PKSPIN_LOCK lock)
{
    lock->Held = 0;
    lock->OwnerThread = 0;
    lock->OwnerProcessor = 0;
}

/* Raw locks protect shared data without changing the caller's IRQL or
 * triggering DPC retirement on release. The caller's interrupt state is
 * preserved, and the lock may therefore also be used from HIGH_LEVEL paths. */
UINT64 KiAcquireRawSpinLock(PKSPIN_LOCK lock)
{
    UINT64 interrupt_state = KiIrqSave();
    PKPRCB processor = KeGetCurrentProcessorBlock();
    UINT64 owner = (UINT64)(UINT_PTR)KeGetCurrentThread();
    UINT32 processor_number = processor->ProcessorNumber;
    for (;;) {
        UINT32 expected = 0;
        if (__atomic_compare_exchange_n(&lock->Held, &expected, 1, FALSE,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            __atomic_store_n(&lock->OwnerThread, owner, __ATOMIC_RELAXED);
            __atomic_store_n(&lock->OwnerProcessor, processor_number,
                             __ATOMIC_RELAXED);
            return interrupt_state;
        }
        if (__atomic_load_n(&lock->OwnerThread, __ATOMIC_RELAXED) == owner &&
            __atomic_load_n(&lock->OwnerProcessor, __ATOMIC_RELAXED) ==
                processor_number)
            KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "recursive raw spin lock");
        while (__atomic_load_n(&lock->Held, __ATOMIC_RELAXED))
            __asm__ volatile("pause" ::: "memory");
    }
}

void KiReleaseRawSpinLock(PKSPIN_LOCK lock, UINT64 interrupt_state)
{
    PKPRCB processor = KeGetCurrentProcessorBlock();
    UINT64 owner = (UINT64)(UINT_PTR)KeGetCurrentThread();
    if (!__atomic_load_n(&lock->Held, __ATOMIC_RELAXED) ||
        __atomic_load_n(&lock->OwnerThread, __ATOMIC_RELAXED) != owner ||
        __atomic_load_n(&lock->OwnerProcessor, __ATOMIC_RELAXED) !=
            processor->ProcessorNumber)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "invalid raw spin lock release");
    __atomic_store_n(&lock->OwnerThread, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->OwnerProcessor, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->Held, 0, __ATOMIC_RELEASE);
    KiIrqRestore(interrupt_state);
}

void KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK lock)
{
    PKPRCB processor = KeGetCurrentProcessorBlock();
    if (processor->Irql < DISPATCH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "spin lock below DISPATCH_LEVEL");
    UINT64 owner = (UINT64)(UINT_PTR)KeGetCurrentThread();
    UINT32 processor_number = processor->ProcessorNumber;
    for (;;) {
        UINT32 expected = 0;
        if (__atomic_compare_exchange_n(&lock->Held, &expected, 1, FALSE,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            __atomic_store_n(&lock->OwnerThread, owner, __ATOMIC_RELAXED);
            __atomic_store_n(&lock->OwnerProcessor, processor_number,
                             __ATOMIC_RELAXED);
            break;
        }
        if (__atomic_load_n(&lock->OwnerThread, __ATOMIC_RELAXED) == owner &&
            __atomic_load_n(&lock->OwnerProcessor, __ATOMIC_RELAXED) == processor_number)
            KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL,
                       "recursive spin lock acquisition");
        while (__atomic_load_n(&lock->Held, __ATOMIC_RELAXED))
            __asm__ volatile("pause" ::: "memory");
    }
    processor->HeldLocks++;
}

void KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK lock)
{
    PKPRCB processor = KeGetCurrentProcessorBlock();
    UINT64 owner = (UINT64)(UINT_PTR)KeGetCurrentThread();
    if (processor->Irql < DISPATCH_LEVEL || !lock->Held || !processor->HeldLocks ||
        __atomic_load_n(&lock->OwnerThread, __ATOMIC_RELAXED) != owner ||
        __atomic_load_n(&lock->OwnerProcessor, __ATOMIC_RELAXED) !=
            processor->ProcessorNumber)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "invalid spin lock release");
    processor->HeldLocks--;
    __atomic_store_n(&lock->OwnerThread, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->OwnerProcessor, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->Held, 0, __ATOMIC_RELEASE);
}

void KeAcquireSpinLock(PKSPIN_LOCK lock, KIRQL *old)
{
    KeRaiseIrql(DISPATCH_LEVEL, old);
    KeAcquireSpinLockAtDpcLevel(lock);
}

void KeReleaseSpinLock(PKSPIN_LOCK lock, KIRQL old)
{
    if (KeGetCurrentProcessorBlock()->Irql != DISPATCH_LEVEL || old > DISPATCH_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "invalid spin lock IRQL restore");
    KeReleaseSpinLockFromDpcLevel(lock);
    KeLowerIrql(old);
}
