/* execve replaces one Linux process image without changing its process ID.
 * ELF images map the Linux way; PE images convert the thread into a Windows
 * process so shells (xterm) can run either personality as one program set. */
#include "Internal.h"
#include <svindows/AddressSpace.h>
#include <svindows/Ex.h>
#include <svindows/Io.h>
#include <svindows/Ldr.h>
#include <svindows/Ob.h>
#include <svindows/Ps.h>
#include <svindows/Rtl.h>

#define LINUX_EXEC_VECTOR_MAX 64u
#define LINUX_EXEC_TEXT_MAX 32768u
#define LINUX_COMMAND_MAX 768u /* PE process-parameter command line limit */

typedef struct _LINUX_EXEC_CAPTURE {
    char Path[260];
    const char *Argv[LINUX_EXEC_VECTOR_MAX];
    const char *Env[LINUX_EXEC_VECTOR_MAX];
    UINT32 Argc;
    UINT32 Envc;
    UINT32 TextUsed;
    char Text[LINUX_EXEC_TEXT_MAX];
} LINUX_EXEC_CAPTURE;

static UINT64 capture_vector(UINT64 user, const char **output,
                             UINT32 *count, LINUX_EXEC_CAPTURE *capture)
{
    *count = 0;
    if (!user) return 0;
    for (UINT32 i = 0; i <= LINUX_EXEC_VECTOR_MAX; i++) {
        UINT64 item_address = user + (UINT64)i * sizeof(UINT64);
        if (item_address < user ||
            !MmProbeForRead(item_address, sizeof(UINT64)))
            return LinuxError(LINUX_EFAULT);
        UINT64 item = *(const UINT64 *)(ULONG_PTR)item_address;
        if (!item) return 0;
        if (i == LINUX_EXEC_VECTOR_MAX)
            return LinuxError(LINUX_E2BIG);
        output[i] = &capture->Text[capture->TextUsed];
        for (;;) {
            if (capture->TextUsed >= LINUX_EXEC_TEXT_MAX)
                return LinuxError(LINUX_E2BIG);
            if (!MmProbeForRead(item, 1))
                return LinuxError(LINUX_EFAULT);
            char ch = *(const char *)(ULONG_PTR)item++;
            capture->Text[capture->TextUsed++] = ch;
            if (!ch) break;
        }
        (*count)++;
    }
    return LinuxError(LINUX_E2BIG);
}

static void detach_framebuffer_aliases(UINT64 root,
                                       const LINUX_MEMORY_CONTEXT *memory)
{
    for (UINT32 i = 0; i < LINUX_MAP_REGIONS; i++) {
        const LINUX_MAP_REGION *region = &memory->Regions[i];
        if (region->Kind != LINUX_MAP_FRAMEBUFFER || !region->Base) continue;
        for (UINT64 va = region->Base; va < region->Base + region->Size;
             va += PAGE_SIZE)
            MmUnmapPageInAddressSpace(root, va);
    }
}

static BOOLEAN linux_is_pe_image(const void *image, SIZE_T size)
{
    if (size < 0x40) return FALSE;
    const UINT8 *bytes = image;
    if (bytes[0] != 'M' || bytes[1] != 'Z') return FALSE;
    UINT32 offset = *(const UINT32 *)(bytes + 0x3c);
    if (offset < 0x40 || offset > size - 4) return FALSE;
    return bytes[offset] == 'P' && bytes[offset + 1] == 'E' &&
           bytes[offset + 2] == 0 && bytes[offset + 3] == 0;
}

static BOOLEAN linux_argument_needs_quotes(const char *text)
{
    if (!text[0]) return TRUE;
    for (SIZE_T i = 0; text[i]; i++)
        if (text[i] == ' ' || text[i] == '\t' || text[i] == '"') return TRUE;
    return FALSE;
}

static BOOLEAN linux_command_character(char *out, SIZE_T capacity,
                                       SIZE_T *length, char ch)
{
    if (*length + 1 >= capacity) return FALSE;
    out[(*length)++] = ch;
    return TRUE;
}

/* Windows command-line quoting: double backslashes before a quote (and at
 * the end of a quoted argument), then escape literal quotes. */
