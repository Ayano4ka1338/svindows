/* Native x64 critical sections, shared by ntdll and kernel32 wrappers. */
typedef struct _RTL_CRITICAL_SECTION_NT {
    void *DebugInfo;          /* 0x00 */
    LONG LockCount;           /* 0x08 */
    LONG RecursionCount;      /* 0x0C */
    HANDLE OwningThread;      /* 0x10 */
    HANDLE LockSemaphore;     /* 0x18 */
    ULONGLONG SpinCount;      /* 0x20 */
} RTL_CRITICAL_SECTION_NT;
_Static_assert(sizeof(RTL_CRITICAL_SECTION_NT) == 40,
               "Win64 critical section layout must be 40 bytes");

NTSTATUS RtlInitializeCriticalSection(void *cs)
{
    RTL_CRITICAL_SECTION_NT *c = (RTL_CRITICAL_SECTION_NT *)cs;
    c->DebugInfo = 0;
    c->LockCount = -1;
    c->RecursionCount = 0;
    c->OwningThread = 0;
    c->LockSemaphore = 0;
    c->SpinCount = 0;
    return STATUS_SUCCESS;
}

NTSTATUS RtlInitializeCriticalSectionAndSpinCount(void *cs, ULONG count)
{
    NTSTATUS st = RtlInitializeCriticalSection(cs);
    ((RTL_CRITICAL_SECTION_NT *)cs)->SpinCount = count;
    return st;
}

NTSTATUS RtlEnterCriticalSection(void *cs)
{
    RTL_CRITICAL_SECTION_NT *c = (RTL_CRITICAL_SECTION_NT *)cs;
    ULONGLONG tid = read_teb_qword(0x48); /* ClientId.UniqueThread */
    if (__atomic_load_n(&c->OwningThread, __ATOMIC_RELAXED) == (HANDLE)tid) {
        c->RecursionCount++;
        return STATUS_SUCCESS;
    }
    for (;;) {
        if (__sync_bool_compare_and_swap(&c->LockCount, -1, 0)) {
            __atomic_store_n(&c->OwningThread, (HANDLE)tid, __ATOMIC_RELAXED);
            c->RecursionCount = 1;
            return STATUS_SUCCESS;
        }
        __asm__ volatile("pause");
    }
}

NTSTATUS RtlLeaveCriticalSection(void *cs)
{
    RTL_CRITICAL_SECTION_NT *c = (RTL_CRITICAL_SECTION_NT *)cs;
    if (__atomic_load_n(&c->OwningThread, __ATOMIC_RELAXED) !=
        (HANDLE)read_teb_qword(0x48) ||
        c->RecursionCount <= 0)
        return (NTSTATUS)0xC0000264L; /* STATUS_RESOURCE_NOT_OWNED */
    if (--c->RecursionCount == 0) {
        __atomic_store_n(&c->OwningThread, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&c->LockCount, -1, __ATOMIC_RELEASE);
    }
    return STATUS_SUCCESS;
}

BOOL RtlTryEnterCriticalSection(void *cs)
{
    RTL_CRITICAL_SECTION_NT *c = (RTL_CRITICAL_SECTION_NT *)cs;
    ULONGLONG tid = read_teb_qword(0x48);
    if (__atomic_load_n(&c->OwningThread, __ATOMIC_RELAXED) == (HANDLE)tid) {
        c->RecursionCount++;
        return TRUE;
    }
    if (!__sync_bool_compare_and_swap(&c->LockCount, -1, 0))
        return FALSE;
    __atomic_store_n(&c->OwningThread, (HANDLE)tid, __ATOMIC_RELAXED);
    c->RecursionCount = 1;
    return TRUE;
}

NTSTATUS RtlDeleteCriticalSection(void *cs)
{
    memset(cs, 0, sizeof(RTL_CRITICAL_SECTION_NT));
    return STATUS_SUCCESS;
}
