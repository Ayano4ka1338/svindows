/*
 * ps/Psobj.c - Event and Thread objects, and the Nt* services that create,
 * signal, wait on, and spawn them.
 *
 * Event and Thread objects both begin their body with a DISPATCHER_HEADER, so
 * NtWaitForSingleObject can wait on either uniformly. Creating a thread here
 * spins up a new ring-3 thread in the current process (shared address space and
 * PEB) with its own stack and TEB, wrapped in a waitable Thread object that is
 * signaled when the thread exits.
 */
#include <svindows/Ps.h>
#include <svindows/Ob.h>
#include <svindows/Ke.h>
#include <svindows/Mm.h>
#include <svindows/Rtl.h>
#include <svindows/Ldr.h>
#include <svindows/Io.h>
#include <svindows/AddressSpace.h>
#include <svindows/Session.h>
#include <nt/Ntstatus.h>
#include <nt/Ntobject.h>
#include <nt/Peb.h>

typedef struct _THREAD_OBJECT {
    DISPATCHER_HEADER Header;   /* signaled on thread exit */
    PKTHREAD          Thread;
    NTSTATUS          ExitStatus;
    ULONG ProcessId, ThreadId;
    UINT64 TebBase;
    LONG Priority, BasePriority;
} THREAD_OBJECT;

static POBJECT_TYPE g_event_type;
static POBJECT_TYPE g_thread_type;
static POBJECT_TYPE g_semaphore_type;
static POBJECT_TYPE g_mutant_type;
static struct _OBJECT_DIRECTORY *g_named_objects;

NTSTATUS PsCreateThreadObject(PVOID *object_out, HANDLE *handle_out)
{
    if (!object_out) return STATUS_INVALID_PARAMETER;
    POBJECT object;
    NTSTATUS status = ObCreateObject(g_thread_type, sizeof(THREAD_OBJECT), &object);
    if (!NT_SUCCESS(status)) return status;
    THREAD_OBJECT *record = object;
    record->Header.Type = ThreadObject;
    InitializeListHead(&record->Header.WaitListHead);
    record->ExitStatus = STATUS_PENDING;
    if (handle_out) {
        status = ObCreateHandle(object, GENERIC_ALL, handle_out);
        if (!NT_SUCCESS(status)) { ObDereferenceObject(object); return status; }
    }
    *object_out = object;
    return STATUS_SUCCESS;
}

void PsAttachThreadObject(PVOID object, PKTHREAD thread)
{
    THREAD_OBJECT *record = object;
    record->Thread = thread;
    record->ExitStatus = thread->ExitStatus;
    record->ProcessId = thread->Process->ProcessId;
    record->ThreadId = thread->ThreadId;
    record->TebBase = thread->UserGsBase;
    record->Priority = thread->Priority;
    record->BasePriority = thread->Process->BasePriority;
    ObReferenceObject(object);
    PVOID previous = thread->TerminationObject;
    thread->TerminationObject = &record->Header;
    if (previous) ObDereferenceObject(previous);
}

NTSTATUS PsCaptureNamedObjectPath(POBJECT_ATTRIBUTES attributes,
                                   char *path, SIZE_T capacity)
{
    if (!attributes || !MmProbeForRead((UINT64)attributes,
                                         sizeof(*attributes)) ||
        !attributes->ObjectName ||
        !MmProbeForRead((UINT64)attributes->ObjectName,
                        sizeof(UNICODE_STRING)))
        return STATUS_INVALID_PARAMETER;
    PUNICODE_STRING name = attributes->ObjectName;
    if (!name->Length || (name->Length & 1) ||
        name->Length / 2 >= capacity ||
        !MmProbeForRead((UINT64)name->Buffer, name->Length))
        return STATUS_OBJECT_NAME_INVALID;
    SIZE_T chars = name->Length / 2;
    for (SIZE_T i = 0; i < chars; i++) {
        UINT16 c = name->Buffer[i];
        if (c < 32 || c > 127) return STATUS_OBJECT_NAME_INVALID;
        path[i] = (char)c;
    }
    path[chars] = 0;
    static const char prefix[] = "\\BaseNamedObjects\\";
    if (chars < sizeof(prefix)) return STATUS_OBJECT_NAME_INVALID;
    for (SIZE_T i = 0; i < sizeof(prefix) - 1; i++)
        if (path[i] != prefix[i]) return STATUS_OBJECT_NAME_INVALID;
    const char *leaf = path + sizeof(prefix) - 1;
    if (leaf[0] == 'G' && leaf[1] == 'l' && leaf[2] == 'o' &&
        leaf[3] == 'b' && leaf[4] == 'a' && leaf[5] == 'l' &&
        leaf[6] == '\\') leaf += 7;
    else if (leaf[0] == 'L' && leaf[1] == 'o' && leaf[2] == 'c' &&
             leaf[3] == 'a' && leaf[4] == 'l' && leaf[5] == '\\') leaf += 6;
    if (!*leaf) return STATUS_OBJECT_NAME_INVALID;
    for (; *leaf; leaf++)
        if (*leaf == '\\' || *leaf == '/') return STATUS_OBJECT_NAME_INVALID;
    return STATUS_SUCCESS;
}

NTSTATUS PsValidateThreadHandle(HANDLE handle)
{
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_thread_type,
                                                &object);
    if (NT_SUCCESS(status)) ObDereferenceObject(object);
    return status;
}