static BOOLEAN linux_build_windows_command(const char *const *argv,
                                           UINT32 argc, char *out,
                                           SIZE_T capacity)
{
    SIZE_T n = 0;
    if (!argc) return FALSE;
    for (UINT32 i = 0; i < argc; i++) {
        const char *argument = argv[i] ? argv[i] : "";
        if (i && !linux_command_character(out, capacity, &n, ' '))
            return FALSE;
        BOOLEAN quoted = linux_argument_needs_quotes(argument);
        if (quoted && !linux_command_character(out, capacity, &n, '"'))
            return FALSE;
        for (SIZE_T j = 0; argument[j];) {
            SIZE_T slashes = 0;
            while (argument[j] == '\\') { slashes++; j++; }
            if (argument[j] == '"') {
                for (SIZE_T k = 0; k < slashes * 2 + 1; k++)
                    if (!linux_command_character(out, capacity, &n, '\\'))
                        return FALSE;
                if (!linux_command_character(out, capacity, &n, '"'))
                    return FALSE;
                j++;
            } else {
                SIZE_T count = argument[j] ? slashes :
                                 slashes * (quoted ? 2 : 1);
                for (SIZE_T k = 0; k < count; k++)
                    if (!linux_command_character(out, capacity, &n, '\\'))
                        return FALSE;
                if (argument[j] &&
                    !linux_command_character(out, capacity, &n,
                                             argument[j++]))
                    return FALSE;
            }
        }
        if (quoted && !linux_command_character(out, capacity, &n, '"'))
            return FALSE;
    }
    out[n] = 0;
    return TRUE;
}

static BOOLEAN linux_windows_search_path(const char *source, char out[512])
{
    if (strlen(source) < 5 || memcmp(source, "PATH=", 5)) return FALSE;
    SIZE_T n = 0;
    const char *prefix = "PATH=";
    while (*prefix) out[n++] = *prefix++;
    const char *part = source + 5;
    while (*part) {
        if (part != source + 5) {
            if (n + 1 >= 512) return FALSE;
            out[n++] = ';';
        }
        if (*part == '/') {
            if (n + 3 >= 512) return FALSE;
            out[n++] = 'C';
            out[n++] = ':';
        }
        while (*part && *part != ':') {
            if (n + 1 >= 512) return FALSE;
            out[n++] = *part == '/' ? '\\' : *part;
            part++;
        }
        if (*part == ':') part++;
    }
    static const char native_search[] = ";C:\\;C:\\windows\\system32";
    if (n + sizeof(native_search) > 512) return FALSE;
    memcpy(out + n, native_search, sizeof(native_search));
    return TRUE;
}

static void linux_update_process_name(PKPROCESS process, const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    char *name = (char *)(ULONG_PTR)process->Name;
    if (!name) return;
    UINT32 n = 0;
    while (base[n] && n + 1 < 64) {
        name[n] = base[n];
        n++;
    }
    name[n] = 0;
}

/* execve onto a PE image: same pid, same handle table, new Windows
 * personality. The thread re-enters ring 3 through the syscall exit stub's
 * Windows-exec path, so nothing here returns to the Linux caller. */
