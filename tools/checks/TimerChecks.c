/* Actual Ke timers, native services, waits, Sleep syscall and Ob lifetime.
 * Hardware/context switching are simulated; nothing installed in the guest. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <svindows/Ke.h>
#include <svindows/Ex.h>
#include <svindows/Ob.h>
static UINT64 host_flags = 0x202, tick = 10;
static UINT64 HostIrqSave(void)
{ UINT64 saved = host_flags; host_flags &= ~0x200ULL; return saved; }
static void HostIrqRestore(UINT64 flags) { host_flags = flags; }
#define KiIrqSave HostIrqSave
#define KiIrqRestore HostIrqRestore
#include "../../kernel/ke/Irql.c"
#include "../../kernel/ke/Dpc.c"
#include "../../kernel/ke/Timer.c"
#include "../../kernel/ke/Dispatcher.c"
#include "../../kernel/ob/ObCore.c"
#include "../../kernel/ob/ObHandle.c"
#include "../../kernel/ob/ObNamespace.c"
#include "../../kernel/ps/TimerObject.c"
#include "../../kernel/ps/Token.c"
#include "../../kernel/ps/Psobj.c"
#include "../../kernel/ps/Job.c"
#include "../../kernel/cm/Registry.c"
#include "../../kernel/ke/Syscall.c"

static KTHREAD current;
static KPROCESS process;
static ULONG allocations, blocks, yields, firings;
static HANDLE close_during_wait;
static BOOLEAN allocation_fail;
static UINT64 invalid_address;
PKTHREAD KeGetCurrentThread(void) { return &current; }
UINT64 KeGetTickCount(void) { return tick; }
void KeYield(void) { yields++; }
int KeLog(const char *format, ...) { (void)format; return 0; }
NORETURN void KeBugCheck(ULONG code, const char *message)
{ fprintf(stderr, "BUGCHECK %x: %s\n", code, message); abort(); }
PVOID ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T size, ULONG tag)
{
    (void)type; (void)tag;
    if (allocation_fail) return NULL;
    void *memory = malloc(size);
    assert(memory); allocations++; return memory;
}
void ExFreePool(PVOID memory) { if (memory) { allocations--; free(memory); } }
PKTHREAD KeLookupThreadById(ULONG id)
{ return id == current.ThreadId ? &current : NULL; }
BOOLEAN PsIsProcessObjectType(POBJECT_TYPE type) { (void)type; return FALSE; }
PKPROCESS PsGetProcessFromObject(PVOID object) { (void)object; return NULL; }
BOOLEAN MmCaptureUnicodeName(UINT64 address, char *out, SIZE_T size)
{
    if (!address || !out || !size) return FALSE;
    const UNICODE_STRING *name = (const UNICODE_STRING *)address;
    if (!name->Buffer || name->Length / 2 >= size) return FALSE;
    for (ULONG i = 0; i < name->Length / 2; i++)
        out[i] = (char)name->Buffer[i];
    out[name->Length / 2] = 0;
    return TRUE;
}
BOOLEAN MmProbeForRead(UINT64 address, UINT64 size)
{ (void)size; return address && address != invalid_address; }
BOOLEAN MmProbeForWrite(UINT64 address, UINT64 size)
{ return MmProbeForRead(address, size); }
void KiReadyThread(PKTHREAD thread) { thread->State = ThreadStateReady; }

static void Advance(UINT64 now)
{
    tick = now;
    KIRQL old;
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    KiExpireTimers(tick);
    KeLowerIrql(old);
}
void KiBlockCurrentThread(void)
{
    blocks++;
    assert(!IsListEmpty(&g_timers));
    assert(KeGetCurrentIrql() <= APC_LEVEL && current.State == ThreadStateWaiting);
    if (close_during_wait) {
        assert(ObCloseHandle(close_during_wait) == STATUS_SUCCESS);
        close_during_wait = NULL;
        assert(g_timer_type->TotalObjects == 1); /* wait owns a reference */
    }
    Advance(CONTAINING_RECORD(g_timers.Flink, KTIMER, Entry)->DueTick);
    assert(current.State == ThreadStateReady);
    current.State = ThreadStateRunning;
}
static void Fired(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2)
{
    (void)dpc; (void)context;
    assert(KeGetCurrentIrql() == DISPATCH_LEVEL);
    UINT64 time = tick * KE_TIME_UNITS_PER_TICK;
    assert((ULONG_PTR)arg1 == (ULONG)time && (ULONG_PTR)arg2 == (ULONG)(time >> 32));
    firings++;
}

