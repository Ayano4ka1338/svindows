/*
 * process_object.c - minimal waitable user process creation.
 *
 * This is intentionally separate from thread/event services and from the PE
 * loader. It establishes the first real multi-process path while handles and
 * loader contexts are still system-wide transitional facilities.
 */
#include <svindows/ps.h>
#include <svindows/address_space.h>
#include <svindows/mm.h>
#include <svindows/ldr.h>
#include <svindows/ob.h>
#include <svindows/rtl.h>
#include <nt/peb.h>

#define CHILD_STACK_TOP   0x0000000010010000ULL
#define CHILD_STACK_PAGES 16

typedef struct _PROCESS_OBJECT {
    DISPATCHER_HEADER Header;
    KPROCESS          Process;
    char              Name[64];
} PROCESS_OBJECT;

static POBJECT_TYPE g_process_type;
static ULONG g_next_process_id = 8;

static void delete_process(POBJECT raw)
{
    PROCESS_OBJECT *object = (PROCESS_OBJECT *)raw;
    LdrDestroyProcessContext(object->Process.LoaderContext);
    object->Process.LoaderContext = NULL;
    ObDestroyHandleTable(object->Process.HandleTable);
    object->Process.HandleTable = NULL;
    MmDestroyAddressSpace(object->Process.DirectoryTableBase);
    object->Process.DirectoryTableBase = 0;
}

static BOOLEAN capture_ascii(UINT64 address, char *out, SIZE_T capacity)
{
    if (!address || !capacity) return FALSE;
    for (SIZE_T i = 0; i + 1 < capacity; i++) {
        if (!MmProbeForRead(address + i, 1)) return FALSE;
        out[i] = *(const char *)(address + i);
        if (!out[i]) return TRUE;
    }
    out[capacity - 1] = 0;
    return TRUE;
}

static void *map_rw(UINT64 va)
{
    UINT64 pa = MmAllocatePage();
    if (pa == MM_INVALID_PHYS ||
        !MmMapPage(va, pa, PTE_USER | PTE_WRITE))
        return NULL;
    memset((void *)va, 0, PAGE_SIZE);
    return (void *)va;
}

static NTSTATUS build_environment(PROCESS_OBJECT *object, const char *command,
                                  UINT64 image_base, UINT64 entry,
                                  UINT64 argument, PKTHREAD *thread_out)
{
    PPEB peb = map_rw(PROCESS_PEB_VA);
    if (!peb) return STATUS_NO_MEMORY;
    peb->ImageBaseAddress = (PVOID)image_base;

    for (UINT64 off = 0; off < PROCESS_LDR_SIZE; off += PAGE_SIZE)
        if (!map_rw(PROCESS_LDR_VA + off)) return STATUS_NO_MEMORY;
    LdrBuildProcessModuleList(peb, PROCESS_LDR_VA, PROCESS_LDR_SIZE);

    UINT8 *params = map_rw(PROCESS_PARAMS_VA);
    if (!params) return STATUS_NO_MEMORY;
    UINT16 *imagew = (UINT16 *)(params + 0x200);
    UINT16 *cmdw = (UINT16 *)(params + 0x400);
    UINT16 image_n = 0, cmd_n = 0;
    while (object->Name[image_n] && image_n < 200) {
        imagew[image_n] = (UINT8)object->Name[image_n]; image_n++;
    }
    while (command[cmd_n] && cmd_n < 500) {
        cmdw[cmd_n] = (UINT8)command[cmd_n]; cmd_n++;
    }
    *(UINT16 *)(params + 0x60) = image_n * 2;
    *(UINT16 *)(params + 0x62) = image_n * 2 + 2;
    *(void **)(params + 0x68) = imagew;
    *(UINT16 *)(params + 0x70) = cmd_n * 2;
    *(UINT16 *)(params + 0x72) = cmd_n * 2 + 2;
    *(void **)(params + 0x78) = cmdw;
    peb->ProcessParameters = params;
    *(HANDLE *)(params + 0x20) = object->Process.StandardInput;
    *(HANDLE *)(params + 0x28) = object->Process.StandardOutput;
    *(HANDLE *)(params + 0x30) = object->Process.StandardError;
    char *cmda = (char *)(params + 0x800);
    for (UINT16 i = 0; i <= cmd_n; i++) cmda[i] = command[i];
    LdrSeedCrtCommandLine((UINT64)cmda, (UINT64)cmdw);

    for (UINT64 off = 0; off < PROCESS_TEB_SIZE; off += PAGE_SIZE)
        if (!map_rw(PROCESS_MAIN_TEB_VA + off)) return STATUS_NO_MEMORY;
    PTEB teb = (PTEB)PROCESS_MAIN_TEB_VA;
    teb->NtTib.Self = (struct _NT_TIB *)PROCESS_MAIN_TEB_VA;
    teb->ProcessEnvironmentBlock = (PVOID)PROCESS_PEB_VA;
    teb->ClientId.UniqueProcess =
        (HANDLE)(ULONG_PTR)object->Process.ProcessId;

    UINT64 stack_base = CHILD_STACK_TOP - CHILD_STACK_PAGES * PAGE_SIZE;
    for (UINT64 off = 0; off < CHILD_STACK_PAGES * PAGE_SIZE; off += PAGE_SIZE)
        if (!map_rw(stack_base + off)) return STATUS_NO_MEMORY;
    teb->NtTib.StackBase = (PVOID)CHILD_STACK_TOP;
    teb->NtTib.StackLimit = (PVOID)stack_base;

    /* Static TLS: allocate this thread's block vector now that every module
     * (and so every TLS index) is known, and publish the index count so
     * kernel32's TlsAlloc continues the same space. */
    LdrApplyThreadTls(PROCESS_MAIN_TEB_VA);
    peb->LdrpTlsNextIndex = LdrTlsIndexCount();
    peb->LdrpTlsSlotCount = 64;

    PKTHREAD thread = KeCreateUserThreadForProcess(
        &object->Process, object->Name, entry, CHILD_STACK_TOP,
        PROCESS_MAIN_TEB_VA, argument, 8);
    if (!thread) return STATUS_NO_MEMORY;
    *thread_out = thread;
    return STATUS_SUCCESS;
}