static UINT64 linux_exec_windows(PKTHREAD thread, PKPROCESS process,
                                 const LINUX_EXEC_CAPTURE *capture)
{
    UINT64 old_root = process->DirectoryTableBase;
    UINT64 new_root = MmCreateAddressSpace();
    if (new_root == MM_INVALID_PHYS) return LinuxError(LINUX_ENOMEM);

    /* Convert the standard streams before touching the personality, so a
     * failure leaves the running Linux image fully intact. */
    HANDLE standard[3] = {NULL, NULL, NULL};
    static const BOOLEAN write_direction[3] = {FALSE, TRUE, TRUE};
    for (UINT32 i = 0; i < 3; i++) {
        if (!process->LinuxContext->Files[i].Used) continue;
        NTSTATUS status = LinuxStdHandleFromFd(process, i, write_direction[i],
                                               &standard[i]);
        if (!NT_SUCCESS(status)) {
            for (UINT32 j = 0; j < i; j++)
                if (standard[j]) ObCloseHandle(standard[j]);
            MmDestroyAddressSpace(new_root);
            return LinuxError(LINUX_EBADF);
        }
    }

    char command[LINUX_COMMAND_MAX];
    if (!linux_build_windows_command(capture->Argv, capture->Argc, command,
                                     sizeof(command))) {
        for (UINT32 i = 0; i < 3; i++)
            if (standard[i]) ObCloseHandle(standard[i]);
        MmDestroyAddressSpace(new_root);
        return LinuxError(LINUX_E2BIG);
    }

    /* Forked Linux processes never owned a loader context; the PE image
     * being exec'd needs one for its module list. */
    BOOLEAN created_loader = FALSE;
    if (!process->LoaderContext) {
        process->LoaderContext = LdrCreateProcessContext();
        created_loader = process->LoaderContext != NULL;
        if (!created_loader) {
            for (UINT32 i = 0; i < 3; i++)
                if (standard[i]) ObCloseHandle(standard[i]);
            MmDestroyAddressSpace(new_root);
            return LinuxError(LINUX_ENOMEM);
        }
    }

    /* Publish the Windows identity only after the switch succeeds; keep the
     * previous values so a failure restores the running Linux image. */
    LINUX_MEMORY_CONTEXT *old_memory = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*old_memory), 'mxeL');
    if (!old_memory) {
        for (UINT32 i = 0; i < 3; i++)
            if (standard[i]) ObCloseHandle(standard[i]);
        if (created_loader) {
            LdrDestroyProcessContext(process->LoaderContext);
            process->LoaderContext = NULL;
        }
        MmDestroyAddressSpace(new_root);
        return LinuxError(LINUX_ENOMEM);
    }
    *old_memory = process->LinuxContext->Memory;

    PS_ENVIRONMENT_SNAPSHOT old_environment;
    PsCaptureProcessEnvironment(process, &old_environment);
    const char *windows_environment[LINUX_EXEC_VECTOR_MAX];
    char windows_path[512];
    BOOLEAN path_converted = FALSE;
    for (UINT32 i = 0; i < capture->Envc; i++) {
        windows_environment[i] = capture->Env[i];
        if (!path_converted &&
            linux_windows_search_path(capture->Env[i], windows_path)) {
            windows_environment[i] = windows_path;
            path_converted = TRUE;
        }
    }
    NTSTATUS env_status = PsSetProcessEnvironment(process,
        windows_environment, capture->Envc);
    if (!NT_SUCCESS(env_status)) {
        PsReleaseEnvironmentSnapshot(&old_environment);
        ExFreePool(old_memory);
        for (UINT32 i = 0; i < 3; i++)
            if (standard[i]) ObCloseHandle(standard[i]);
        if (created_loader) {
            LdrDestroyProcessContext(process->LoaderContext);
            process->LoaderContext = NULL;
        }
        MmDestroyAddressSpace(new_root);
        return LinuxError(LINUX_E2BIG);
    }

    HANDLE old_input = process->StandardInput;
    HANDLE old_output = process->StandardOutput;
    HANDLE old_error = process->StandardError;
    char old_path[260];
    memcpy(old_path, process->ImagePath, sizeof(old_path));
    char old_name[64];
    memcpy(old_name, process->Name, sizeof(old_name));

    process->StandardInput = standard[0];
    process->StandardOutput = standard[1];
    process->StandardError = standard[2];
    memcpy(process->ImagePath, capture->Path, sizeof(process->ImagePath));
    linux_update_process_name(process, capture->Path);

    MmSwitchAddressSpace(new_root);
    process->DirectoryTableBase = new_root;
    NTSTATUS status = PsSwitchThreadToWindows(thread, process,
                                              capture->Path, command);
    if (!NT_SUCCESS(status)) {
        process->DirectoryTableBase = old_root;
        MmSwitchAddressSpace(old_root);
        MmDestroyAddressSpace(new_root);
        process->StandardInput = old_input;
        process->StandardOutput = old_output;
        process->StandardError = old_error;
        memcpy(process->ImagePath, old_path, sizeof(old_path));
        memcpy((char *)(ULONG_PTR)process->Name, old_name,
               sizeof(old_name));
        PsRestoreProcessEnvironment(process, &old_environment);
        for (UINT32 i = 0; i < 3; i++)
            if (standard[i]) ObCloseHandle(standard[i]);
        if (created_loader) {
            LdrDestroyProcessContext(process->LoaderContext);
            process->LoaderContext = NULL;
        }
        ExFreePool(old_memory);
        return LinuxError(status == STATUS_NO_MEMORY ? LINUX_ENOMEM :
                          status == STATUS_BUFFER_TOO_SMALL ? LINUX_E2BIG : 8);
    }

    /* Point of no return: the Windows environment is mapped. Give the
     * standard streams Windows-console semantics, retire the Linux
     * descriptor table, and drop the old address space. */
    for (UINT32 i = 0; i < 3; i++)
        if (standard[i]) IoPrepareFileForWindows(standard[i]);
    HANDLE old_standard[3] = {old_input, old_output, old_error};
    BOOLEAN old_standard_closed_by_fd[3] = {FALSE, FALSE, FALSE};
    for (UINT32 fd = 0; fd < LINUX_MAX_FDS; fd++) {
        LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
        if (!entry->Used || !entry->Owned ||
            entry->Kind != LINUX_FD_REGULAR) continue;
        for (UINT32 i = 0; i < 3; i++)
            if (old_standard[i] && entry->Handle == old_standard[i])
                old_standard_closed_by_fd[i] = TRUE;
    }
    for (UINT32 i = 0; i < LINUX_MAX_FDS; i++)
        if (process->LinuxContext->Files[i].Used) LinuxClose(process, i);
    LinuxDestroyProcess(process);
    for (UINT32 i = 0; i < 3; i++) {
        if (!old_standard[i] || old_standard_closed_by_fd[i]) continue;
        BOOLEAN already_closed = FALSE;
        for (UINT32 j = 0; j < i; j++)
            if (old_standard[i] == old_standard[j]) already_closed = TRUE;
        if (!already_closed) ObCloseHandle(old_standard[i]);
    }
    detach_framebuffer_aliases(old_root, old_memory);
    LinuxReleaseFramebufferOwner(old_memory);
    MmDestroyAddressSpace(old_root);
    ExFreePool(old_memory);
    PsReleaseEnvironmentSnapshot(&old_environment);
    return 0;
}