static void CheckKernelTimers(void)
{
    KTIMER one, periodic, immediate;
    KDPC callback;
    KeInitializeDpc(&callback, Fired, NULL);
    KeInitializeTimerEx(&one, FALSE);
    KeInitializeTimerEx(&periodic, TRUE);
    KeInitializeTimerEx(&immediate, TRUE);
    assert(KeDueTimeToTicks(-1) == 1);
    assert(KeDueTimeToTicks(-100001) == 2);
    assert(KeDueTimeToTicks(0) == 0);
    assert(KeDueTimeToTicks(1100000) == 1);
    assert(KeDueTimeToTicks((INT64)0x8000000000000000ULL) == 92233720368548ULL);
    assert(!KeSetTimerEx(&one, -200000, 0, &callback));
    assert(KeWaitForSingleObjectTimeout(&one.Header, 0) == STATUS_TIMEOUT);
    Advance(11);
    assert(!one.Header.SignalState);
    Advance(12);
    assert(firings == 1 && !one.Inserted);
    assert(KeWaitForSingleObjectTimeout(&one.Header, 0) == STATUS_SUCCESS);
    assert(KeWaitForSingleObjectTimeout(&one.Header, 0) == STATUS_TIMEOUT);
    KeSetTimerEx(&periodic, -100000, 20, NULL);
    Advance(13);
    assert(periodic.DueTick == 15 && periodic.Inserted);
    assert(KeWaitForSingleObjectTimeout(&periodic.Header, 0) == STATUS_SUCCESS);
    assert(KeWaitForSingleObjectTimeout(&periodic.Header, 0) == STATUS_SUCCESS);
    Advance(100); /* missed intervals skipped, phase preserved */
    assert(periodic.DueTick == 101);
    assert(KeSetTimerEx(&periodic, -500000, 0, NULL));
    assert(!periodic.Header.SignalState);
    assert(KeCancelTimer(&periodic) && !KeCancelTimer(&periodic));
    Advance(110);
    assert(!periodic.Header.SignalState);
    KeSetTimerEx(&immediate, 1, 0, NULL); /* expired absolute time */
    assert(immediate.Header.SignalState && !immediate.Inserted);
    assert(!KeCancelTimer(&immediate) && immediate.Header.SignalState);
    KeSetTimerEx(&one, -1, 0, NULL);
    assert(KeWaitForSingleObject(&one.Header) == STATUS_SUCCESS);
    assert(blocks == 1 && tick == 111);
    KTIMER a, b;
    KeInitializeTimerEx(&a, FALSE); KeInitializeTimerEx(&b, FALSE);
    KeSetTimerEx(&a, -100000, 0, NULL); KeSetTimerEx(&b, -200000, 0, NULL);
    PDISPATCHER_HEADER objects[] = { &a.Header, &b.Header };
    assert(KeWaitForMultipleObjects(2, objects, TRUE, ~(UINT64)0) == STATUS_SUCCESS);
    assert(blocks == 3 && !a.Header.SignalState && !b.Header.SignalState);
    KeSetTimerEx(&one, -1000000, 0, NULL);
    one.StackOwner = &current;
    KiCancelThreadTimers(&current);
    assert(!one.Inserted && IsListEmpty(&g_timers));
    tick = ~(UINT64)0 - 1;
    KeSetTimerEx(&one, -500000, 10, NULL);
    assert(one.DueTick == ~(UINT64)0);
    Advance(~(UINT64)0);
    assert(!one.Inserted); /* no periodic reinsertion loop at saturated time */
    tick = 113;
    puts("PASS kernel timers: ceil/INT64_MIN, one-shot, periodic, rearm/cancel, DPC, wait-all, stack-owner cleanup");
}

