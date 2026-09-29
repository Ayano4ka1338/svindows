/* Anonymous native Timer objects; APC/names/wake timers are unsupported. */
#include <svindows/Ps.h>
#include <svindows/Ob.h>
#include <svindows/Mm.h>
#include <svindows/Rtl.h>
#include <nt/Ntobject.h>
#include <nt/Nttimer.h>

static POBJECT_TYPE g_timer_type;
static void PspDeleteTimer(POBJECT object) { KeCancelTimer((PKTIMER)object); }
void PsInitializeTimerObjects(void)
{
    g_timer_type = ObCreateObjectType("Timer", PspDeleteTimer);
    if (!g_timer_type)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "cannot register Timer type");
}
BOOLEAN PsIsTimerObjectType(POBJECT_TYPE type) { return type == g_timer_type; }

UINT64 NtCreateTimer(UINT64 *a)
{
    if (!MmProbeForWrite(a[0], sizeof(HANDLE))) return (UINT64)STATUS_ACCESS_VIOLATION;
    if (a[3] > 1) return (UINT64)STATUS_INVALID_PARAMETER;
    if (a[2]) {
        OBJECT_ATTRIBUTES attributes;
        if (!MmProbeForRead(a[2], sizeof(attributes))) return (UINT64)STATUS_ACCESS_VIOLATION;
        memcpy(&attributes, (PVOID)a[2], sizeof(attributes));
        if (attributes.Length != sizeof(attributes)) return (UINT64)STATUS_INVALID_PARAMETER;
        if (attributes.ObjectName || attributes.RootDirectory || attributes.Attributes ||
            attributes.SecurityDescriptor || attributes.SecurityQualityOfService)
            return (UINT64)STATUS_NOT_IMPLEMENTED;
    }
    ACCESS_MASK access = (ACCESS_MASK)a[1];
    if (access & GENERIC_ALL) access = (access & ~GENERIC_ALL) | TIMER_ALL_ACCESS;
    if (access & GENERIC_READ)
        access = (access & ~GENERIC_READ) | READ_CONTROL | TIMER_QUERY_STATE;
    if (access & GENERIC_WRITE)
        access = (access & ~GENERIC_WRITE) | READ_CONTROL | TIMER_MODIFY_STATE;
    if (access & GENERIC_EXECUTE)
        access = (access & ~GENERIC_EXECUTE) | READ_CONTROL | SYNCHRONIZE;
    if (access & ~TIMER_ALL_ACCESS) return (UINT64)STATUS_ACCESS_DENIED;
    POBJECT object;
    NTSTATUS status = ObCreateObject(g_timer_type, sizeof(KTIMER), &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    KeInitializeTimerEx(object, a[3] == 0);
    HANDLE handle;
    status = ObCreateHandle(object, access, &handle);
    ObDereferenceObject(object);
    if (NT_SUCCESS(status)) memcpy((PVOID)a[0], &handle, sizeof(handle));
    return (UINT64)status;
}

UINT64 NtSetTimer(UINT64 *a)
{
    if (!MmProbeForRead(a[1], sizeof(INT64)) ||
        (a[6] && !MmProbeForWrite(a[6], sizeof(BOOLEAN))))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if ((LONG)a[5] < 0) return (UINT64)STATUS_INVALID_PARAMETER;
    if (a[2] || a[4]) return (UINT64)STATUS_NOT_IMPLEMENTED;
    INT64 due;
    memcpy(&due, (PVOID)a[1], sizeof(due));
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)a[0], TIMER_MODIFY_STATE, g_timer_type, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    UINT64 flags = KiIrqSave();
    BOOLEAN previous = ((PKTIMER)object)->Header.SignalState != 0;
    KeSetTimerEx(object, due, (ULONG)a[5], NULL);
    if (a[6]) memcpy((PVOID)a[6], &previous, sizeof(previous));
    KiIrqRestore(flags);
    ObDereferenceObject(object);
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtCancelTimer(UINT64 *a)
{
    if (a[1] && !MmProbeForWrite(a[1], sizeof(BOOLEAN))) return (UINT64)STATUS_ACCESS_VIOLATION;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)a[0], TIMER_MODIFY_STATE, g_timer_type, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    UINT64 flags = KiIrqSave();
    BOOLEAN state = ((PKTIMER)object)->Header.SignalState != 0;
    KeCancelTimer(object);
    if (a[1]) memcpy((PVOID)a[1], &state, sizeof(state));
    KiIrqRestore(flags);
    ObDereferenceObject(object);
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtQueryTimer(UINT64 *a)
{
    if (a[1] != 0) return (UINT64)STATUS_INVALID_INFO_CLASS;
    if (a[4] && !MmProbeForWrite(a[4], sizeof(ULONG))) return (UINT64)STATUS_ACCESS_VIOLATION;
    ULONG length = sizeof(TIMER_BASIC_INFORMATION);
    if (a[4]) memcpy((PVOID)a[4], &length, sizeof(length));
    if (a[3] < length) return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
    if (!MmProbeForWrite(a[2], length)) return (UINT64)STATUS_ACCESS_VIOLATION;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)a[0], TIMER_QUERY_STATE, g_timer_type, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    TIMER_BASIC_INFORMATION info = {0};
    UINT64 flags = KiIrqSave();
    PKTIMER timer = object;
    UINT64 now = KeGetTickCount();
    BOOLEAN future = timer->DueTick >= now;
    UINT64 ticks = future ? timer->DueTick - now : now - timer->DueTick;
    /* Cancellation preserves the deadline. An elapsed timer can therefore
     * report a negative difference, independently of its signal state. */
    if (ticks > 0x7FFFFFFFFFFFFFFFULL / KE_TIME_UNITS_PER_TICK)
        info.RemainingTime = future ? 0x7FFFFFFFFFFFFFFFLL : (-0x7FFFFFFFFFFFFFFFLL - 1);
    else {
        INT64 units = (INT64)(ticks * KE_TIME_UNITS_PER_TICK);
        info.RemainingTime = future ? units : -units;
    }
    info.TimerState = timer->Header.SignalState != 0;
    KiIrqRestore(flags);
    memcpy((PVOID)a[2], &info, sizeof(info));
    ObDereferenceObject(object);
    return (UINT64)STATUS_SUCCESS;
}