UINT64 LinuxExecve(PKTHREAD thread, UINT64 path, UINT64 argv, UINT64 envp)
{
    PKPROCESS process = thread->Process;
    for (PLIST_ENTRY e = process->ThreadListHead.Flink;
         e != &process->ThreadListHead; e = e->Flink) {
        PKTHREAD other = CONTAINING_RECORD(e, KTHREAD, ProcessEntry);
        if (other != thread && other->State != ThreadStateTerminated)
            return LinuxError(LINUX_EOPNOTSUPP);
    }
    LINUX_EXEC_CAPTURE *capture = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*capture), 'xecL');
    if (!capture) return LinuxError(LINUX_ENOMEM);
    memset(capture, 0, sizeof(*capture));
    UINT64 error = LinuxCapturePath(process, path, capture->Path);
    if (!error) {
        UINT8 proc = LinuxProcPathKind(capture->Path);
        if (proc == LINUX_PROC_EXE)
            memcpy(capture->Path, process->ImagePath,
                   sizeof(capture->Path));
        else if (proc != LINUX_PROC_NONE)
            error = LinuxError(proc == LINUX_PROC_UNKNOWN ?
                               LINUX_ENOENT : LINUX_EACCES);
    }
    if (!error)
        error = capture_vector(argv, capture->Argv, &capture->Argc, capture);
    if (!error)
        error = capture_vector(envp, capture->Env, &capture->Envc, capture);
    if (error) {
        ExFreePool(capture);
        return error;
    }

    void *image = NULL;
    SIZE_T size = 0;
    NTSTATUS status = FatLoadFile(capture->Path, &image, &size, NULL);
    if (!NT_SUCCESS(status)) {
        ExFreePool(capture);
        return LinuxError(status == STATUS_OBJECT_NAME_NOT_FOUND ||
                          status == STATUS_OBJECT_PATH_NOT_FOUND ||
                          status == STATUS_NO_SUCH_FILE ? LINUX_ENOENT :
                                                          LINUX_EIO);
    }
    if (linux_is_pe_image(image, size)) {
        KeTrace("[linux-exec] PE image '%s' (%lu bytes), switching pid %u\n",
                capture->Path, (unsigned long)size, process->ProcessId);
        ExFreePool(image);
        error = linux_exec_windows(thread, process, capture);
        if (error)
            KeLog("[linux-exec] PE switch for '%s' returned Linux error %ld\n",
                  capture->Path, (long)(INT64)error);
        ExFreePool(capture);
        return error;
    }
    if (!LinuxIsElfImage(image, size)) {
        ExFreePool(image);
        ExFreePool(capture);
        return LinuxError(8); /* ENOEXEC */
    }
    LINUX_MEMORY_CONTEXT *old_memory = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*old_memory), 'mxeL');
    if (!old_memory) {
        ExFreePool(image);
        ExFreePool(capture);
        return LinuxError(LINUX_ENOMEM);
    }
    *old_memory = process->LinuxContext->Memory;
    PS_ENVIRONMENT_SNAPSHOT old_environment;
    PsCaptureProcessEnvironment(process, &old_environment);
    status = PsSetProcessEnvironment(process, capture->Env, capture->Envc);
    if (!NT_SUCCESS(status)) {
        PsReleaseEnvironmentSnapshot(&old_environment);
        ExFreePool(old_memory);
        ExFreePool(image);
        ExFreePool(capture);
        return LinuxError(LINUX_E2BIG);
    }
    char old_path[260];
    memcpy(old_path, process->ImagePath, sizeof(old_path));
    UINT64 old_root = process->DirectoryTableBase;
    UINT64 new_root = MmCreateAddressSpace();
    if (new_root == MM_INVALID_PHYS) {
        PsRestoreProcessEnvironment(process, &old_environment);
        ExFreePool(old_memory);
        ExFreePool(image);
        ExFreePool(capture);
        return LinuxError(LINUX_ENOMEM);
    }
    memcpy(process->ImagePath, capture->Path, sizeof(process->ImagePath));
    process->DirectoryTableBase = new_root;
    MmSwitchAddressSpace(new_root);
    UINT64 entry = 0, stack = 0;
    status = LinuxLoadExecImage(process, image, size,
                                capture->Argv, capture->Argc,
                                capture->Env, capture->Envc, &entry, &stack);
    ExFreePool(image);
    ExFreePool(capture);
    if (!NT_SUCCESS(status)) {
        process->DirectoryTableBase = old_root;
        MmSwitchAddressSpace(old_root);
        MmDestroyAddressSpace(new_root);
        process->LinuxContext->Memory = *old_memory;
        PsRestoreProcessEnvironment(process, &old_environment);
        memcpy(process->ImagePath, old_path, sizeof(process->ImagePath));
        ExFreePool(old_memory);
        return LinuxError(status == STATUS_NO_MEMORY ? LINUX_ENOMEM : 8);
    }

    for (UINT32 i = 0; i < LINUX_MAX_FDS; i++)
        if (process->LinuxContext->Files[i].Used &&
            process->LinuxContext->Files[i].CloseOnExec)
            LinuxClose(process, i);
    linux_update_process_name(process, process->ImagePath);
    detach_framebuffer_aliases(old_root, old_memory);
    LinuxReleaseFramebufferOwner(old_memory);
    MmDestroyAddressSpace(old_root);
    ExFreePool(old_memory);
    PsReleaseEnvironmentSnapshot(&old_environment);
    thread->UserRip = entry;
    thread->UserRsp = stack;
    thread->UserFsBase = 0;
    thread->UserGsBase = 0;
    KeSetUserFsBase(0);
    KeSetUserGsBase(0);
    thread->LinuxThread->CloneStart = FALSE;
    thread->LinuxThread->WindowsExec = FALSE;
    thread->LinuxThread->ExecStart = TRUE;
    return 0;
}