static void CheckNamedEvents(void)
{
    UINT16 name_buffer[] = {'\\','B','a','s','e','N','a','m','e','d','O','b','j',
        'e','c','t','s','\\','P','r','o','b','e',0};
    UNICODE_STRING name = {46, 48, name_buffer};
    OBJECT_ATTRIBUTES attributes = {sizeof(attributes), NULL, &name, 0, 0, 0};
    HANDLE first = NULL, second = NULL;
    UINT64 create[] = {(UINT64)&first, 0x001F0003, (UINT64)&attributes, 0, 0};
    UINT64 open[] = {(UINT64)&second, 0x001F0003, (UINT64)&attributes};
    assert((NTSTATUS)NtOpenEvent(open) == STATUS_OBJECT_NAME_NOT_FOUND);
    assert((NTSTATUS)NtCreateEvent(create) == STATUS_SUCCESS && first);
    assert((NTSTATUS)NtOpenEvent(open) == STATUS_SUCCESS && second);
    POBJECT one, two;
    assert(ObReferenceObjectByHandle(first, 0, g_event_type, &one) == STATUS_SUCCESS);
    assert(ObReferenceObjectByHandle(second, 0, g_event_type, &two) == STATUS_SUCCESS);
    assert(one == two);
    ObDereferenceObject(one); ObDereferenceObject(two);
    HANDLE third = NULL;
    create[0] = (UINT64)&third;
    assert((NTSTATUS)NtCreateEvent(create) == 0x40000000u && third);
    assert(ObCloseHandle(first) == STATUS_SUCCESS);
    assert(ObCloseHandle(second) == STATUS_SUCCESS);
    assert(ObCloseHandle(third) == STATUS_SUCCESS);
    assert((NTSTATUS)NtOpenEvent(open) == STATUS_OBJECT_NAME_NOT_FOUND);
    puts("PASS named event open, shared identity, collision and last-handle unlink");
}

static void CheckJobs(void)
{
    UINT16 name_buffer[] = {'\\','B','a','s','e','N','a','m','e','d','O','b','j',
        'e','c','t','s','\\','T','e','s','t','J','o','b',0};
    UNICODE_STRING name = {50, 52, name_buffer};
    OBJECT_ATTRIBUTES attributes = {sizeof(attributes), NULL, &name, 0, 0, 0};
    HANDLE job = NULL, reopened = NULL;
    UINT64 create[] = {(UINT64)&job, 0x001F003F, (UINT64)&attributes};
    assert((NTSTATUS)NtCreateJobObject(create) == STATUS_SUCCESS && job);
    create[0] = (UINT64)&reopened;
    assert((NTSTATUS)NtCreateJobObject(create) == 0x40000000u && reopened);
    JOB_EXTENDED_LIMITS limits = {0};
    limits.Basic.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.Basic.ActiveProcessLimit = 1;
    UINT64 set[] = {(UINT64)job, 9, (UINT64)&limits, sizeof(limits)};
    assert((NTSTATUS)NtSetInformationJobObject(set) == STATUS_SUCCESS);
    limits.Basic.LimitFlags = 0x2000; /* unsupported kill-on-close limit */
    assert((NTSTATUS)NtSetInformationJobObject(set) == STATUS_NOT_SUPPORTED);
    InitializeListHead(&process.JobEntry);
    process.Object = &process;
    process.ProcessId = 44;
    UINT64 assign[] = {(UINT64)job, ~(UINT64)0};
    assert((NTSTATUS)NtAssignProcessToJobObject(assign) == STATUS_SUCCESS);
    assert((NTSTATUS)NtAssignProcessToJobObject(assign) == STATUS_ACCESS_DENIED);
    JOB_ACCOUNTING accounting = {0};
    ULONG length = 0;
    UINT64 query[] = {(UINT64)job, 1, (UINT64)&accounting,
                      sizeof(accounting), (UINT64)&length};
    assert((NTSTATUS)NtQueryInformationJobObject(query) == STATUS_SUCCESS);
    assert(length == sizeof(accounting) && accounting.TotalProcesses == 1 &&
           accounting.ActiveProcesses == 1);
    PsJobProcessExited(&process);
    assert((NTSTATUS)NtQueryInformationJobObject(query) == STATUS_SUCCESS);
    assert(accounting.ActiveProcesses == 0 &&
           accounting.TotalTerminatedProcesses == 1);
    process.Object = NULL;
    assert(ObCloseHandle(job) == STATUS_SUCCESS);
    assert(ObCloseHandle(reopened) == STATUS_SUCCESS);
    POBJECT absent;
    assert(ObLookupObjectByName("\\BaseNamedObjects\\TestJob", &absent) ==
           STATUS_OBJECT_NAME_NOT_FOUND);
    puts("PASS job naming, membership, active-process limit and accounting");
}

