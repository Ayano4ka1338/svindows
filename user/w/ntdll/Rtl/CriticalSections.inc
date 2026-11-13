/* Win64 recursive critical sections. Contenders sleep on an executive
 * semaphore; LockCount accounts for both recursion and pending acquisitions. */
typedef struct _RTL_CRITICAL_SECTION_NT {
    void *DebugInfo;
    LONG LockCount, RecursionCount;
    HANDLE OwningThread, LockSemaphore;
    ULONGLONG SpinCount;
} RTL_CRITICAL_SECTION_NT;
_Static_assert(sizeof(RTL_CRITICAL_SECTION_NT) == 40,
               "Win64 critical section layout must be 40 bytes");

extern NTSTATUS NtCreateSemaphore(HANDLE *, ULONG, void *, LONG, LONG);
extern NTSTATUS NtReleaseSemaphore(HANDLE, LONG, LONG *);
extern NTSTATUS NtWaitForSingleObject(HANDLE, int, const LONGLONG *);
extern NTSTATUS NtClose(HANDLE);

NTSTATUS RtlInitializeCriticalSection(void *section)
{
    if (!section) return STATUS_INVALID_PARAMETER;
    RTL_CRITICAL_SECTION_NT *lock = section;
    memset(lock, 0, sizeof(*lock));
    lock->LockCount = -1;
    return STATUS_SUCCESS;
}

NTSTATUS RtlInitializeCriticalSectionAndSpinCount(void *section, ULONG count)
{
    NTSTATUS status = RtlInitializeCriticalSection(section);
    if (NT_SUCCESS(status)) ((RTL_CRITICAL_SECTION_NT *)section)->SpinCount = count & 0x7fffffffUL;
    return status;
}

static NTSTATUS rtl_critical_section_semaphore(RTL_CRITICAL_SECTION_NT *lock, HANDLE *out)
{
    HANDLE semaphore = __atomic_load_n(&lock->LockSemaphore, __ATOMIC_ACQUIRE);
    if (!semaphore) {
        HANDLE created = 0;
        NTSTATUS status = NtCreateSemaphore(&created, 0x001f0003UL, 0, 0, 0x7fffffffL);
        if (!NT_SUCCESS(status)) return status;
        HANDLE expected = 0;
        if (__atomic_compare_exchange_n(&lock->LockSemaphore, &expected, created,
                0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) semaphore = created;
        else { semaphore = expected; NtClose(created); }
    }
    *out = semaphore;
    return STATUS_SUCCESS;
}

BOOL RtlTryEnterCriticalSection(void *section)
{
    if (!section) return FALSE;
    RTL_CRITICAL_SECTION_NT *lock = section;
    HANDLE thread = (HANDLE)read_teb_qword(0x48);
    if (__atomic_load_n(&lock->OwningThread, __ATOMIC_RELAXED) == thread) {
        if (lock->RecursionCount == 0x7fffffffL) return FALSE;
        __atomic_add_fetch(&lock->LockCount, 1, __ATOMIC_RELAXED);
        lock->RecursionCount++;
        return TRUE;
    }
    LONG expected = -1;
    if (!__atomic_compare_exchange_n(&lock->LockCount, &expected, 0,
            0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return FALSE;
    __atomic_store_n(&lock->OwningThread, thread, __ATOMIC_RELAXED);
    lock->RecursionCount = 1;
    return TRUE;
}

NTSTATUS RtlEnterCriticalSection(void *section)
{
    if (!section) return STATUS_INVALID_PARAMETER;
    RTL_CRITICAL_SECTION_NT *lock = section;
    if (RtlTryEnterCriticalSection(section)) return STATUS_SUCCESS;
    HANDLE thread = (HANDLE)read_teb_qword(0x48);
    if (__atomic_load_n(&lock->OwningThread, __ATOMIC_RELAXED) == thread)
        return (NTSTATUS)0xc0000095L; /* STATUS_INTEGER_OVERFLOW */
    ULONGLONG spins = lock->SpinCount;
    while (spins--) {
        if (RtlTryEnterCriticalSection(section)) return STATUS_SUCCESS;
        __asm__ volatile("pause");
    }
    HANDLE semaphore;
    NTSTATUS status = rtl_critical_section_semaphore(lock, &semaphore);
    if (!NT_SUCCESS(status)) return status; /* no waiter count was published */
    LONG count = __atomic_add_fetch(&lock->LockCount, 1, __ATOMIC_ACQUIRE);
    if (count != 0) {
        status = NtWaitForSingleObject(semaphore, 0, 0);
        if (status != STATUS_SUCCESS) {
            __atomic_sub_fetch(&lock->LockCount, 1, __ATOMIC_RELEASE);
            return status;
        }
    }
    __atomic_store_n(&lock->OwningThread, thread, __ATOMIC_RELAXED);
    lock->RecursionCount = 1;
    return STATUS_SUCCESS;
}

NTSTATUS RtlLeaveCriticalSection(void *section)
{
    if (!section) return STATUS_INVALID_PARAMETER;
    RTL_CRITICAL_SECTION_NT *lock = section;
    if (__atomic_load_n(&lock->OwningThread, __ATOMIC_RELAXED) !=
            (HANDLE)read_teb_qword(0x48) || lock->RecursionCount <= 0)
        return (NTSTATUS)0xc0000264L; /* STATUS_RESOURCE_NOT_OWNED */
    if (--lock->RecursionCount) {
        __atomic_sub_fetch(&lock->LockCount, 1, __ATOMIC_RELEASE);
        return STATUS_SUCCESS;
    }
    __atomic_store_n(&lock->OwningThread, 0, __ATOMIC_RELAXED);
    LONG remaining = __atomic_sub_fetch(&lock->LockCount, 1, __ATOMIC_RELEASE);
    if (remaining >= 0)
        return NtReleaseSemaphore(__atomic_load_n(&lock->LockSemaphore, __ATOMIC_ACQUIRE), 1, 0);
    return STATUS_SUCCESS;
}

NTSTATUS RtlDeleteCriticalSection(void *section)
{
    if (!section) return STATUS_INVALID_PARAMETER;
    RTL_CRITICAL_SECTION_NT *lock = section;
    if (__atomic_load_n(&lock->LockCount, __ATOMIC_ACQUIRE) != -1 ||
        __atomic_load_n(&lock->OwningThread, __ATOMIC_RELAXED))
        return (NTSTATUS)0xc0000708L; /* STATUS_RESOURCE_IN_USE */
    HANDLE semaphore = __atomic_exchange_n(&lock->LockSemaphore, 0, __ATOMIC_ACQ_REL);
    NTSTATUS status = semaphore ? NtClose(semaphore) : STATUS_SUCCESS;
    if (NT_SUCCESS(status)) {
        memset(lock, 0, sizeof(*lock));
        lock->LockCount = -1;
    }
    return status;
}
