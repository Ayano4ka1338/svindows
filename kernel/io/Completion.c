/* Completion ports: waitable FIFO packet queues shared by file I/O and
 * PostQueuedCompletionStatus. All queue mutations occur with IRQs masked on
 * the current single-CPU scheduler; waiters recheck the queue after wakeup. */
#include <svindows/Io.h>
#include <svindows/Ob.h>
#include <svindows/Ke.h>
#include <svindows/Ex.h>
#include <svindows/Mm.h>
#include <svindows/Rtl.h>
#include <nt/Ntobject.h>

typedef struct _IO_COMPLETION_PACKET {
    LIST_ENTRY Entry;
    ULONG_PTR Key;
    PVOID Overlapped;
    IO_STATUS_BLOCK Result;
} IO_COMPLETION_PACKET;

typedef struct _IO_COMPLETION_PORT {
    DISPATCHER_HEADER Header;
    LIST_ENTRY Packets;
    ULONG Queued;
    ULONG Concurrency;
} IO_COMPLETION_PORT;

static POBJECT_TYPE g_completion_type;

static void IopDeleteCompletionPort(POBJECT object)
{
    IO_COMPLETION_PORT *port = object;
    while (!IsListEmpty(&port->Packets)) {
        PLIST_ENTRY entry = RemoveHeadList(&port->Packets);
        ExFreePool(CONTAINING_RECORD(entry, IO_COMPLETION_PACKET, Entry));
    }
}

void IoInitializeCompletionObjects(void)
{
    g_completion_type = ObCreateObjectType("IoCompletion", IopDeleteCompletionPort);
    if (!g_completion_type)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "cannot register IoCompletion type");
}

NTSTATUS IoReferenceCompletionPort(HANDLE handle, PVOID *object)
{
    return ObReferenceObjectByHandle(handle, 0, g_completion_type, object);
}

NTSTATUS IoPostCompletion(PVOID raw, ULONG_PTR key, PVOID overlapped,
                          NTSTATUS status, SIZE_T information)
{
    if (!raw || ObHeaderFromObject(raw)->Type != g_completion_type)
        return STATUS_INVALID_HANDLE;
    IO_COMPLETION_PACKET *packet = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*packet), 'pCoI');
    if (!packet) return STATUS_NO_MEMORY;
    packet->Key = key;
    packet->Overlapped = overlapped;
    packet->Result.Status = status;
    packet->Result.Information = information;
    IO_COMPLETION_PORT *port = raw;
    UINT64 flags = KiIrqSave();
    InsertTailList(&port->Packets, &packet->Entry);
    port->Queued++;
    port->Header.SignalState = 1;
    KiSignalObject(&port->Header);
    KiIrqRestore(flags);
    return STATUS_SUCCESS;
}

UINT64 NtCreateIoCompletion(UINT64 *a)
{
    PHANDLE out = (PHANDLE)a[0];
    if (!MmProbeForWrite((UINT64)out, sizeof(*out)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (a[2]) {
        OBJECT_ATTRIBUTES attributes;
        if (!MmProbeForRead(a[2], sizeof(attributes)))
            return (UINT64)STATUS_ACCESS_VIOLATION;
        memcpy(&attributes, (PVOID)a[2], sizeof(attributes));
        if (attributes.Length != sizeof(attributes) ||
            attributes.ObjectName || attributes.RootDirectory)
            return (UINT64)STATUS_INVALID_PARAMETER;
    }
    POBJECT object;
    NTSTATUS status = ObCreateObject(g_completion_type,
                                    sizeof(IO_COMPLETION_PORT), &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    IO_COMPLETION_PORT *port = object;
    port->Header.Type = EventNotificationObject;
    port->Header.SignalState = 0;
    InitializeListHead(&port->Header.WaitListHead);
    InitializeListHead(&port->Packets);
    port->Concurrency = (ULONG)a[3];
    HANDLE handle;
    status = ObCreateHandle(object, GENERIC_ALL, &handle);
    ObDereferenceObject(object);
    if (NT_SUCCESS(status)) *out = handle;
    return (UINT64)status;
}

UINT64 NtSetIoCompletion(UINT64 *a)
{
    POBJECT object;
    NTSTATUS status = IoReferenceCompletionPort((HANDLE)(ULONG_PTR)a[0],
                                                &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    status = IoPostCompletion(object, (ULONG_PTR)a[1], (PVOID)a[2],
                              (NTSTATUS)a[3], (SIZE_T)a[4]);
    ObDereferenceObject(object);
    return (UINT64)status;
}

UINT64 NtRemoveIoCompletion(UINT64 *a)
{
    if (!MmProbeForWrite(a[1], sizeof(PVOID)) ||
        !MmProbeForWrite(a[2], sizeof(PVOID)) ||
        !MmProbeForWrite(a[3], sizeof(IO_STATUS_BLOCK)) ||
        (a[4] && !MmProbeForRead(a[4], sizeof(INT64))))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    POBJECT object;
    NTSTATUS status = IoReferenceCompletionPort((HANDLE)(ULONG_PTR)a[0],
                                                &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    IO_COMPLETION_PORT *port = object;
    UINT64 timeout = ~(UINT64)0;
    if (a[4]) {
        INT64 due;
        memcpy(&due, (PVOID)a[4], sizeof(due));
        timeout = KeDueTimeToTicks(due);
    }
    UINT64 start = KeGetTickCount();
    for (;;) {
        UINT64 flags = KiIrqSave();
        if (!IsListEmpty(&port->Packets)) {
            PLIST_ENTRY entry = RemoveHeadList(&port->Packets);
            IO_COMPLETION_PACKET *packet = CONTAINING_RECORD(
                entry, IO_COMPLETION_PACKET, Entry);
            port->Queued--;
            if (!port->Queued) port->Header.SignalState = 0;
            KiIrqRestore(flags);
            *(PVOID *)a[1] = (PVOID)packet->Key;
            *(PVOID *)a[2] = packet->Overlapped;
            *(IO_STATUS_BLOCK *)a[3] = packet->Result;
            ExFreePool(packet);
            status = STATUS_SUCCESS;
            break;
        }
        KiIrqRestore(flags);
        UINT64 elapsed = KeGetTickCount() - start;
        if (timeout != ~(UINT64)0 && elapsed >= timeout) {
            status = STATUS_TIMEOUT;
            break;
        }
        UINT64 remaining = timeout == ~(UINT64)0 ? timeout : timeout - elapsed;
        status = KeWaitForSingleObjectTimeout(&port->Header, remaining);
        if (!NT_SUCCESS(status)) break;
    }
    ObDereferenceObject(object);
    return (UINT64)status;
}