static HANDLE Create(ACCESS_MASK access, ULONG type)
{
    HANDLE handle = NULL;
    UINT64 args[] = { (UINT64)&handle, access, 0, type };
    assert((NTSTATUS)NtCreateTimer(args) == STATUS_SUCCESS);
    return handle;
}
static void CheckNativeTimers(void)
{
    HANDLE handle = Create(TIMER_ALL_ACCESS, 1);
    BOOLEAN previous = TRUE;
    INT64 due = -100001;
    UINT64 set[] = { (UINT64)handle, (UINT64)&due, 0, 0, 0, 0, (UINT64)&previous };
    assert((NTSTATUS)NtSetTimer(set) == STATUS_SUCCESS && !previous);
    TIMER_BASIC_INFORMATION info;
    ULONG length = 0;
    UINT64 query[] = { (UINT64)handle, 0, (UINT64)&info, sizeof(info), (UINT64)&length };
    assert((NTSTATUS)NtQueryTimer(query) == STATUS_SUCCESS);
    assert(info.RemainingTime == 200000 && !info.TimerState && length == sizeof(info));
    UINT64 wait[] = { (UINT64)handle, 0, 0 };
    assert((NTSTATUS)NtWaitForSingleObject(wait) == STATUS_SUCCESS);
    assert((NTSTATUS)NtQueryTimer(query) == STATUS_SUCCESS && !info.TimerState);
    due = 0;
    assert((NTSTATUS)NtSetTimer(set) == STATUS_SUCCESS);
    UINT64 cancel[] = { (UINT64)handle, (UINT64)&previous };
    assert((NTSTATUS)NtCancelTimer(cancel) == STATUS_SUCCESS && previous);
    assert((NTSTATUS)NtQueryTimer(query) == STATUS_SUCCESS && info.TimerState);
    due = -500000;
    assert((NTSTATUS)NtSetTimer(set) == STATUS_SUCCESS);
    assert((NTSTATUS)NtCancelTimer(cancel) == STATUS_SUCCESS);
    assert((NTSTATUS)NtQueryTimer(query) == STATUS_SUCCESS && info.RemainingTime == 500000);
    Advance(tick + 6);
    assert((NTSTATUS)NtQueryTimer(query) == STATUS_SUCCESS && info.RemainingTime == -100000);
    set[2] = 1;
    assert((NTSTATUS)NtSetTimer(set) == STATUS_NOT_IMPLEMENTED);
    set[2] = 0; set[4] = 1;
    assert((NTSTATUS)NtSetTimer(set) == STATUS_NOT_IMPLEMENTED);
    set[4] = 0; set[5] = (UINT64)-1;
    assert((NTSTATUS)NtSetTimer(set) == STATUS_INVALID_PARAMETER);
    set[5] = 0; set[6] = 0;
    HANDLE restricted = Create(TIMER_QUERY_STATE, 0);
    set[0] = (UINT64)restricted;
    assert((NTSTATUS)NtSetTimer(set) == STATUS_ACCESS_DENIED);
    wait[0] = (UINT64)restricted;
    assert((NTSTATUS)NtWaitForSingleObject(wait) == STATUS_ACCESS_DENIED);
    assert(ObCloseHandle(restricted) == STATUS_SUCCESS);
    assert(ObCloseHandle(handle) == STATUS_SUCCESS);
    handle = Create(TIMER_ALL_ACCESS, 1);
    set[0] = (UINT64)handle; due = -100000;
    assert((NTSTATUS)NtSetTimer(set) == STATUS_SUCCESS);
    wait[0] = (UINT64)handle;
    close_during_wait = handle;
    assert((NTSTATUS)NtWaitForSingleObject(wait) == STATUS_SUCCESS);
    assert(!g_timer_type->TotalObjects);
    for (ULONG i = 0; i < 1000; i++) {
        handle = Create(TIMER_ALL_ACCESS, 0);
        due = -1000000; set[0] = (UINT64)handle;
        assert((NTSTATUS)NtSetTimer(set) == STATUS_SUCCESS);
        assert(ObCloseHandle(handle) == STATUS_SUCCESS); /* delete cancels active */
        assert(IsListEmpty(&g_timers));
    }
    assert(!g_timer_type->TotalObjects && !g_timer_type->TotalHandles);
    allocation_fail = TRUE;
    UINT64 create[] = { (UINT64)&handle, TIMER_ALL_ACCESS, 0, 0 };
    assert((NTSTATUS)NtCreateTimer(create) == STATUS_NO_MEMORY);
    allocation_fail = FALSE;
    create[3] = 2;
    assert((NTSTATUS)NtCreateTimer(create) == STATUS_INVALID_PARAMETER);
    query[3] = 1;
    assert((NTSTATUS)NtQueryTimer(query) == STATUS_INFO_LENGTH_MISMATCH);
    query[3] = sizeof(info); query[1] = 1;
    assert((NTSTATUS)NtQueryTimer(query) == STATUS_INVALID_INFO_CLASS);
    query[1] = 0; query[2] = 0;
    assert((NTSTATUS)NtQueryTimer(query) == STATUS_ACCESS_VIOLATION);
    puts("PASS native timer handles/access/query, APC rejection, 1000 armed-close lifecycles and allocation failure");
}