BOOLEAN PsIsProcessObjectType(POBJECT_TYPE type)
{
    return type && type == g_process_type;
}

PKPROCESS PsGetProcessFromObject(PVOID raw)
{
    if (!raw || !PsIsProcessObjectType(ObHeaderFromObject(raw)->Type))
        return NULL;
    return &((PROCESS_OBJECT *)raw)->Process;
}

static NTSTATUS create_process_captured(const char *filename,
                                        const char *command,
                                        HANDLE *out, ULONG *pid_out,
                                        HANDLE std_input, HANDLE std_output,
                                        HANDLE std_error)
{
    if (!g_process_type)
        g_process_type = ObCreateObjectType("Process", delete_process);
    POBJECT raw;
    NTSTATUS status = ObCreateObject(g_process_type, sizeof(PROCESS_OBJECT),
                                     &raw);
    if (!NT_SUCCESS(status)) return status;
    PROCESS_OBJECT *object = (PROCESS_OBJECT *)raw;
    memset(object, 0, sizeof(*object));
    object->Header.Type = ThreadObject;
    InitializeListHead(&object->Header.WaitListHead);
    object->Process.ProcessId = g_next_process_id;
    g_next_process_id += 4;
    PKTHREAD creator = KeGetCurrentThread();
    object->Process.ParentProcessId =
        creator && creator->Process ? creator->Process->ProcessId : 0;
    object->Process.ExitStatus = STATUS_PENDING;
    object->Process.Object = raw;
    object->Process.UserAllocationNext = 0x0000000020000000ULL;
    object->Process.ThreadAllocationNext = 0x0000000030000000ULL;
    InitializeListHead(&object->Process.ThreadListHead);
    for (UINT32 i = 0; filename[i] && i + 1 < sizeof(object->Name); i++)
        object->Name[i] = filename[i];
    object->Process.Name = object->Name;
    object->Process.HandleTable = ObCreateHandleTable();
    if (!object->Process.HandleTable) {
        ObDereferenceObject(raw);
        return STATUS_NO_MEMORY;
    }
    object->Process.LoaderContext = LdrCreateProcessContext();
    if (!object->Process.LoaderContext) {
        ObDestroyHandleTable(object->Process.HandleTable);
        object->Process.HandleTable = NULL;
        ObDereferenceObject(raw);
        return STATUS_NO_MEMORY;
    }
    KeLog("[ps]   process object %p owns loader context %p\n", raw,
          object->Process.LoaderContext);
    if (std_input &&
        !NT_SUCCESS(ObDuplicateHandleToTable(std_input,
                     object->Process.HandleTable,
                     &object->Process.StandardInput))) {
        ObDereferenceObject(raw);
        return STATUS_INVALID_HANDLE;
    }
    if (std_output &&
        !NT_SUCCESS(ObDuplicateHandleToTable(std_output,
                     object->Process.HandleTable,
                     &object->Process.StandardOutput))) {
        ObDereferenceObject(raw);
        return STATUS_INVALID_HANDLE;
    }
    if (std_error &&
        !NT_SUCCESS(ObDuplicateHandleToTable(std_error,
                     object->Process.HandleTable,
                     &object->Process.StandardError))) {
        ObDereferenceObject(raw);
        return STATUS_INVALID_HANDLE;
    }
    object->Process.DirectoryTableBase = MmCreateAddressSpace();
    if (object->Process.DirectoryTableBase == MM_INVALID_PHYS) {
        LdrDestroyProcessContext(object->Process.LoaderContext);
        object->Process.LoaderContext = NULL;
        ObDestroyHandleTable(object->Process.HandleTable);
        object->Process.HandleTable = NULL;
        ObDereferenceObject(raw);
        return STATUS_NO_MEMORY;
    }

    HANDLE handle = NULL;
    if (out) {
        status = ObCreateHandle(raw, GENERIC_ALL, &handle);
        if (!NT_SUCCESS(status)) {
            ObDereferenceObject(raw);
            return status;
        }
    }

    UINT64 parent_space = MmGetCurrentAddressSpace();
    PLDR_PROCESS_CONTEXT previous_loader = LdrSetThreadProcessContext(
        object->Process.LoaderContext);
    MmSwitchAddressSpace(object->Process.DirectoryTableBase);
    LdrResetProcessContext();
    UINT64 entry, base, argument;
    status = LdrLoadExecutable(filename, &entry, &base, &argument);
    PKTHREAD thread = NULL;
    if (NT_SUCCESS(status))
        status = build_environment(object, command, base, entry, argument,
                                   &thread);
    if (NT_SUCCESS(status)) {
        ObReferenceObject(raw); /* held until the main thread signals exit */
        thread->TerminationObject = &object->Header;
        thread->ProcessMainThread = TRUE;
    }
    MmSwitchAddressSpace(parent_space);
    LdrSetThreadProcessContext(previous_loader);

    if (!NT_SUCCESS(status)) {
        if (handle)
            ObCloseHandle(handle);
        ObDereferenceObject(raw);
        return status;
    }
    if (out) *out = handle;
    if (pid_out) *pid_out = object->Process.ProcessId;
    ObDereferenceObject(raw); /* handle + running thread retain it */
    KeLog("[ps]   process '%s' pid %u -> handle %p, CR3 0x%lx\n",
          filename, object->Process.ProcessId, handle,
          (unsigned long)object->Process.DirectoryTableBase);
    return STATUS_SUCCESS;
}

NTSTATUS PsCreateInitialProcess(const char *application,
                                const char *command_line)
{
    return create_process_captured(application, command_line, NULL, NULL,
                                   NULL, NULL, NULL);
}

UINT64 NtCreateUserProcessSimple(UINT64 *a)
{
    char filename[64], command[260];
    PHANDLE out = (PHANDLE)a[2];
    ULONG *pid_out = (ULONG *)a[3];
    KeLog("[ps]   NtCreateUserProcessSimple request image=%p command=%p\n",
          (void *)a[0], (void *)a[1]);
    if (!MmProbeForWrite((UINT64)out, sizeof(*out)) ||
        (pid_out && !MmProbeForWrite((UINT64)pid_out, sizeof(*pid_out))) ||
        !capture_ascii(a[0], filename, sizeof(filename)) ||
        !capture_ascii(a[1], command, sizeof(command))) {
        KeLog("[ps]   process request rejected: invalid user buffer\n");
        return (UINT64)STATUS_ACCESS_VIOLATION;
    }
    return (UINT64)create_process_captured(filename, command, out, pid_out,
                                           (HANDLE)a[4], (HANDLE)a[5],
                                           (HANDLE)a[6]);
}