/* NtOpenThread(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PCLIENT_ID). */
UINT64 NtOpenThread(UINT64 *a)
{
    typedef struct { HANDLE Process; HANDLE Thread; } CLIENT_ID_LOCAL;
    PHANDLE out = (PHANDLE)a[0];
    const CLIENT_ID_LOCAL *id = (const CLIENT_ID_LOCAL *)a[3];
    if (!MmProbeForWrite((UINT64)out, sizeof(HANDLE)) ||
        !MmProbeForRead((UINT64)id, sizeof(*id)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    *out = NULL;
    BOOLEAN inherit = FALSE;
    if (a[2]) {
        OBJECT_ATTRIBUTES attributes;
        if (!MmProbeForRead(a[2], sizeof(attributes))) return (UINT64)STATUS_ACCESS_VIOLATION;
        memcpy(&attributes, (void *)a[2], sizeof(attributes));
        if (attributes.Length != sizeof(attributes) || attributes.ObjectName || attributes.RootDirectory ||
            (attributes.Attributes & 0x200u)) return (UINT64)STATUS_INVALID_PARAMETER;
        inherit = (attributes.Attributes & 2u) != 0;
    }
    PKTHREAD thread = KeLookupThreadById((ULONG)(ULONG_PTR)id->Thread);
    if (!thread || (id->Process && (ULONG_PTR)id->Process !=
                    thread->Process->ProcessId))
        return (UINT64)STATUS_INVALID_HANDLE;

    POBJECT object;
    BOOLEAN created = FALSE;
    if (thread->TerminationObject &&
        ObHeaderFromObject(thread->TerminationObject)->Type == g_thread_type) {
        object = (POBJECT)thread->TerminationObject;
    } else {
        NTSTATUS status = ObCreateObject(g_thread_type, sizeof(THREAD_OBJECT),
                                         &object);
        if (!NT_SUCCESS(status)) return (UINT64)status;
        THREAD_OBJECT *to = (THREAD_OBJECT *)object;
        to->Header.Type = ThreadObject;
        to->Header.SignalState = 0;
        InitializeListHead(&to->Header.WaitListHead);
        to->Thread = thread;
        to->ExitStatus = thread->ExitStatus;
        created = TRUE;
    }
    HANDLE handle;
    PKTHREAD caller = KeGetCurrentThread();
    SE_DESCRIPTOR *descriptor = SeReferenceObjectDescriptor(object);
    ACCESS_MASK granted = 0;
    NTSTATUS status = SeAccessCheck(descriptor, caller->Process->SecurityUserRid, (ACCESS_MASK)a[1],
                                    &g_thread_type->GenericMapping, &granted);
    SeDereferenceDescriptor(descriptor);
    if (NT_SUCCESS(status)) status = ObCreateHandle(object, granted, &handle);
    if (NT_SUCCESS(status)) {
        if (created) {
            PsAttachThreadObject(object, thread);
        }
        *out = handle;
        if (inherit) ObSetHandleAttributesInTable(caller->Process->HandleTable, handle,
                                                   OB_HANDLE_INHERIT, OB_HANDLE_INHERIT);
    }
    if (created) ObDereferenceObject(object);
    return (UINT64)status;
}

/* PE images commonly reserve a 1 MiB user stack. Quake's PAK loader alone
 * uses a 128 KiB local table, so the old 64 KiB stack could not start it. */
#define THREAD_STACK_PAGES 256
#define THREAD_SET_CONTEXT 0x0010u
#define THREAD_SUSPEND_RESUME 0x0002u

static NTSTATUS reference_live_thread(HANDLE handle, ACCESS_MASK access,
                                      POBJECT *referenced, PKTHREAD *thread)
{
    *referenced = NULL;
    *thread = NULL;
    if ((ULONG_PTR)handle == ~(ULONG_PTR)1) {
        *thread = KeGetCurrentThread();
        return *thread ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
    }
    NTSTATUS status = ObReferenceObjectByHandle(handle, access,
                                                g_thread_type, referenced);
    if (!NT_SUCCESS(status)) return status;
    THREAD_OBJECT *target = (THREAD_OBJECT *)*referenced;
    *thread = target->Header.SignalState ? NULL : target->Thread;
    if (!*thread) {
        ObDereferenceObject(*referenced);
        *referenced = NULL;
        return STATUS_INVALID_HANDLE;
    }
    return STATUS_SUCCESS;
}

static LONG effective_thread_priority(PKTHREAD thread, LONG relative)
{
    LONG base = thread->Process->BasePriority;
    LONG value = base + relative;
    LONG minimum = thread->Process->PriorityClass == 4 ? 16 : 1;
    LONG maximum = thread->Process->PriorityClass == 4 ? 31 : 15;
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    return value;
}

UINT64 NtQueryThreadPriority(UINT64 *a)
{
    LONG *out = (LONG *)a[1];
    if (!MmProbeForWrite((UINT64)out, sizeof(*out)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    POBJECT referenced;
    PKTHREAD thread;
    NTSTATUS status = reference_live_thread((HANDLE)(ULONG_PTR)a[0], 0x40,
                                            &referenced, &thread);
    if (NT_SUCCESS(status)) {
        *out = thread->RelativePriority;
        if (referenced) ObDereferenceObject(referenced);
    }
    return (UINT64)status;
}

UINT64 NtSetThreadPriority(UINT64 *a)
{
    LONG relative = (LONG)a[1];
    if (relative < -15 || relative > 15)
        return (UINT64)STATUS_INVALID_PARAMETER;
    POBJECT referenced;
    PKTHREAD thread;
    NTSTATUS status = reference_live_thread((HANDLE)(ULONG_PTR)a[0], 0x20,
                                            &referenced, &thread);
    if (NT_SUCCESS(status)) {
        thread->RelativePriority = relative;
        thread->Priority = effective_thread_priority(thread, relative);
        if (referenced) ObDereferenceObject(referenced);
    }
    return (UINT64)status;
}

static UINT64 change_thread_suspend_count(UINT64 *a, BOOLEAN resume)
{
    PULONG previous = (PULONG)a[1];
    if (previous && !MmProbeForWrite((UINT64)previous, sizeof(*previous)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)a[0],
                                                THREAD_SUSPEND_RESUME,
                                                g_thread_type, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    THREAD_OBJECT *target = (THREAD_OBJECT *)object;
    PKTHREAD thread = target->Header.SignalState ? NULL : target->Thread;
    ULONG count = 0;
    status = resume ? KeResumeThread(thread, &count)
                    : KeSuspendThread(thread, &count);
    if (NT_SUCCESS(status) && previous) *previous = count;
    ObDereferenceObject(object);
    return (UINT64)status;
}

UINT64 NtSuspendThread(UINT64 *a)
{
    return change_thread_suspend_count(a, FALSE);
}

UINT64 NtResumeThread(UINT64 *a)
{
    return change_thread_suspend_count(a, TRUE);
}

NTSTATUS PsTerminateThreadByHandle(HANDLE handle, NTSTATUS exit_status)
{
    POBJECT referenced;
    PKTHREAD thread;
    NTSTATUS status = reference_live_thread(handle, 0x0001,
                                            &referenced, &thread);
    if (!NT_SUCCESS(status)) return status;
    status = KeTerminateThreadByPointer(thread, exit_status);
    if (referenced) ObDereferenceObject(referenced);
    return status;
}

void PsInitialize(void)
{
    PsInitializeTimerObjects();
    PsInitializeTokens();
    g_event_type = ObCreateObjectType("Event", NULL);
    SeConfigureType(g_event_type, 0x20001u, 0x20002u, 0x120000u, 0x1F0003u);
    if (g_event_type) g_event_type->UnlinkNameOnLastHandle = TRUE;
    g_thread_type = ObCreateObjectType("Thread", NULL);
    SeConfigureType(g_thread_type, 0x20048u, 0x20337u, 0x120000u, 0x1FFFFFu);
    g_semaphore_type = ObCreateObjectType("Semaphore", NULL);
    SeConfigureType(g_semaphore_type, 0x20001u, 0x20002u, 0x120000u, 0x1F0003u);
    g_mutant_type = ObCreateObjectType("Mutant", NULL);
    SeConfigureType(g_mutant_type, 0x20001u, 0x20000u, 0x120000u, 0x1F0001u);
    PsInitializeJobObjects();
    if (!NT_SUCCESS(ObCreateDirectory(ObRootDirectory, "BaseNamedObjects",
                                      &g_named_objects)))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "named object directory");
    struct _OBJECT_DIRECTORY *subdir;
    if (!NT_SUCCESS(ObCreateDirectory(g_named_objects, "Global", &subdir)) ||
        !NT_SUCCESS(ObCreateDirectory(g_named_objects, "Local", &subdir)))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "named event scopes");
}

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

/* NtCreateEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, EVENT_TYPE, InitialState).
 * EVENT_TYPE 0 = notification (manual reset), 1 = synchronization (auto). */
UINT64 NtCreateEvent(UINT64 *a)
{
    PHANDLE out = (PHANDLE)a[0];
    UINT64 event_type = a[3], initial = a[4];
    if (event_type > 1) return (UINT64)STATUS_INVALID_PARAMETER;
    if (!MmProbeForWrite((UINT64)out, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    char path[260];
    BOOLEAN named = a[2] != 0;
    if (named) {
        NTSTATUS status = PsCaptureNamedObjectPath((POBJECT_ATTRIBUTES)a[2],
                                              path, sizeof(path));
        if (!NT_SUCCESS(status)) return (UINT64)status;
        POBJECT existing;
        status = ObLookupObjectByName(path, &existing);
        if (NT_SUCCESS(status)) {
            if (ObHeaderFromObject(existing)->Type != g_event_type) {
                ObDereferenceObject(existing);
                return (UINT64)STATUS_OBJECT_TYPE_MISMATCH;
            }
            HANDLE found;
            status = ObCreateHandle(existing, (ACCESS_MASK)a[1], &found);
            ObDereferenceObject(existing);
            if (NT_SUCCESS(status)) *out = found;
            return NT_SUCCESS(status) ? 0x40000000u : (UINT64)status;
        }
        if (status != STATUS_OBJECT_NAME_NOT_FOUND)
            return (UINT64)status;
    }

    POBJECT obj;
    if (!NT_SUCCESS(ObCreateObject(g_event_type, sizeof(KEVENT) +
                                    (named ? sizeof(path) : 0), &obj)))
        return (UINT64)STATUS_NO_MEMORY;
    KeInitializeEvent((PKEVENT)obj, (BOOLEAN)(event_type == 0), (BOOLEAN)initial);

    if (named) {
        char *last = path;
        for (char *p = path; *p; p++) if (*p == '\\') last = p;
        if (last == path || !last[1]) {
            ObDereferenceObject(obj); return (UINT64)STATUS_OBJECT_NAME_INVALID;
        }
        *last = 0;
        POBJECT parent;
        NTSTATUS status = ObLookupObjectByName(path, &parent);
        if (!NT_SUCCESS(status)) {
            ObDereferenceObject(obj); return (UINT64)status;
        }
        char *owned_name = (char *)obj + sizeof(KEVENT);
        SIZE_T i = 0;
        do { owned_name[i] = last[1+i]; } while (owned_name[i++]);
        status = ObInsertObjectByName((struct _OBJECT_DIRECTORY *)parent,
                                      owned_name, obj);
        ObDereferenceObject(parent);
        if (!NT_SUCCESS(status)) {
            ObDereferenceObject(obj); return (UINT64)status;
        }
    }

    HANDLE h;
    if (!NT_SUCCESS(ObCreateHandle(obj, (ACCESS_MASK)a[1], &h))) {
        if (named) ObRemoveObjectName(obj);
        ObDereferenceObject(obj);
        return (UINT64)STATUS_NO_MEMORY;
    }
    ObDereferenceObject(obj); /* the handle keeps it alive */
    *out = h;
    KeLog("[ps]   NtCreateEvent -> handle %p (thread %u)\n", (void *)h,
          KeGetCurrentThread() ? KeGetCurrentThread()->ThreadId : 0);
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtOpenEvent(UINT64 *a)
{
    PHANDLE out = (PHANDLE)a[0];
    if (!MmProbeForWrite((UINT64)out, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    *out = NULL;
    char path[260];
    NTSTATUS status = PsCaptureNamedObjectPath((POBJECT_ATTRIBUTES)a[2],
                                          path, sizeof(path));
    if (!NT_SUCCESS(status)) return (UINT64)status;
    POBJECT object;
    status = ObLookupObjectByName(path, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    if (ObHeaderFromObject(object)->Type != g_event_type) {
        ObDereferenceObject(object);
        return (UINT64)STATUS_OBJECT_TYPE_MISMATCH;
    }
    HANDLE handle;
    status = ObCreateHandle(object, (ACCESS_MASK)a[1], &handle);
    ObDereferenceObject(object);
    if (NT_SUCCESS(status)) *out = handle;
    return (UINT64)status;
}

/*
 * Kernel-owned notification event with a ring-3 handle. USER needs one per GUI
 * thread: MsgWaitForMultipleObjects waits on the thread's "input event", which
 * win32k signals when a message lands in that thread's queue. The kernel keeps
 * the object pointer so it can signal without a handle lookup.
 */
NTSTATUS PsCreateNotificationEvent(PKEVENT *event_out, HANDLE *handle_out)
{
    POBJECT obj;
    if (!NT_SUCCESS(ObCreateObject(g_event_type, sizeof(KEVENT), &obj)))
        return STATUS_NO_MEMORY;
    KeInitializeEvent((PKEVENT)obj, TRUE /* notification */, FALSE);

    HANDLE h;
    if (!NT_SUCCESS(ObCreateHandle(obj, GENERIC_ALL, &h))) {
        ObDereferenceObject(obj);
        return STATUS_NO_MEMORY;
    }
    /* Transfer the creation reference to the kernel caller. The handle has
     * its own reference and may be closed independently by user mode. */
    *event_out = (PKEVENT)obj;
    *handle_out = h;
    return STATUS_SUCCESS;
}

/* NtSetEvent(HANDLE, PLONG PreviousState). */
UINT64 NtSetEvent(UINT64 *a)
{
    UINT64 handle = a[0];
    LONG  *prev_out = (LONG *)a[1];

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)handle, 0,
                                              g_event_type, &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;

    KeLog("[ps]   NtSetEvent(handle %p, thread %u)\n", (void *)handle,
          KeGetCurrentThread() ? KeGetCurrentThread()->ThreadId : 0);
    LONG previous = KeSetEvent((PKEVENT)obj);
    ObDereferenceObject(obj);
    if (prev_out && MmProbeForWrite((UINT64)prev_out, sizeof(LONG)))
        *prev_out = previous;
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtResetEvent(UINT64 *a)
{
    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)a[0], 0,
                                              g_event_type, &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;
    PKEVENT event = (PKEVENT)obj;
    LONG previous = event->Header.SignalState;
    KeResetEvent(event);
    LONG *previous_out = (LONG *)a[1];
    if (previous_out && MmProbeForWrite((UINT64)previous_out, sizeof(LONG)))
        *previous_out = previous;
    ObDereferenceObject(obj);
    return (UINT64)STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Semaphores                                                         */
/* ------------------------------------------------------------------ */

UINT64 NtCreateSemaphore(UINT64 *a)
{
    PHANDLE out = (PHANDLE)a[0];
    LONG initial = (LONG)a[3];
    LONG limit = (LONG)a[4];
    if (!MmProbeForWrite((UINT64)out, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (limit <= 0 || initial < 0 || initial > limit)
        return (UINT64)STATUS_INVALID_PARAMETER;

    POBJECT obj;
    if (!NT_SUCCESS(ObCreateObject(g_semaphore_type, sizeof(KSEMAPHORE), &obj)))
        return (UINT64)STATUS_NO_MEMORY;
    KeInitializeSemaphore((PKSEMAPHORE)obj, initial, limit);

    HANDLE handle;
    NTSTATUS status = ObCreateHandle(obj, GENERIC_ALL, &handle);
    ObDereferenceObject(obj);
    if (!NT_SUCCESS(status))
        return (UINT64)status;
    *out = handle;
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtReleaseSemaphore(UINT64 *a)
{
    LONG release = (LONG)a[1];
    if (release <= 0)
        return (UINT64)STATUS_INVALID_PARAMETER;

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)a[0], 0,
                                              g_semaphore_type, &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;
    PKSEMAPHORE semaphore = (PKSEMAPHORE)obj;
    if (semaphore->Header.SignalState > semaphore->Limit - release) {
        ObDereferenceObject(obj);
        return (UINT64)STATUS_INVALID_PARAMETER;
    }
    LONG previous = KeReleaseSemaphore(semaphore, release);
    LONG *previous_out = (LONG *)a[2];
    if (previous_out && MmProbeForWrite((UINT64)previous_out, sizeof(LONG)))
        *previous_out = previous;
    ObDereferenceObject(obj);
    return (UINT64)STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Mutants                                                            */
/* ------------------------------------------------------------------ */

UINT64 NtCreateMutant(UINT64 *a)
{
    PHANDLE out = (PHANDLE)a[0];
    if (!MmProbeForWrite((UINT64)out, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    POBJECT object;
    NTSTATUS status = ObCreateObject(g_mutant_type, sizeof(KMUTANT), &object);
    if (!NT_SUCCESS(status))
        return (UINT64)status;
    KeInitializeMutantWithReference((PKMUTANT)object, (BOOLEAN)a[3], object);

    HANDLE handle;
    status = ObCreateHandle(object, GENERIC_ALL, &handle);
    ObDereferenceObject(object);
    if (!NT_SUCCESS(status))
        return (UINT64)status;
    *out = handle;
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtReleaseMutant(UINT64 *a)
{
    POBJECT object;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)a[0], 0,
                                              g_mutant_type, &object)))
        return (UINT64)STATUS_INVALID_HANDLE;
    LONG previous = 0;
    BOOLEAN released = KeReleaseMutant((PKMUTANT)object, &previous);
    LONG *previous_out = (LONG *)a[1];
    if (released && previous_out &&
        MmProbeForWrite((UINT64)previous_out, sizeof(*previous_out)))
        *previous_out = previous;
    ObDereferenceObject(object);
    return released ? (UINT64)STATUS_SUCCESS : (UINT64)STATUS_ACCESS_DENIED;
}

/* ------------------------------------------------------------------ */
/* Waiting                                                            */
/* ------------------------------------------------------------------ */

static UINT64 timeout_to_ticks(const INT64 *timeout)
{
    if (!timeout)
        return ~(UINT64)0; /* infinite */
    if (!MmProbeForRead((UINT64)timeout, sizeof(*timeout)))
        return ~(UINT64)0;

    INT64 value;
    memcpy(&value, timeout, sizeof(value));
    return KeDueTimeToTicks(value);
}

static NTSTATUS reference_waitable(HANDLE handle, POBJECT *object,
                                   PDISPATCHER_HEADER *header)
{
    POBJECT obj;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, NULL, &obj);
    if (!NT_SUCCESS(status))
        return STATUS_INVALID_HANDLE;

    POBJECT_TYPE type = ObHeaderFromObject(obj)->Type;
    if (type != g_event_type && type != g_thread_type &&
        type != g_semaphore_type && type != g_mutant_type &&
        !PsIsProcessObjectType(type) && !PsIsTimerObjectType(type)) {
        ObDereferenceObject(obj);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    *object = obj;
    if (PsIsTimerObjectType(type)) {
        POBJECT checked;
        NTSTATUS access = ObReferenceObjectByHandle(handle, SYNCHRONIZE, type, &checked);
        if (!NT_SUCCESS(access)) { ObDereferenceObject(obj); return access; }
        ObDereferenceObject(checked);
    }
    *header = (PDISPATCHER_HEADER)obj;
    return STATUS_SUCCESS;
}

/* NtWaitForSingleObject(HANDLE, BOOLEAN Alertable, PLARGE_INTEGER Timeout). */
UINT64 NtWaitForSingleObject(UINT64 *a)
{
    if (a[2] && !MmProbeForRead(a[2], sizeof(INT64)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    POBJECT obj;
    PDISPATCHER_HEADER header;
    NTSTATUS st = reference_waitable((HANDLE)(ULONG_PTR)a[0], &obj, &header);
    if (!NT_SUCCESS(st))
        return (UINT64)st;

    /* Diagnostics: an indefinite wait is the signature of a stuck user
     * thread; report each begin/end pair. */
    BOOLEAN indefinite = !a[2];
    UINT64 tid = 0;
    PKTHREAD thread = KeGetCurrentThread();
    if (thread)
        tid = thread->ThreadId;
    if (indefinite) {
        static UINT64 logged[64];
        static ULONG logged_count;
        BOOLEAN seen = FALSE;
        for (ULONG i = 0; i < logged_count; i++)
            if (logged[i] == (tid << 32 | (UINT32)(ULONG_PTR)a[0]))
                seen = TRUE;
        if (!seen && logged_count < 64) {
            logged[logged_count++] = tid << 32 | (UINT32)(ULONG_PTR)a[0];
            KeLog("[ps]   thread %llu begins indefinite wait on handle %p\n",
                  (unsigned long long)tid, (void *)(ULONG_PTR)a[0]);
        }
    }

    st = KeWaitForSingleObjectTimeoutAlertable(
        header, timeout_to_ticks((const INT64 *)a[2]), (BOOLEAN)(a[1] != 0));
    if (indefinite)
        KeLog("[ps]   thread %llu resumed from indefinite wait on %p "
              "(status 0x%08lx)\n", (unsigned long long)tid,
              (void *)(ULONG_PTR)a[0], (unsigned long)st);
    ObDereferenceObject(obj);
    return (UINT64)st;
}

/* SVINDOWS-private service used by kernel32 until the native system-service table
 * is switched to a specific Windows build's NtWaitForMultipleObjects number.
 * Arguments mirror the native routine closely: count, handle array, WaitType
 * (0=all, 1=any), alertable, timeout. */
UINT64 NtWaitForMultipleObjects(UINT64 *a)
{
    ULONG count = (ULONG)a[0];
    HANDLE *handles = (HANDLE *)a[1];
    ULONG wait_type = (ULONG)a[2];
    const INT64 *timeout = (const INT64 *)a[4];
    if (timeout && !MmProbeForRead((UINT64)timeout, sizeof(*timeout)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    if (!count || count > KE_MAXIMUM_WAIT_OBJECTS || wait_type > 1)
        return (UINT64)STATUS_INVALID_PARAMETER;
    if (!MmProbeForRead((UINT64)handles, count * sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    POBJECT objects[KE_MAXIMUM_WAIT_OBJECTS];
    PDISPATCHER_HEADER headers[KE_MAXIMUM_WAIT_OBJECTS];
    ULONG referenced = 0;
    NTSTATUS status = STATUS_SUCCESS;
    for (; referenced < count; referenced++) {
        status = reference_waitable(handles[referenced], &objects[referenced],
                                    &headers[referenced]);
        if (!NT_SUCCESS(status))
            break;
    }

    if (NT_SUCCESS(status)) {
        /* Same diagnostic as the single-object wait: a blocking multi-object
         * wait is where a stuck GUI thread ends up (MsgWaitForMultipleObjects
         * waits on the caller's handles plus the USER input event), so report
         * the handle set once per (thread, count, first handle). */
        UINT64 tid = 0;
        PKTHREAD thread = KeGetCurrentThread();
        if (thread)
            tid = thread->ThreadId;
        static UINT64 logged[64];
        static ULONG logged_count;
        UINT64 key = tid << 40 | (UINT64)count << 32 |
                     (UINT32)(ULONG_PTR)handles[0];
        BOOLEAN seen = FALSE;
        for (ULONG i = 0; i < logged_count; i++)
            if (logged[i] == key)
                seen = TRUE;
        if (!seen && logged_count < 64) {
            logged[logged_count++] = key;
            KeLog("[ps]   thread %llu waits on %lu object(s) (%s, %s): "
                  "%p %p %p %p\n", (unsigned long long)tid,
                  (unsigned long)count, wait_type == 0 ? "all" : "any",
                  timeout ? "timed" : "indefinite",
                  (void *)(ULONG_PTR)handles[0],
                  (void *)(count > 1 ? (ULONG_PTR)handles[1] : 0),
                  (void *)(count > 2 ? (ULONG_PTR)handles[2] : 0),
                  (void *)(count > 3 ? (ULONG_PTR)handles[3] : 0));
        }
        status = KeWaitForMultipleObjectsAlertable(
            count, headers, (BOOLEAN)(wait_type == 0),
            timeout_to_ticks(timeout), (BOOLEAN)(a[3] != 0));
    }
    while (referenced)
        ObDereferenceObject(objects[--referenced]);
    return (UINT64)status;
}

/* Native user APCs run only when the target enters an alertable wait. The
 * caller's handle table is process-local, so cross-process queuing is not
 * exposed until Ob supports cross-process handles and access checks. */
UINT64 NtQueueApcThread(UINT64 *a)
{
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)a[0],
                                                THREAD_SET_CONTEXT,
                                                g_thread_type, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    THREAD_OBJECT *target = (THREAD_OBJECT *)object;
    PKTHREAD thread = target->Header.SignalState ? NULL : target->Thread;
    PKTHREAD caller = KeGetCurrentThread();
    if (!thread || !caller || thread->Process != caller->Process)
        status = STATUS_INVALID_PARAMETER;
    else if (!a[1] || !MmProbeForRead(a[1], 1))
        status = STATUS_ACCESS_VIOLATION;
    else {
        KUSER_APC apc = { a[1], a[2], a[3], (UINT32)a[4] };
        status = KeQueueUserApc(thread, &apc);
    }
    ObDereferenceObject(object);
    return (UINT64)status;
}

/* Private handoff to ntdll's user-mode APC dispatcher. Probe before removing
 * the record: a bad caller buffer must not silently consume a queued APC. */
UINT64 NtDequeueUserApc(UINT64 *a)
{
    if (!MmProbeForWrite(a[0], sizeof(KUSER_APC)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    KUSER_APC apc;
    NTSTATUS status = KeDequeueUserApc(KeGetCurrentThread(), &apc);
    if (NT_SUCCESS(status))
        memcpy((void *)a[0], &apc, sizeof(apc));
    return (UINT64)status;
}

UINT64 NtTestAlert(UINT64 *a)
{
    (void)a;
    return (UINT64)(KeHasUserApc(KeGetCurrentThread()) ?
                    STATUS_USER_APC : STATUS_SUCCESS);
}

/* NtQueryInformationProcess: the first class required by real SHCORE during
 * DLL initialization is ProcessBasicInformation (0). */
UINT64 NtQueryInformationProcess(UINT64 *a)
{
    typedef struct { UINT8 Foreground, PriorityClass; } PRIORITY_CLASS_INFO;
    typedef struct _PROCESS_BASIC_INFORMATION_LOCAL {
        NTSTATUS ExitStatus;
        UINT32 Padding;
        PVOID PebBaseAddress;
        UINT64 AffinityMask;
        LONG BasePriority;
        UINT32 Padding2;
        UINT64 UniqueProcessId;
        UINT64 InheritedFromUniqueProcessId;
    } PROCESS_BASIC_INFORMATION_LOCAL;

    ULONG info_class = (ULONG)a[1];
    void *buffer = (void *)a[2];
    ULONG length = (ULONG)a[3];
    ULONG *return_length = (ULONG *)a[4];
    if (info_class != 0 && info_class != 18 && info_class != 24 && info_class != 27 &&
        info_class != 0x1000 && info_class != 0x1001)
        return (UINT64)STATUS_INVALID_INFO_CLASS;
    ULONG required = info_class == 0x1001 || info_class == 24 ? sizeof(ULONG) :
                     info_class == 0x1000 ? 2 * sizeof(ULONG) :
                     info_class == 18 ? sizeof(PRIORITY_CLASS_INFO) :
                                      sizeof(PROCESS_BASIC_INFORMATION_LOCAL);
    if (return_length && !MmProbeForWrite((UINT64)return_length, sizeof(ULONG)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (return_length && info_class != 27)
        *return_length = required;
    if (info_class != 27 && length < required)
        return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
    if (info_class != 27 && !MmProbeForWrite((UINT64)buffer, required))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    PKPROCESS process = NULL;
    POBJECT referenced = NULL;
    HANDLE handle = (HANDLE)(ULONG_PTR)a[0];
    if ((ULONG_PTR)handle == ~(ULONG_PTR)0) {
        PKTHREAD thread = KeGetCurrentThread();
        process = thread ? thread->Process : NULL;
    } else if (NT_SUCCESS(ObReferenceObjectByHandle(handle, 0, NULL,
                                                    &referenced))) {
        process = PsGetProcessFromObject(referenced);
    }
    if (!process) {
        if (referenced)
            ObDereferenceObject(referenced);
        return (UINT64)STATUS_INVALID_HANDLE;
    }

    if (info_class == 27) {
        const char *path = process->ImagePath;
        ULONG chars = 0;
        while (path[chars] && chars < 259) chars++;
        required = sizeof(UNICODE_STRING) + (chars + 1) * sizeof(UINT16);
        if (return_length) *return_length = required;
        if (length < required) {
            if (referenced) ObDereferenceObject(referenced);
            return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!MmProbeForWrite((UINT64)buffer, required)) {
            if (referenced) ObDereferenceObject(referenced);
            return (UINT64)STATUS_ACCESS_VIOLATION;
        }
        UNICODE_STRING *name = buffer;
        UINT16 *wide = (UINT16 *)((UINT8 *)buffer + sizeof(*name));
        name->Length = (UINT16)(chars * sizeof(UINT16));
        name->MaximumLength = (UINT16)((chars + 1) * sizeof(UINT16));
        name->Buffer = wide;
        for (ULONG i = 0; i < chars; i++) wide[i] = (UINT8)path[i];
        wide[chars] = 0;
        if (referenced) ObDereferenceObject(referenced);
        return (UINT64)STATUS_SUCCESS;
    }

    if (info_class == 24) {
        *(ULONG *)buffer = process->WindowsSessionId;
        if (referenced) ObDereferenceObject(referenced);
        return (UINT64)STATUS_SUCCESS;
    }
    if (info_class == 18) {
        PRIORITY_CLASS_INFO *info = buffer;
        info->Foreground = 0;
        info->PriorityClass = process->PriorityClass;
        if (referenced) ObDereferenceObject(referenced);
        return (UINT64)STATUS_SUCCESS;
    }
    if (info_class == 0x1000) {
        ULONG *parameters = buffer;
        parameters[0] = process->ShutdownLevel;
        parameters[1] = process->ShutdownFlags;
        if (referenced) ObDereferenceObject(referenced);
        return (UINT64)STATUS_SUCCESS;
    }
    if (info_class == 0x1001) {
        *(ULONG *)buffer = process->ErrorMode;
        if (referenced) ObDereferenceObject(referenced);
        return (UINT64)STATUS_SUCCESS;
    }
    PROCESS_BASIC_INFORMATION_LOCAL *info = buffer;
    memset(info, 0, sizeof(*info));
    info->ExitStatus = process->ExitStatus;
    info->PebBaseAddress = (PVOID)PROCESS_PEB_VA;
    info->AffinityMask = 1;
    info->BasePriority = process->BasePriority;
    info->UniqueProcessId = process->ProcessId;
    info->InheritedFromUniqueProcessId = process->ParentProcessId;
    if (referenced)
        ObDereferenceObject(referenced);
    return (UINT64)STATUS_SUCCESS;
}

/* ProcessPriorityClass (18): update both the stored class and the runnable
 * threads' effective base priorities. The scheduler selects the highest ready
 * priority, with FIFO rotation among threads at the same level. */
UINT64 NtSetInformationProcess(UINT64 *a)
{
    typedef struct { UINT8 Foreground, PriorityClass; } PRIORITY_CLASS_INFO;
    ULONG info_class = (ULONG)a[1];
    /* Private current-directory update. The buffer is a 260 WCHAR in/out
     * path: return the canonical DOS path so the caller can update its PEB. */
    if (info_class == 0x1002) {
        if ((ULONG_PTR)a[0] != ~(ULONG_PTR)0)
            return (UINT64)STATUS_INVALID_HANDLE;
        if ((ULONG)a[3] != 260 * sizeof(UINT16))
            return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
        if (!MmProbeForRead(a[2], 260 * sizeof(UINT16)) ||
            !MmProbeForWrite(a[2], 260 * sizeof(UINT16)))
            return (UINT64)STATUS_ACCESS_VIOLATION;
        PKPROCESS process = KeGetCurrentThread()->Process;
        if (!process) return (UINT64)STATUS_INVALID_HANDLE;
        UINT16 *wide = (UINT16 *)a[2];
        char input[260], canonical[260];
        UINT32 n = 0;
        while (n < 260 && wide[n]) {
            if (wide[n] < 32 || wide[n] > 127)
                return (UINT64)STATUS_OBJECT_NAME_INVALID;
            input[n] = (char)wide[n];
            n++;
        }
        if (!n || n == 260)
            return (UINT64)STATUS_INVALID_PARAMETER;
        input[n] = 0;
        if (!IoResolveDosPathKernel(input, process->CurrentDirectory,
                                    canonical))
            return (UINT64)STATUS_OBJECT_NAME_INVALID;
        FAT_FIND_DATA entry;
        NTSTATUS status = FatQueryPath(canonical, &entry);
        if (!NT_SUCCESS(status)) return (UINT64)status;
        if (!(entry.Attributes & 0x10))
            return (UINT64)STATUS_OBJECT_PATH_NOT_FOUND;
        memcpy(process->CurrentDirectory, canonical, strlen(canonical) + 1);
        UINT32 i = 0;
        for (; canonical[i]; i++) wide[i] = (UINT8)canonical[i];
        wide[i] = 0;
        return (UINT64)STATUS_SUCCESS;
    }
    if (info_class != 18 && info_class != 0x1000 && info_class != 0x1001)
        return (UINT64)STATUS_INVALID_INFO_CLASS;
    ULONG required = info_class == 0x1001 ? 2 * sizeof(ULONG) :
                     info_class == 0x1000 ? 2 * sizeof(ULONG) :
                                          sizeof(PRIORITY_CLASS_INFO);
    if ((ULONG)a[3] != required)
        return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
    if (!MmProbeForRead(a[2], required))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (info_class == 0x1001) {
        if (!MmProbeForWrite(a[2], required))
            return (UINT64)STATUS_ACCESS_VIOLATION;
        if ((ULONG_PTR)a[0] != ~(ULONG_PTR)0)
            return (UINT64)STATUS_INVALID_HANDLE;
        ULONG *parameters = (ULONG *)a[2];
        ULONG requested = parameters[0];
        if (requested & ~(1u | 2u | 4u | 0x8000u))
            return (UINT64)STATUS_INVALID_PARAMETER;
        PKPROCESS current = KeGetCurrentThread()->Process;
        if (!current) return (UINT64)STATUS_INVALID_HANDLE;
        ULONG old;
        do {
            old = current->ErrorMode;
        } while (!__sync_bool_compare_and_swap(&current->ErrorMode, old,
                                                requested | (old & 4u)));
        parameters[1] = old;
        return (UINT64)STATUS_SUCCESS;
    }
    if (info_class == 0x1000) {
        const ULONG *parameters = (const ULONG *)a[2];
        ULONG level = parameters[0], flags = parameters[1];
        if (level > 0x4ff || (flags & ~1u))
            return (UINT64)STATUS_INVALID_PARAMETER;
        if ((ULONG_PTR)a[0] != ~(ULONG_PTR)0)
            return (UINT64)STATUS_INVALID_HANDLE;
        PKPROCESS current = KeGetCurrentThread()->Process;
        if (!current) return (UINT64)STATUS_INVALID_HANDLE;
        current->ShutdownLevel = level;
        current->ShutdownFlags = flags;
        return (UINT64)STATUS_SUCCESS;
    }
    static const LONG base[] = {0, 4, 8, 13, 24, 6, 10};
    PRIORITY_CLASS_INFO info = *(const PRIORITY_CLASS_INFO *)a[2];
    if (info.PriorityClass < 1 || info.PriorityClass > 6)
        return (UINT64)STATUS_INVALID_PARAMETER;
    HANDLE handle = (HANDLE)(ULONG_PTR)a[0];
    POBJECT referenced = NULL;
    PKPROCESS process = NULL;
    if ((ULONG_PTR)handle == ~(ULONG_PTR)0) {
        process = KeGetCurrentThread()->Process;
    } else {
        NTSTATUS status = ObReferenceObjectByHandle(handle, 0x200, NULL,
                                                    &referenced);
        if (!NT_SUCCESS(status)) return (UINT64)status;
        process = PsGetProcessFromObject(referenced);
    }
    if (!process) {
        if (referenced) ObDereferenceObject(referenced);
        return (UINT64)STATUS_INVALID_HANDLE;
    }
    process->PriorityClass = info.PriorityClass;
    process->BasePriority = base[info.PriorityClass];
    for (PLIST_ENTRY e = process->ThreadListHead.Flink;
         e != &process->ThreadListHead; e = e->Flink) {
        PKTHREAD thread = CONTAINING_RECORD(e, KTHREAD, ProcessEntry);
        thread->Priority = effective_thread_priority(thread,
                                                      thread->RelativePriority);
    }
    if (referenced) ObDereferenceObject(referenced);
    return (UINT64)STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Thread creation                                                    */
/* ------------------------------------------------------------------ */

static void unmap_user_pages(UINT64 base, SIZE_T pages);

static UINT64 map_user_pages(PKPROCESS process, SIZE_T pages)
{
    if (!process || !process->ThreadAllocationNext || !pages)
        return 0;
    /* Reuse completed threads' holes before extending the high-water mark. */
    UINT64 base = 0x0000000030000000ULL;
    while (base < process->ThreadAllocationNext && base < MM_USER_MAX &&
           pages <= (MM_USER_MAX - base) / PAGE_SIZE) {
        SIZE_T i = 0;
        while (i < pages &&
               MmGetPhysicalAddress(base + i * PAGE_SIZE) == MM_INVALID_PHYS)
            ++i;
        if (i == pages) break;
        base += (i + 1) * PAGE_SIZE;
    }
    if (base >= MM_USER_MAX || pages > (MM_USER_MAX - base) / PAGE_SIZE)
        return 0;
    /* Reject occupied pages before mapping; rollback must never unmap an
     * unrelated allocation if the hole scan reached the high-water mark. */
    for (SIZE_T i = 0; i < pages; ++i)
        if (MmGetPhysicalAddress(base + i * PAGE_SIZE) != MM_INVALID_PHYS)
            return 0;
    for (SIZE_T i = 0; i < pages; i++) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS) {
            unmap_user_pages(base, i);
            return 0;
        }
        if (!MmMapPage(base + i * PAGE_SIZE, pa, PTE_USER | PTE_WRITE)) {
            MmFreePages(pa, 1);
            unmap_user_pages(base, i);
            return 0;
        }
    }
    UINT64 end = base + pages * PAGE_SIZE;
    if (end > process->ThreadAllocationNext)
        process->ThreadAllocationNext = end;
    return base;
}

static void unmap_user_pages(UINT64 base, SIZE_T pages)
{
    if (!base) return;
    for (SIZE_T i = 0; i < pages; i++) {
        UINT64 pa = MmGetPhysicalAddress(base + i * PAGE_SIZE);
        if (pa != MM_INVALID_PHYS && MmUnmapPage(base + i * PAGE_SIZE))
            MmFreePages(PAGE_ALIGN(pa), 1);
    }
}

/*
 * NtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK, POBJECT_ATTRIBUTES,
 *   HANDLE ProcessHandle, PVOID StartRoutine, PVOID Argument, ULONG CreateFlags,
 *   SIZE_T ZeroBits, SIZE_T StackSize, SIZE_T MaximumStackSize, PVOID AttrList)
 * The modern (Vista+) thread-creation service kernel32's CreateThread uses.
 */
static NTSTATUS PspCreateThreadInAddressSpace(UINT64 *a, PKPROCESS process, PHANDLE out, SE_DESCRIPTOR *descriptor, BOOLEAN inherit)
{
    UINT64 entry = a[4], arg = a[5];
    ULONG create_flags = (ULONG)a[6];
    if (create_flags & ~1u)
        return STATUS_INVALID_PARAMETER;
    if (!MmIsUserAddress(entry) || !MmProbeForRead(entry, 1) ||
        (MmGetPageFlags(entry) & PTE_NX)) return STATUS_ACCESS_VIOLATION;

    /* Honor the requested reservation/commit size using the current eager
     * MM commitment model; reservation is rounded to Windows allocation units. */
    UINT64 reserve = a[9] ? a[9] : a[8] > THREAD_STACK_PAGES * PAGE_SIZE ? a[8] : THREAD_STACK_PAGES * PAGE_SIZE;
    if (a[9] && a[8] > a[9]) return STATUS_INVALID_PARAMETER;
    if (reserve > MM_USER_MAX - 0xFFFFu) return STATUS_NO_MEMORY;
    reserve = (reserve + 0xFFFFu) & ~0xFFFFull;
    SIZE_T stack_pages = reserve / PAGE_SIZE;
    UINT64 allocation_start = process->ThreadAllocationNext;
    UINT64 stack_base = map_user_pages(
        process, stack_pages + PROCESS_TEB_SIZE / PAGE_SIZE);
    if (!stack_base)
        return STATUS_NO_MEMORY;
    UINT64 teb_va = stack_base + stack_pages * PAGE_SIZE;
    UINT64 stack_top = stack_base + stack_pages * PAGE_SIZE;

    PTEB teb = (PTEB)teb_va;
    memset(teb, 0, PROCESS_TEB_SIZE);
    /* USER32 keeps its per-thread client callback/cache block in the extended
     * TEB starting at 0x800. The kernel normally initializes this as a thread
     * joins win32k. Until that path is split out, inherit the process template
     * established in the main TEB during USER32 process attach. */
    memcpy((UINT8 *)teb + 0x800,
           (const UINT8 *)PROCESS_MAIN_TEB_VA + 0x800,
           PROCESS_TEB_SIZE - 0x800);
    teb->NtTib.Self = (struct _NT_TIB *)teb_va;
    teb->NtTib.StackBase = (PVOID)stack_top;
    teb->NtTib.StackLimit = (PVOID)stack_base;
    teb->ProcessEnvironmentBlock = (PVOID)PROCESS_PEB_VA;
    teb->ClientId.UniqueProcess =
        (HANDLE)(ULONG_PTR)(process ? process->ProcessId : PROCESS_CLIENT_ID);

    /* Give the new thread its own static TLS blocks (fresh copies of each
     * module's .tls template). */
    UINT64 tls_base = 0;
    SIZE_T tls_pages = 0;
    NTSTATUS tls_status = LdrApplyThreadTlsWithAllocation(
        teb_va, &tls_base, &tls_pages);
    if (!NT_SUCCESS(tls_status)) {
        unmap_user_pages(teb_va, PROCESS_TEB_SIZE / PAGE_SIZE);
        unmap_user_pages(stack_base, stack_pages);
        if (process->ThreadAllocationNext ==
            stack_base + (stack_pages + PROCESS_TEB_SIZE / PAGE_SIZE) * PAGE_SIZE)
            process->ThreadAllocationNext = allocation_start;
        return tls_status;
    }

    /* Waitable thread object (signaled on exit). */
    POBJECT obj;
    if (!NT_SUCCESS(ObCreateObject(g_thread_type, sizeof(THREAD_OBJECT), &obj)))
        goto rollback_mappings;
    THREAD_OBJECT *to = (THREAD_OBJECT *)obj;
    to->Header.Type = ThreadObject;
    to->Header.SignalState = 0;
    InitializeListHead(&to->Header.WaitListHead);
    to->ExitStatus = STATUS_PENDING;
    if (descriptor) SeAssignObjectDescriptor(obj, descriptor);

    /* Install the handle before publishing the runnable KTHREAD. This leaves
     * no allocation that can fail after the child has been enqueued. */
    HANDLE h;
    if (!NT_SUCCESS(ObCreateHandle(obj, (ACCESS_MASK)a[1], &h))) {
        ObDereferenceObject(obj);
        goto rollback_mappings;
    }

    if (inherit) ObSetHandleAttributesInTable(KeGetCurrentThread()->Process->HandleTable, h, OB_HANDLE_INHERIT, OB_HANDLE_INHERIT);

    /* Interrupts are masked during a syscall, so the new thread cannot run
     * before we finish wiring it up. */
    PKTHREAD kt = KeCreateUserThreadForProcess(
        process, "userthread", entry, stack_top, teb_va, arg,
        process->BasePriority);
    if (!kt) {
        ObCloseHandle(h);
        ObDereferenceObject(obj);
        goto rollback_mappings;
    }
    kt->OwnedUserStackBase = stack_base;
    kt->OwnedUserStackPages = stack_pages;
    kt->OwnedUserTebBase = teb_va;
    kt->OwnedUserTebPages = PROCESS_TEB_SIZE / PAGE_SIZE;
    kt->OwnedTlsBase = tls_base;
    kt->OwnedTlsPages = tls_pages;
    W32kInitializeThreadSessionForCreator(kt, KeGetCurrentThread());
    to->Thread = kt;
    to->ProcessId = process->ProcessId; to->ThreadId = kt->ThreadId;
    to->TebBase = kt->UserGsBase; to->Priority = kt->Priority; to->BasePriority = process->BasePriority;
    /* The KTHREAD keeps a raw pointer to this object until it exits, so it owns
     * a reference of its own: user mode routinely closes the thread handle long
     * before the thread runs to completion, and without this the exit path would
     * signal a freed (and by then zeroed) dispatcher header. */
    ObReferenceObject(obj);
    kt->TerminationObject = &to->Header;

    if (create_flags & 1u)
        KeSuspendThread(kt, NULL);

    ObDereferenceObject(obj); /* the handle keeps it alive */

    *out = h;
    UINT64 user_start = 0;
    if (arg && MmProbeForRead(arg, sizeof(UINT64)))
        user_start = *(const UINT64 *)arg; /* kernel32 THREAD_INFO.Start */
    KeLog("[ps]   NtCreateThreadEx(entry=%p, start=%p) -> handle %p\n",
          (void *)entry, (void *)user_start, h);

    /* A newly-created Windows thread is eligible to run before
     * NtCreateThreadEx returns. SHCore relies on that scheduling point when it
     * hands a worker a short-lived stack context and waits for the worker to
     * copy it. Let the child execute now instead of allowing the creator to
     * reuse that stack storage first. */
    return STATUS_SUCCESS;

rollback_mappings:
    LdrRollbackThreadTls(tls_base, tls_pages);
    unmap_user_pages(teb_va, PROCESS_TEB_SIZE / PAGE_SIZE);
    unmap_user_pages(stack_base, stack_pages);
    if (process->ThreadAllocationNext ==
        stack_base + (stack_pages + PROCESS_TEB_SIZE / PAGE_SIZE) * PAGE_SIZE)
        process->ThreadAllocationNext = allocation_start;
    return STATUS_NO_MEMORY;
}


static NTSTATUS PspCreateThreadForProcess(PKPROCESS process, UINT64 *arguments,
                                         HANDLE *out, SE_DESCRIPTOR *descriptor, BOOLEAN inherit)
{
    if (!process || process->Exiting || !process->LoaderContext) return STATUS_INVALID_PARAMETER;
    PLDR_PROCESS_CONTEXT previous_loader = LdrSetThreadProcessContext(process->LoaderContext);
    KAPC_STATE attach_state;
    KeStackAttachProcess(process, &attach_state);
    NTSTATUS status = PspCreateThreadInAddressSpace(arguments, process, out, descriptor, inherit);
    KeUnstackDetachProcess(&attach_state);
    LdrSetThreadProcessContext(previous_loader);
    return status;
}

NTSTATUS PsCreateUserThreadForProcess(PKPROCESS process, UINT64 entry,
                                      UINT64 argument, HANDLE *thread_handle)
{
    if (!process || !thread_handle) return STATUS_INVALID_PARAMETER;
    UINT64 arguments[11] = {0};
    arguments[1] = GENERIC_ALL; arguments[4] = entry; arguments[5] = argument;
    SE_DESCRIPTOR *descriptor = NULL;
    NTSTATUS status = SeCreateDefaultDescriptor(process->SecurityUserRid, &descriptor);
    if (NT_SUCCESS(status)) status = PspCreateThreadForProcess(process, arguments, thread_handle, descriptor, FALSE);
    SeDereferenceDescriptor(descriptor); return status;
}

UINT64 NtCreateThreadEx(UINT64 *arguments)
{
    HANDLE *out = (HANDLE *)arguments[0];
    if (!MmProbeForWrite((UINT64)out, sizeof(*out))) return (UINT64)STATUS_ACCESS_VIOLATION;
    *out = NULL;
    POBJECT reference = NULL;
    PKPROCESS process = NULL;
    if (arguments[3] == (UINT64)-1) {
        PKTHREAD caller = KeGetCurrentThread(); process = caller ? caller->Process : NULL;
    } else {
        NTSTATUS status = ObReferenceObjectByHandle((HANDLE)arguments[3], 0x0002u, NULL, &reference);
        if (!NT_SUCCESS(status)) return (UINT64)status;
        process = PsGetProcessFromObject(reference);
    }
    HANDLE handle = NULL;
    SE_DESCRIPTOR *descriptor = NULL;
    PKTHREAD caller = KeGetCurrentThread();
    NTSTATUS status = SeCreateDefaultDescriptor(caller->Process->SecurityUserRid, &descriptor);
    BOOLEAN inherit = FALSE;
    if (NT_SUCCESS(status) && arguments[2]) {
        OBJECT_ATTRIBUTES attributes;
        if (!MmProbeForRead(arguments[2], sizeof(attributes))) status = STATUS_ACCESS_VIOLATION;
        else {
            memcpy(&attributes, (void *)arguments[2], sizeof(attributes));
            if (attributes.Length != sizeof(attributes) || attributes.ObjectName || attributes.RootDirectory ||
                (attributes.Attributes & 0x200u)) status = STATUS_INVALID_PARAMETER;
            else {
                inherit = (attributes.Attributes & 2u) != 0;
                if (attributes.SecurityDescriptor) {
                    SeDereferenceDescriptor(descriptor); descriptor = NULL;
                    status = SeCaptureDescriptor((UINT64)attributes.SecurityDescriptor, caller->Process->SecurityUserRid, &descriptor);
                    if (NT_SUCCESS(status) && SeDescriptorOwnerRid(descriptor) != caller->Process->SecurityUserRid)
                        status = STATUS_INVALID_OWNER;
                }
            }
        }
    }
    if (NT_SUCCESS(status)) status = PspCreateThreadForProcess(process, arguments, &handle, descriptor, inherit);
    SeDereferenceDescriptor(descriptor);
    if (reference) ObDereferenceObject(reference);
    if (NT_SUCCESS(status)) {
        *out = handle;
        /* Restore the caller's address-space/loader context before allowing
         * either a local or remote worker to execute. */
        if (!((ULONG)arguments[6] & 1u)) KeYield();
    }
    return (UINT64)status;
}

/* ThreadBasicInformation retains identifiers even after the KTHREAD reaper
 * runs; a live object reads current priority from its referenced thread. */
UINT64 NtQueryInformationThread(UINT64 *args)
{
    typedef struct { NTSTATUS Exit; ULONG Padding; UINT64 Teb,Process,Thread,Affinity;
        LONG Priority,BasePriority; } THREAD_BASIC_LOCAL;
    if ((ULONG)args[1] != 0) return (UINT64)STATUS_INVALID_INFO_CLASS;
    if (args[4]) {
        if (!MmProbeForWrite(args[4],4)) return (UINT64)STATUS_ACCESS_VIOLATION;
        *(ULONG *)args[4] = sizeof(THREAD_BASIC_LOCAL);
    }
    if ((ULONG)args[3] < sizeof(THREAD_BASIC_LOCAL)) return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
    if (!MmProbeForWrite(args[2],sizeof(THREAD_BASIC_LOCAL))) return (UINT64)STATUS_ACCESS_VIOLATION;
    THREAD_BASIC_LOCAL info = {0};
    if (args[0] == (UINT64)-2) {
        PKTHREAD thread = KeGetCurrentThread();
        info.Exit=thread->State==ThreadStateTerminated?thread->ExitStatus:STATUS_PENDING;
        info.Teb=thread->UserGsBase;info.Process=thread->Process->ProcessId;info.Thread=thread->ThreadId;
        info.Priority=thread->Priority;info.BasePriority=thread->Process->BasePriority;
    } else {
        POBJECT object; ACCESS_MASK granted;
        PKTHREAD caller = KeGetCurrentThread();
        NTSTATUS status = ObReferenceObjectByHandleInTable(caller->Process->HandleTable,
            (HANDLE)args[0],0,g_thread_type,&object,&granted);
        if (!NT_SUCCESS(status)) return (UINT64)status;
        if (!(granted & (0x40u|0x800u))) {ObDereferenceObject(object);return (UINT64)STATUS_ACCESS_DENIED;}
        THREAD_OBJECT *record=object;
        info.Exit=record->Header.SignalState?record->ExitStatus:STATUS_PENDING;
        info.Teb=record->TebBase;info.Process=record->ProcessId;info.Thread=record->ThreadId;
        info.Priority=record->Header.SignalState?record->Priority:record->Thread->Priority;
        info.BasePriority=record->BasePriority;
        ObDereferenceObject(object);
    }
    info.Affinity=1; /* APs are parked; execution is actually confined to CPU0. */
    memcpy((void *)args[2],&info,sizeof(info));return (UINT64)STATUS_SUCCESS;
}

/* Private query used by GetExitCodeThread. The handle type and requested
 * information access are checked before exposing the retained exit status. */
UINT64 SvNtQueryThreadExitStatus(UINT64 *a)
{
    if (!a[1] || !MmProbeForWrite(a[1], sizeof(ULONG)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    POBJECT object = NULL;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)a[0],
        0x0040u, g_thread_type, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    *(ULONG *)a[1] = (ULONG)((THREAD_OBJECT *)object)->ExitStatus;
    ObDereferenceObject(object);
    return (UINT64)STATUS_SUCCESS;
}