static void CheckTokens(void)
{
    process.SecurityUserRid = 1000;
    HANDLE handle = NULL;
    UINT64 open[] = { (UINT64)-1, TOKEN_QUERY, (UINT64)&handle };
    assert((NTSTATUS)NtOpenProcessToken(open) == STATUS_SUCCESS && handle);
    UINT8 buffer[256];
    ULONG required = 0;
    UINT64 query[] = { (UINT64)handle, 1, 0, 0, (UINT64)&required };
    assert((NTSTATUS)NtQueryInformationToken(query) == STATUS_BUFFER_TOO_SMALL);
    assert(required == 44);
    query[2] = (UINT64)buffer; query[3] = sizeof(buffer);
    assert((NTSTATUS)NtQueryInformationToken(query) == STATUS_SUCCESS);
    SID_ATTRIBUTES *user = (SID_ATTRIBUTES *)buffer;
    assert(user->Sid == buffer + 16);
    assert(((UINT8 *)user->Sid)[1] == 5);
    assert(*(ULONG *)((UINT8 *)user->Sid + 24) == 1000);
    query[1] = 2;
    assert((NTSTATUS)NtQueryInformationToken(query) == STATUS_SUCCESS);
    assert(*(ULONG *)buffer == 4);
    SID_ATTRIBUTES *groups = (SID_ATTRIBUTES *)(buffer + 8);
    assert(((UINT8 *)groups[0].Sid)[7] == 1);
    assert(*(ULONG *)((UINT8 *)groups[3].Sid + 12) == 545);
    query[1] = 25;
    assert((NTSTATUS)NtQueryInformationToken(query) == STATUS_SUCCESS);
    assert(*(ULONG *)((UINT8 *)((SID_ATTRIBUTES *)buffer)->Sid + 8) == 8192);
    query[1] = 999;
    assert((NTSTATUS)NtQueryInformationToken(query) == STATUS_INVALID_INFO_CLASS);
    assert(ObCloseHandle(handle) == STATUS_SUCCESS);
    query[1] = 1;
    assert((NTSTATUS)NtQueryInformationToken(query) == STATUS_INVALID_HANDLE);
    puts("PASS process token handle lifetime, SID layout, groups, integrity and query lengths");
}

