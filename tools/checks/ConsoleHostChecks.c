/* Actual console broker and executive objects/handles. The scheduler and
 * File adapter are deterministic fixtures; guest conhost runs validate UI. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <svindows/Console.h>
#include <svindows/Ex.h>
#include <svindows/Gfx.h>
#include <svindows/Io.h>
#include <svindows/Ke.h>
#include <svindows/Ob.h>
#include <svindows/Ps.h>
#include <svindows/Rtl.h>

static UINT64 HostIrqSave(void) { return 0; }
static void HostIrqRestore(UINT64 flags) { (void)flags; }
static UINT64 HostAcquireRaw(PKSPIN_LOCK lock) {
    (void)lock;
    return 0;
}
static void HostReleaseRaw(PKSPIN_LOCK lock, UINT64 flags) {
    (void)lock;
    (void)flags;
}
#define KiIrqSave HostIrqSave
#define KiIrqRestore HostIrqRestore
#define KiAcquireRawSpinLock HostAcquireRaw
#define KiReleaseRawSpinLock HostReleaseRaw

static KTHREAD thread_a, thread_b, thread_host, broker;
static KPROCESS process_a, process_b, process_host, parent;
static PKTHREAD current;
static long allocations;
static int fail_after = -1;
static UINT64 deny_address;
static UINT64 deny_after_gate;
PKTHREAD KeGetCurrentThread(void) { return current; }
UINT64 KeGetTickCount(void) { return 100; }
int KeLog(const char *format, ...) {
    (void)format;
    return 0;
}
NORETURN void KeBugCheck(ULONG code, const char *message) {
    fprintf(stderr, "BUGCHECK %u %s\n", code, message);
    abort();
}
PVOID ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T bytes, ULONG tag) {
    (void)type;
    (void)tag;
    if (!fail_after)
        return NULL;
    if (fail_after > 0)
        fail_after--;
    void *p = malloc(bytes);
    if (p)
        allocations++;
    return p;
}
void ExFreePool(void *memory) {
    if (memory) {
        allocations--;
        free(memory);
    }
}
BOOLEAN MmProbeForRead(UINT64 address, UINT64 bytes) {
    return !bytes || (address && address != deny_address);
}
BOOLEAN MmProbeForWrite(UINT64 address, UINT64 bytes) { return MmProbeForRead(address, bytes); }
BOOLEAN MmIsUserAddress(UINT64 address) { return address < 0x800000000000ull; }
GFX_FRAMEBUFFER GfxFramebuffer = {.Width = 1200, .Height = 800};
BOOLEAN GfxAvailable(void) { return TRUE; }
void KeInitializeEvent(PKEVENT event, BOOLEAN notification, BOOLEAN signaled) {
    memset(event, 0, sizeof(*event));
    event->Header.Type = notification ? EventNotificationObject : EventSynchronizationObject;
    event->Header.SignalState = signaled;
    InitializeListHead(&event->Header.WaitListHead);
}
LONG KeSetEvent(PKEVENT event) {
    LONG old = event->Header.SignalState;
    event->Header.SignalState = 1;
    return old;
}
void KeResetEvent(PKEVENT event) { event->Header.SignalState = 0; }
void KeInitializeMutant(PKMUTANT mutant, BOOLEAN owned) {
    memset(mutant, 0, sizeof(*mutant));
    mutant->Header.Type = MutantObject;
    mutant->Header.SignalState = owned ? 0 : 1;
    mutant->OwnerThread = owned ? current : NULL;
    InitializeListHead(&mutant->Header.WaitListHead);
}
void KeInitializeMutantWithReference(PKMUTANT mutant, BOOLEAN owned,
                                     PVOID reference) {
    KeInitializeMutant(mutant, owned);
    mutant->ReferenceObject = reference;
}
NTSTATUS KeWaitForSingleObject(PDISPATCHER_HEADER header) {
    if (header->Type == MutantObject) {
        PKMUTANT mutant = (PKMUTANT)header;
        assert(mutant->OwnerThread == current || header->SignalState > 0);
        header->SignalState--;
        mutant->OwnerThread = current;
        if (deny_after_gate) {
            deny_address = deny_after_gate;
            deny_after_gate = 0;
        }
    } else
        assert(header->SignalState > 0);
    return STATUS_SUCCESS;
}
NTSTATUS KeWaitForSingleObjectTimeout(PDISPATCHER_HEADER header, UINT64 timeout) {
    (void)timeout;
    if (header->Type == MutantObject && ((PKMUTANT)header)->OwnerThread != current &&
        header->SignalState <= 0)
        return STATUS_TIMEOUT;
    return KeWaitForSingleObject(header);
}
BOOLEAN KeReleaseMutant(PKMUTANT mutant, LONG *previous) {
    assert(mutant->OwnerThread == current);
    if (previous)
        *previous = mutant->Header.SignalState;
    if (++mutant->Header.SignalState == 1)
        mutant->OwnerThread = NULL;
    return TRUE;
}
PKTHREAD KeCreateThread(const char *name, PKSTART_ROUTINE start, void *context, LONG priority) {
    (void)name;
    (void)start;
    (void)context;
    (void)priority;
    return &broker;
}
void PsRegisterConsoleProcessInitializer(PS_CONSOLE_PROCESS_INITIALIZER initialize) {
    assert(initialize);
}
void PsRegisterConsoleProcessPublished(void (*published)(PKPROCESS)) { assert(published); }
NTSTATUS PsCreateConsoleHost(PKPROCESS owner, const char *command, HANDLE *out) {
    (void)owner;
    (void)command;
    *out = NULL;
    return STATUS_UNSUCCESSFUL;
}
NTSTATUS PsCreateUserThreadForProcess(PKPROCESS process, UINT64 entry, UINT64 arg, HANDLE *out) {
    (void)process;
    (void)entry;
    (void)arg;
    *out = NULL;
    return STATUS_UNSUCCESSFUL;
}
PKPROCESS PsGetProcessFromObject(PVOID object) { return object; }
PVOID PsReferenceNextProcess(ULONG after, ULONG *pid) {
    (void)after;
    *pid = 0;
    return NULL;
}
UINT64 NtTerminateProcess(UINT64 *args) {
    (void)args;
    return STATUS_SUCCESS;
}
PVOID W32kGetProcessWindowStationIdentity(PKPROCESS process) {
    (void)process;
    return (void *)1;
}
BOOLEAN W32kWindowBelongsToProcess(UINT64 window, PKPROCESS process) {
    return window == (UINT64)process->ProcessId << 16;
}
PVOID IoPinThreadConsoleObject(PVOID object) {
    assert(object);
    return ExAllocatePoolWithTag(NonPagedPool, 8, 0);
}
void IoUnpinThreadConsoleObject(PVOID pin) { ExFreePool(pin); }

#include "../../kernel/ob/ObCore.c"
#include "../../kernel/ob/ObHandle.c"
#include "../../kernel/ob/ObNamespace.c"
#include "../../kernel/se/Security.c"

typedef struct {
    UINT64 Tag;
    void *Context;
    const IO_CHAR_DEVICE_OPS *Ops;
} TEST_FILE;
static POBJECT_TYPE file_type;
static void delete_file(POBJECT raw) {
    TEST_FILE *file = raw;
    if (file->Ops)
        file->Ops->Close(file->Context);
}
NTSTATUS IoCreateCharDeviceFileInTable(POB_HANDLE_TABLE table, UINT64 tag, void *context,
                                       const IO_CHAR_DEVICE_OPS *ops, ACCESS_MASK access,
                                       HANDLE *out) {
    POBJECT object;
    NTSTATUS status = ObCreateObject(file_type, sizeof(TEST_FILE), &object);
    if (!NT_SUCCESS(status))
        return status;
    TEST_FILE *file = object;
    file->Tag = tag;
    file->Context = context;
    file->Ops = ops;
    status = ObCreateHandleInTable(table, object, access, out);
    if (!NT_SUCCESS(status))
        file->Ops = NULL;
    ObDereferenceObject(object);
    return status;
}
NTSTATUS IoQueryCharDeviceFileInTable(POB_HANDLE_TABLE table, HANDLE handle, UINT64 *tag,
                                      void **context) {
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandleInTable(table, handle, 0, file_type, &object, NULL);
    if (!NT_SUCCESS(status))
        return status;
    TEST_FILE *file = object;
    *tag = file->Tag;
    *context = file->Context;
    ObDereferenceObject(object);
    return STATUS_SUCCESS;
}
NTSTATUS IoQueryCharDeviceFile(HANDLE handle, UINT64 *tag, void **context) {
    return IoQueryCharDeviceFileInTable(current->Process->HandleTable, handle, tag, context);
}

#include "../../kernel/io/ConsoleHost.c"

static CON_ENDPOINT *endpoint(HANDLE handle) {
    UINT64 tag;
    void *context;
    assert(NT_SUCCESS(IoQueryCharDeviceFile(handle, &tag, &context)));
    assert(tag == IO_CHAR_DEVICE_TAG_CONSOLE);
    return context;
}
static void fixture(KPROCESS *process, KTHREAD *thread, ULONG pid) {
    memset(process, 0, sizeof(*process));
    memset(thread, 0, sizeof(*thread));
    process->ProcessId = pid;
    process->Name = "test.exe";
    process->SecurityUserRid = 1000;
    strcpy(process->ImagePath, "C:\\test.exe");
    process->HandleTable = ObCreateHandleTable();
    assert(process->HandleTable);
    process->StartupXChars = 8;
    process->StartupYChars = 8;
    thread->Process = process;
    thread->ThreadId = pid + 1;
}
static void key(CON_OBJECT *console, WCHAR character, UINT16 repeat, UINT32 flags) {
    SV_CONSOLE_INPUT_RECORD record = {.EventType = 1};
    record.Event.Key.KeyDown = TRUE;
    record.Event.Key.Repeat = repeat;
    record.Event.Key.Character = character;
    record.Event.Key.ControlState = flags;
    assert(NT_SUCCESS(con_enqueue_input(console, &record)));
}

int main(void) {
    ObInitialize();
    file_type = ObCreateObjectType("File", delete_file);
    assert(file_type);
    ConInitializeHostedConsoles();
    SeConfigureType(file_type, 0x120089u, 0x120116u, 0x1200a0u, 0x1f01ffu);
    long baseline = allocations;
    fixture(&process_a, &thread_a, 8);
    fixture(&process_b, &thread_b, 12);
    fixture(&process_host, &thread_host, 16);
    KPROCESS boot_parent = {0}, boot_cmd = {0}, legacy_child = {0};
    assert(NT_SUCCESS(ConInitializeProcessConsole(&boot_cmd, &boot_parent,
                                                  0, 3, 0, 0, 0)) &&
           boot_cmd.LegacyConsole && !boot_cmd.NativeConsole);
    boot_cmd.Object = (void *)1;
    assert(NT_SUCCESS(ConInitializeProcessConsole(&legacy_child, &boot_cmd,
                                                  0, 3, 0, 0, 0)) &&
           legacy_child.LegacyConsole && !legacy_child.NativeConsole);
    puts("PASS boot command processor and CUI children retain the legacy console");
    parent.Object = (void *)1;
    current = &thread_a;
    assert(NT_SUCCESS(ConInitializeProcessConsole(&process_a, &parent, 0, 3, 0, 0, 0)));
    CON_ATTACHMENT *a = con_attachment(&process_a);
    assert(a && process_a.StandardInput && process_a.StandardOutput);
    current = &thread_b;
    assert(NT_SUCCESS(ConInitializeProcessConsole(&process_b, &parent, 0, 3, 0, 0, 0)));
    CON_ATTACHMENT *b = con_attachment(&process_b);
    assert(b && a->Console != b->Console);
    current = &thread_a;
    CON_ENDPOINT *input = endpoint(process_a.StandardInput),
                 *output = endpoint(process_a.StandardOutput);
    WCHAR text[] = {0x0416, 'A'};
    UINT64 count = 0;
    assert(NT_SUCCESS(con_endpoint_control(output, SV_CONSOLE_CONTROL_WRITE_WIDE, (UINT64)text, 2,
                                           0, &count)) &&
           count == 2);
    assert(output->Screen->Cells[0].Character == 0x0416 &&
           output->Screen->Cells[1].Character == 'A');
    assert(b->Console->Active->Cells[0].Character == ' ');
    assert(!NT_SUCCESS(
        con_endpoint_control(input, SV_CONSOLE_CONTROL_WRITE_WIDE, (UINT64)text, 2, 0, &count)));
    puts("PASS separate executive consoles, real endpoints, Unicode output and direction checks");

    assert(NT_SUCCESS(con_endpoint_control(input, ConsoleControlSetMode, 0, 0, 0, NULL)));
    key(a->Console, 0x0416, 3, 16);
    WCHAR chars[8] = {0};
    assert(
        NT_SUCCESS(con_endpoint_control(input, SV_CONSOLE_CONTROL_PEEK_INPUT,
                                        (UINT64)(SV_CONSOLE_INPUT_RECORD[2]){0}, 2, 0, &count)) &&
        count == 1);
    assert(NT_SUCCESS(con_endpoint_control(input, SV_CONSOLE_CONTROL_READ_WIDE, (UINT64)chars, 2, 0,
                                           &count)) &&
           count == 2 && chars[0] == 0x0416 && chars[1] == 0x0416);
    SV_CONSOLE_INPUT_RECORD record;
    assert(NT_SUCCESS(con_endpoint_control(input, SV_CONSOLE_CONTROL_READ_INPUT, (UINT64)&record, 1,
                                           0, &count)) &&
           count == 1);
    assert(record.Event.Key.Repeat == 1 && record.Event.Key.ControlState == 16 &&
           !a->Console->InputCount && !b->Console->InputCount);
    assert(NT_SUCCESS(con_endpoint_control(input, ConsoleControlSetMode,
                                           CON_INPUT_LINE | CON_INPUT_ECHO | CON_INPUT_PROCESSED, 0,
                                           0, NULL)));
    key(a->Console, 'a', 1, 0);
    key(a->Console, 'b', 1, 0);
    key(a->Console, '\b', 1, 0);
    key(a->Console, 'c', 1, 0);
    key(a->Console, '\r', 1, 0);
    assert(NT_SUCCESS(con_endpoint_control(input, SV_CONSOLE_CONTROL_READ_WIDE, (UINT64)chars, 8, 0,
                                           &count)) &&
           count == 4);
    assert(chars[0] == 'a' && chars[1] == 'c' && chars[2] == '\r' && chars[3] == '\n');
    chars[0] = 'x';
    chars[1] = 'y';
    key(a->Console, 26, 1, 8);
    SV_CONSOLE_READ_CONTROL control = {sizeof(control), 2, 1u << 26, 0};
    assert(NT_SUCCESS(con_endpoint_control(input, SV_CONSOLE_CONTROL_READ_WIDE, (UINT64)chars, 8,
                                           (UINT64)&control, &count)) &&
           count == 3);
    assert(chars[0] == 'x' && chars[1] == 'y' && chars[2] == 26 && control.ControlKeyState == 8);
    puts("PASS input-record peek/consume/repeat, cooked editing and Unicode wakeup control");

    assert(
        NT_SUCCESS(con_endpoint_control(output, ConsoleControlSetCursorPosition, 0, 0, 0, NULL)));
    assert(NT_SUCCESS(
        con_endpoint_control(output, SV_CONSOLE_CONTROL_SET_OUTPUT_CP, 65001, 0, 0, NULL)));
    const unsigned char utf8[] = {0xf0, 0x9f, 0x98, 0x80};
    assert(con_endpoint_write(output, utf8, 2) == 2 &&
           con_endpoint_write(output, utf8 + 2, 2) == 2);
    assert(output->Screen->Cells[0].Character == 0xd83d &&
           output->Screen->Cells[1].Character == 0xde00);
    assert(NT_SUCCESS(con_endpoint_control(input, ConsoleControlSetMode, 0, 0, 0, NULL)));
    assert(NT_SUCCESS(
        con_endpoint_control(input, SV_CONSOLE_CONTROL_SET_INPUT_CP, 65001, 0, 0, NULL)));
    key(a->Console, 0x0416, 1, 0);
    unsigned char encoded[3] = {0};
    SIZE_T transferred = 0;
    assert(NT_SUCCESS(con_endpoint_read_result(input, encoded, 1, FALSE, &transferred)) &&
           transferred == 1 && encoded[0] == 0xd0);
    assert(NT_SUCCESS(con_endpoint_read_result(input, encoded + 1, 1, FALSE, &transferred)) &&
           transferred == 1 && encoded[1] == 0x96);
    puts("PASS incremental UTF-8 output and partial multibyte input");

    SV_CONSOLE_RECT small = {0, 0, 1, 1};
    assert(NT_SUCCESS(
        con_endpoint_control(output, SV_CONSOLE_CONTROL_SET_WINDOW, (UINT64)&small, 1, 0, NULL)));
    assert(NT_SUCCESS(con_endpoint_control(output, SV_CONSOLE_CONTROL_RESIZE_BUFFER,
                                           4u | (4u << 16), 0, 0, NULL)));
    UINT32 old_width = output->Screen->Width;
    fail_after = 0;
    assert(con_endpoint_control(output, SV_CONSOLE_CONTROL_RESIZE_BUFFER, 5u | (5u << 16), 0, 0,
                                NULL) == STATUS_NO_MEMORY);
    fail_after = -1;
    assert(output->Screen->Width == old_width);
    SV_CONSOLE_CELL data[9];
    for (unsigned i = 0; i < 9; i++)
        data[i] = (SV_CONSOLE_CELL){(UINT16)('0' + i), 0x2f};
    SV_CONSOLE_BUFFER_TRANSFER transfer = {
        .Cells = (UINT64)data, .Width = 3, .Height = 3, .Region = {-1, -1, 1, 1}};
    assert(NT_SUCCESS(con_endpoint_control(output, SV_CONSOLE_CONTROL_WRITE_OUTPUT,
                                           (UINT64)&transfer, 0, 0, NULL)));
    assert(transfer.Region.Left == 0 && transfer.Region.Top == 0 && transfer.Region.Right == 1 &&
           transfer.Region.Bottom == 1);
    assert(output->Screen->Cells[0].Character == '4' && output->Screen->Cells[1].Character == '5');
    transfer.Region = (SV_CONSOLE_RECT){-32768, -32768, 1, 1};
    assert(NT_SUCCESS(con_endpoint_control(output, SV_CONSOLE_CONTROL_WRITE_OUTPUT,
                                           (UINT64)&transfer, 0, 0, NULL)) &&
           transfer.Region.Right == -1);
    puts("PASS transactional buffer allocation and clipped screen transfers without coordinate "
         "wrap");

    UINT8 descriptor[68] = {0};
    descriptor[0] = 1;
    descriptor[2] = 4;
    descriptor[3] = 0x80;
    descriptor[16] = 20;
    descriptor[20] = 2;
    descriptor[22] = 48;
    descriptor[24] = 2;
    descriptor[28] = 1;
    descriptor[30] = 20;
    descriptor[32] = 1;
    descriptor[36] = 1;
    descriptor[37] = 1;
    descriptor[43] = 1;
    descriptor[48] = 0;
    descriptor[50] = 20;
    descriptor[55] = 0x10;
    descriptor[56] = 1;
    descriptor[57] = 1;
    descriptor[63] = 1;
    struct {
        UINT32 Size, Padding;
        UINT64 Descriptor;
        UINT32 Inherit, Reserved;
    } attributes = {24, 0, (UINT64)descriptor, 1, 0};
    UINT64 secured_args[5] = {SV_CONSOLE_CREATE_BUFFER, GENERIC_READ | GENERIC_WRITE, 3,
                              (UINT64)&attributes, (UINT64)&count};
    assert(NT_SUCCESS((NTSTATUS)SvNtConsoleManage(secured_args)));
    HANDLE secured = (HANDLE)count;
    ULONG attributes_out = 0;
    assert(NT_SUCCESS(
               ObQueryHandleAttributesInTable(process_a.HandleTable, secured, &attributes_out)) &&
           (attributes_out & 1));
    secured_args[0] = SV_CONSOLE_ACTIVATE_BUFFER;
    secured_args[1] = (UINT64)secured;
    assert(NT_SUCCESS((NTSTATUS)SvNtConsoleManage(secured_args)));
    secured_args[0] = SV_CONSOLE_OPEN_OUTPUT;
    secured_args[2] = GENERIC_READ;
    assert((NTSTATUS)SvNtConsoleManage(secured_args) == STATUS_ACCESS_DENIED);
    secured_args[2] = GENERIC_WRITE;
    assert(NT_SUCCESS((NTSTATUS)SvNtConsoleManage(secured_args)));
    HANDLE write_only = (HANDLE)count;
    POBJECT probe = NULL;
    assert(NT_SUCCESS(ObReferenceObjectByHandle(write_only, GENERIC_WRITE,
                                                file_type, &probe)));
    ObDereferenceObject(probe);
    probe = NULL;
    assert(ObReferenceObjectByHandle(write_only, GENERIC_READ, file_type, &probe) ==
           STATUS_ACCESS_DENIED);
    secured_args[0] = SV_CONSOLE_ACTIVATE_BUFFER;
    secured_args[1] = (UINT64)process_a.StandardOutput;
    assert(NT_SUCCESS((NTSTATUS)SvNtConsoleManage(secured_args)));
    ObCloseHandle(write_only);
    ObCloseHandle(secured);
    puts("PASS captured screen-buffer DACL, ordered access checks and inheritable real handles");
    current = &thread_host;
    assert(NT_SUCCESS(con_attach_process(&process_host, a->Console, 8, TRUE)));
    a->Console->HostProcessId = process_host.ProcessId;
    UINT64 args[5] = {SV_CONSOLE_CONNECT_HOST, (UINT64)process_host.ProcessId << 16, 0, 0, 0};
    assert(NT_SUCCESS((NTSTATUS)SvNtConsoleManage(args)));
    current = &thread_a;
    assert(NT_SUCCESS(ConWaitForProcessConsoleHost(&process_a)));
    a->Console->HostStatus = STATUS_UNSUCCESSFUL;
    assert(ConWaitForProcessConsoleHost(&process_a) == STATUS_UNSUCCESSFUL);
    a->Console->HostStatus = STATUS_SUCCESS;
    key(a->Console, 'z', 1, 0);
    deny_after_gate = (UINT64)chars;
    assert(con_endpoint_control(input, SV_CONSOLE_CONTROL_READ_WIDE, (UINT64)chars, 1, 0, &count) ==
           STATUS_ACCESS_VIOLATION);
    deny_address = 0;
    HANDLE alias = NULL;
    assert(NT_SUCCESS(ConOpenProcessConsole(FALSE, &alias)));
    CON_ENDPOINT *saved = endpoint(alias);
    ConDetachProcessConsole(&process_a);
    assert(!process_a.NativeConsole && !process_a.StandardInput && !process_a.StandardOutput);
    assert(con_endpoint_write_result(saved, text, sizeof(text), &transferred) ==
           STATUS_INVALID_HANDLE);
    current = &thread_host;
    ConDetachProcessConsole(&process_host);
    current = &thread_b;
    ConDetachProcessConsole(&process_b);
    ObDestroyHandleTable(process_a.HandleTable);
    ObDestroyHandleTable(process_b.HandleTable);
    ObDestroyHandleTable(process_host.HandleTable);
    assert(g_console_type->TotalObjects == 0 && g_screen_type->TotalObjects == 0 &&
           file_type->TotalObjects == 0 && allocations == baseline);
    puts("PASS host readiness/failure, post-wait user probes, detach invalidation and exact "
         "resource balance");
    return 0;
}
