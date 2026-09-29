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
#include "../../kernel/ps/TimerObject.c"
#include "../../kernel/ps/Psobj.c"
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
void ObpInitializeNamespace(void) {}
BOOLEAN PsIsProcessObjectType(POBJECT_TYPE type) { (void)type; return FALSE; }
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

int main(void)
{
    current.Process = &process;
    current.State = ThreadStateRunning;
    InitializeListHead(&current.OwnedMutantListHead);
    ObInitialize(); process.HandleTable = ObGetBootstrapHandleTable();
    PsInitialize();
    CheckKernelTimers(); CheckNativeTimers();
    INT64 due = -1;
    UINT64 delay[] = { 0, (UINT64)&due };
    ULONG before = blocks;
    assert((NTSTATUS)NtDelayExecution(delay) == STATUS_SUCCESS && blocks == before + 1);
    due = 0;
    assert((NTSTATUS)NtDelayExecution(delay) == STATUS_SUCCESS && yields == 1);
    delay[0] = 1;
    assert((NTSTATUS)NtDelayExecution(delay) == STATUS_NOT_IMPLEMENTED);
    assert(IsListEmpty(&g_timers));
    ObDestroyHandleTable(process.HandleTable); process.HandleTable = NULL;
    /* The bootstrap table deliberately lives forever in the kernel. */
    ExFreePool(g_bootstrap_table); g_bootstrap_table = NULL;
    while (!IsListEmpty(&g_type_list)) {
        POBJECT_TYPE type = CONTAINING_RECORD(RemoveHeadList(&g_type_list), OBJECT_TYPE, TypeListEntry);
        assert(!type->TotalObjects); ExFreePool(type);
    }
    assert(!allocations);
    puts("PASS NtDelayExecution blocks instead of yield loop; zero delay yields; no leaked objects");
    return 0;
}