static void CheckRegistryDeletion(void)
{
    g_key_type = ObCreateObjectType("Key", CmpKeyDelete);
    g_cm_root = cm_alloc_key("", NULL);
    CM_KEY *parent = cm_alloc_key("Parent", g_cm_root);
    CM_KEY *leaf = cm_alloc_key("Leaf", parent);
    HANDLE first = (HANDLE)cm_make_handle(leaf);
    HANDLE second = (HANDLE)cm_make_handle(leaf);
    assert(first && second && leaf->OpenObjects == 2);
    UINT64 del[] = { (UINT64)(ULONG_PTR)first };
    assert((NTSTATUS)NtDeleteKey(del) == STATUS_SUCCESS);
    assert(cm_find_subkey(parent, "Leaf") == NULL);
    assert(cm_key_from_handle((UINT64)(ULONG_PTR)second) == NULL);
    assert((NTSTATUS)NtDeleteKey(del) == STATUS_INVALID_HANDLE);
    assert(ObCloseHandle(first) == STATUS_SUCCESS);
    assert(leaf->OpenObjects == 1);
    assert(ObCloseHandle(second) == STATUS_SUCCESS);
    CM_KEY *branch = cm_alloc_key("Branch", parent);
    CM_KEY *child = cm_alloc_key("Child", branch);
    HANDLE branch_handle = (HANDLE)cm_make_handle(branch);
    del[0] = (UINT64)(ULONG_PTR)branch_handle;
    assert((NTSTATUS)NtDeleteKey(del) == STATUS_CANNOT_DELETE);
    assert(ObCloseHandle(branch_handle) == STATUS_SUCCESS);
    HANDLE parent_handle = (HANDLE)cm_make_handle(parent);
    UINT8 key_info[64];
    ULONG required = 0;
    UINT64 enum_key[] = { (UINT64)(ULONG_PTR)parent_handle, 0, 0,
                          (UINT64)key_info, sizeof(key_info),
                          (UINT64)&required };
    assert((NTSTATUS)NtEnumerateKey(enum_key) == STATUS_SUCCESS);
    assert(required == 28 && *(UINT16 *)(key_info + 16) == 'B');
    UINT8 key_full[44];
    UINT64 query_key[] = { (UINT64)(ULONG_PTR)parent_handle, 2,
                           (UINT64)key_full, sizeof(key_full),
                           (UINT64)&required };
    assert((NTSTATUS)NtQueryKey(query_key) == STATUS_SUCCESS);
    assert(*(ULONG *)(key_full + 20) == 1);
    assert(*(ULONG *)(key_full + 24) == 12);
    enum_key[1] = 1;
    assert((NTSTATUS)NtEnumerateKey(enum_key) == STATUS_NO_MORE_ENTRIES);
    const char value_bytes[] = "abc";
    assert(cm_set_value(parent, "Text", REG_SZ, value_bytes,
                        sizeof(value_bytes)) == STATUS_SUCCESS);
    UINT8 value_info[64];
    UINT64 enum_value[] = { (UINT64)(ULONG_PTR)parent_handle, 0, 1,
                            0, 0, (UINT64)&required };
    assert((NTSTATUS)NtEnumerateValueKey(enum_value) == STATUS_BUFFER_TOO_SMALL);
    enum_value[3] = (UINT64)value_info;
    enum_value[4] = sizeof(value_info);
    assert((NTSTATUS)NtEnumerateValueKey(enum_value) == STATUS_SUCCESS);
    assert(*(ULONG *)(value_info + 4) == REG_SZ);
    assert(*(ULONG *)(value_info + 16) == 8);
    assert(!memcmp(value_info + *(ULONG *)(value_info + 8), value_bytes,
                   sizeof(value_bytes)));
    assert((NTSTATUS)NtQueryKey(query_key) == STATUS_SUCCESS);
    assert(*(ULONG *)(key_full + 32) == 1);
    assert(*(ULONG *)(key_full + 36) == 8);
    assert(*(ULONG *)(key_full + 40) == sizeof(value_bytes));
    UINT16 value_name[] = { 'T', 'e', 'x', 't', 0 };
    UNICODE_STRING unicode_name = { 8, 10, value_name };
    UINT64 delete_value[] = { (UINT64)(ULONG_PTR)parent_handle,
                              (UINT64)&unicode_name };
    assert((NTSTATUS)NtDeleteValueKey(delete_value) == STATUS_SUCCESS);
    assert((NTSTATUS)NtEnumerateValueKey(enum_value) == STATUS_NO_MORE_ENTRIES);
    assert((NTSTATUS)NtQueryKey(query_key) == STATUS_SUCCESS);
    assert(*(ULONG *)(key_full + 32) == 0);
    assert(ObCloseHandle(parent_handle) == STATUS_SUCCESS);
    ExFreePool(child);
    ExFreePool(branch);
    ExFreePool(parent);
    ExFreePool(g_cm_root);
    g_cm_root = NULL;
    puts("PASS registry delete unlinks leaf, invalidates handles and defers free");
}

int main(void)
{
    current.Process = &process;
    current.State = ThreadStateRunning;
    InitializeListHead(&current.OwnedMutantListHead);
    ObInitialize(); process.HandleTable = ObGetBootstrapHandleTable();
    PsInitialize();
    CheckNamedEvents();
    CheckJobs();
    CheckTokens();
    CheckRegistryDeletion();
    CheckKernelTimers(); CheckNativeTimers();
    INT64 due = -1;
    UINT64 delay[] = { 0, (UINT64)&due };
    ULONG before = blocks;
    assert((NTSTATUS)NtDelayExecution(delay) == STATUS_SUCCESS && blocks == before + 1);
    due = 0;
    assert((NTSTATUS)NtDelayExecution(delay) == STATUS_SUCCESS && yields == 1);
    delay[0] = 1;
    assert((NTSTATUS)NtDelayExecution(delay) == STATUS_SUCCESS && yields == 2);
    current.UserMode = TRUE;
    POBJECT thread_object;
    assert(ObCreateObject(g_thread_type, sizeof(THREAD_OBJECT),
                          &thread_object) == STATUS_SUCCESS);
    THREAD_OBJECT *to = (THREAD_OBJECT *)thread_object;
    to->Header.Type = ThreadObject;
    to->Thread = &current;
    HANDLE apc_handle;
    assert(ObCreateHandle(thread_object, 0x10, &apc_handle) == STATUS_SUCCESS);
    ObDereferenceObject(thread_object);
    HANDLE impersonation = NULL;
    UINT64 open_thread_token[] = { (UINT64)(ULONG_PTR)apc_handle,
                                   TOKEN_QUERY, 0, (UINT64)&impersonation };
    assert((NTSTATUS)NtOpenThreadToken(open_thread_token) == STATUS_NO_TOKEN);
    open_thread_token[0] = 0;
    assert((NTSTATUS)NtOpenThreadToken(open_thread_token) == STATUS_INVALID_HANDLE);
    UINT64 queue[] = { (UINT64)apc_handle, 0x1000, 0x2345, 0, 0 };
    assert((NTSTATUS)NtQueueApcThread(queue) == STATUS_SUCCESS);
    assert((NTSTATUS)NtTestAlert(NULL) == STATUS_USER_APC);
    assert((NTSTATUS)NtDelayExecution(delay) == STATUS_USER_APC);
    KUSER_APC delivered;
    UINT64 dequeue[] = { (UINT64)&delivered };
    assert((NTSTATUS)NtDequeueUserApc(dequeue) == STATUS_SUCCESS);
    assert(delivered.Routine == 0x1000 && delivered.Context == 0x2345);
    assert((NTSTATUS)NtTestAlert(NULL) == STATUS_SUCCESS);
    assert((NTSTATUS)NtDequeueUserApc(dequeue) == STATUS_NOT_FOUND);
    assert(ObCloseHandle(apc_handle) == STATUS_SUCCESS);
    current.UserMode = FALSE;
    assert(IsListEmpty(&g_timers));
    ObDestroyHandleTable(process.HandleTable); process.HandleTable = NULL;
    /* The bootstrap table deliberately lives forever in the kernel. */
    ExFreePool(g_bootstrap_table); g_bootstrap_table = NULL;
    POBJECT global, local;
    assert(ObLookupObjectByName("\\BaseNamedObjects\\Global", &global) == STATUS_SUCCESS);
    assert(ObLookupObjectByName("\\BaseNamedObjects\\Local", &local) == STATUS_SUCCESS);
    ObRemoveObjectName(global); ObDereferenceObject(global); ObDereferenceObject(global);
    ObRemoveObjectName(local); ObDereferenceObject(local); ObDereferenceObject(local);
    ObRemoveObjectName((POBJECT)g_named_objects);
    ObDereferenceObject((POBJECT)g_named_objects);
    ObDereferenceObject((POBJECT)ObRootDirectory);
    while (!IsListEmpty(&g_type_list)) {
        POBJECT_TYPE type = CONTAINING_RECORD(RemoveHeadList(&g_type_list), OBJECT_TYPE, TypeListEntry);
        assert(!type->TotalObjects); ExFreePool(type);
    }
    assert(!allocations);
    puts("PASS NtDelayExecution blocking/alertable APC, zero delay and no leaked objects");
    return 0;
}
