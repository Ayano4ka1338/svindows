/*
 * ProcessObject.c - minimal waitable user process creation.
 *
 * This is intentionally separate from thread/event services and from the PE
 * loader. It establishes the first real multi-process path while handles and
 * loader contexts are still system-wide transitional facilities.
 */
#include <svindows/Ps.h>
#include <svindows/AddressSpace.h>
#include <svindows/Mm.h>
#include <svindows/Ldr.h>
#include <svindows/Ob.h>
#include <svindows/Rtl.h>

#define PE_COMMAND_MAX 768u
#include <svindows/Win32k.h>
#include <svindows/Io.h>
#include <svindows/Ex.h>
#include <svindows/Dos.h>
#include <svindows/Linux.h>
#include <nt/Peb.h>

#define CHILD_STACK_TOP   0x0000000010010000ULL
/* Match the 1 MiB default PE stack reserve. Large native programs such as
 * Quake keep substantial local tables in their startup path. */
#define CHILD_STACK_PAGES 256

typedef struct _PROCESS_OBJECT {
    DISPATCHER_HEADER Header;
    KPROCESS          Process;
    LIST_ENTRY        GlobalEntry;
    char              Name[64];
} PROCESS_OBJECT;

static POBJECT_TYPE g_process_type;
static ULONG g_next_process_id = 8;
static LIST_ENTRY g_process_list = {&g_process_list, &g_process_list};
static KSPIN_LOCK g_process_list_lock;
#define PS_SYSTEM_SNAPSHOT_LIMIT 256
static POBJECT g_system_snapshot_processes[PS_SYSTEM_SNAPSHOT_LIMIT];
static KE_PROCESS_THREAD_INFO
    g_system_snapshot_threads[PS_SYSTEM_SNAPSHOT_LIMIT][128];
static ULONG g_system_snapshot_thread_counts[PS_SYSTEM_SNAPSHOT_LIMIT];
static volatile ULONG g_system_snapshot_lock;

static void delete_process(POBJECT raw)
{
    PROCESS_OBJECT *object = (PROCESS_OBJECT *)raw;
    KIRQL old_irql;
    KeAcquireSpinLock(&g_process_list_lock, &old_irql);
    if (object->GlobalEntry.Flink != &object->GlobalEntry)
        RemoveEntryList(&object->GlobalEntry);
    KeReleaseSpinLock(&g_process_list_lock, old_irql);
    PsJobProcessExited(&object->Process);
    W32kCleanupProcessUser(&object->Process);
    W32kCleanupProcessGdi(&object->Process);
    LdrDestroyProcessContext(object->Process.LoaderContext);
    object->Process.LoaderContext = NULL;
    MmCleanupProcessSections(&object->Process);
    LinuxDestroyProcess(&object->Process);
    ObDestroyHandleTable(object->Process.HandleTable);
    object->Process.HandleTable = NULL;
    MmDestroyAddressSpace(object->Process.DirectoryTableBase);
    object->Process.DirectoryTableBase = 0;
}

static NTSTATUS capture_ascii(UINT64 address, char *out, SIZE_T capacity)
{
    if (!address || !capacity) return STATUS_INVALID_PARAMETER;
    for (SIZE_T i = 0; i + 1 < capacity; i++) {
        if (address + i < address || !MmProbeForRead(address + i, 1))
            return STATUS_ACCESS_VIOLATION;
        out[i] = *(const char *)(address + i);
        if (!out[i]) return STATUS_SUCCESS;
    }
    out[capacity - 1] = 0;
    return STATUS_BUFFER_TOO_SMALL;
}

NTSTATUS PsSetProcessEnvironment(PKPROCESS process,
                                 const char *const *entries, UINT32 count)
{
    if (!process || count > 64 || (count && !entries))
        return STATUS_INVALID_PARAMETER;
    char block[sizeof(process->Environment)] = {0};
    SIZE_T used = 0;
    for (UINT32 i = 0; i < count; i++) {
        const char *entry = entries[i];
        if (!entry) return STATUS_INVALID_PARAMETER;
        SIZE_T length = strlen(entry) + 1;
        if (length > sizeof(block) - used - 1)
            return STATUS_BUFFER_TOO_SMALL;
        memcpy(block + used, entry, length);
        used += length;
    }
    memcpy(process->Environment, block, sizeof(block));
    return STATUS_SUCCESS;
}

/* Keep a minimal POSIX user environment for ELF children even when the PE
 * parent was created without a complete Windows environment block. */
static BOOLEAN process_environment_has(PKPROCESS process, const char *name)
{
    SIZE_T name_length = strlen(name);
    SIZE_T used = 0;
    while (used < sizeof(process->Environment) && process->Environment[used]) {
        const char *entry = process->Environment + used;
        if (strncmp(entry, name, name_length) == 0 &&
            entry[name_length] == '=')
            return TRUE;
        used += strlen(entry) + 1;
    }
    return FALSE;
}

static BOOLEAN process_image_is_feh(const char *filename)
{
    const char *base = filename;
    for (const char *p = filename; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return (base[0] == 'f' || base[0] == 'F') &&
           (base[1] == 'e' || base[1] == 'E') &&
           (base[2] == 'h' || base[2] == 'H') &&
           (base[3] == 0 ||
            ((base[3] == '.' &&
              (base[4] == 'e' || base[4] == 'E') &&
              (base[5] == 'l' || base[5] == 'L') &&
              (base[6] == 'f' || base[6] == 'F') && !base[7])));
}

static NTSTATUS ensure_elf_environment_defaults(PKPROCESS process,
                                                const char *filename)
{
    const char *entries[64];
    UINT32 count = 0;
    SIZE_T used = 0;
    while (used < sizeof(process->Environment) &&
           process->Environment[used] && count < 64) {
        entries[count++] = process->Environment + used;
        used += strlen(process->Environment + used) + 1;
    }

    static const char *defaults[] = {
        "HOME=/home/user", "USER=user", "LOGNAME=user", "TMPDIR=/tmp"
    };
    for (UINT32 i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        const char *entry = defaults[i];
        SIZE_T name_length = 0;
        while (entry[name_length] && entry[name_length] != '=') name_length++;
        char name[16];
        if (!name_length || name_length >= sizeof(name)) continue;
        memcpy(name, entry, name_length);
        name[name_length] = 0;
        if (!process_environment_has(process, name)) {
            if (count == 64) return STATUS_BUFFER_TOO_SMALL;
            entries[count++] = entry;
        }
    }

    /* Upstream feh supports FEH_SKIP_MAGIC to bypass its optional libmagic
     * pre-filter and pass files to Imlib2's real format loaders. */
    if (process_image_is_feh(filename)) {
        /* Imlib2 discovers image codecs with dlopen().  Alpine's package
         * installs them below /usr/lib/imlib2/loaders; set the path explicitly
         * because this guest does not yet implement Alpine's full filesystem
         * layout or loader configuration files.  Use POSIX paths here: the
         * Linux openat ABI maps them to the FAT-backed C: namespace. */
        static const char *feh_defaults[] = {
            "FEH_SKIP_MAGIC=1",
            "IMLIB2_LOADER_PATH=/usr/lib/imlib2/loaders",
            "IMLIB2_FILTER_PATH=/usr/lib/imlib2/filters"
        };
        for (UINT32 i = 0; i < sizeof(feh_defaults) / sizeof(feh_defaults[0]); i++) {
            const char *entry = feh_defaults[i];
            SIZE_T name_length = 0;
            while (entry[name_length] && entry[name_length] != '=')
                name_length++;
            char name[32];
            if (!name_length || name_length >= sizeof(name)) continue;
            memcpy(name, entry, name_length);
            name[name_length] = 0;
            if (!process_environment_has(process, name)) {
                if (count == 64) return STATUS_BUFFER_TOO_SMALL;
                entries[count++] = entry;
            }
        }
    }
    return PsSetProcessEnvironment(process, entries, count);
}

static BOOLEAN linux_convert_search_path(const char *source, char out[512])
{
    if (strlen(source) < 5 ||
        (source[0] != 'P' && source[0] != 'p') ||
        (source[1] != 'A' && source[1] != 'a') ||
        (source[2] != 'T' && source[2] != 't') ||
        (source[3] != 'H' && source[3] != 'h') || source[4] != '=')
        return FALSE;
    SIZE_T n = 0;
    const char *prefix = "PATH=";
    while (*prefix) out[n++] = *prefix++;
    const char *part = source + 5;
    while (*part) {
        if (part != source + 5) {
            if (n + 1 >= 512) return FALSE;
            out[n++] = ':';
        }
        if ((part[0] == 'C' || part[0] == 'c') && part[1] == ':' &&
            (part[2] == '\\' || part[2] == '/')) {
            part += 2;
        }
        while (*part && *part != ';') {
            if (n + 1 >= 512) return FALSE;
            out[n++] = *part == '\\' ? '/' : *part;
            part++;
        }
        if (*part == ';') part++;
    }
    out[n] = 0;
    return TRUE;
}

static UINT32 process_environment_vector(PKPROCESS process,
                                          const char *entries[64],
                                          char linux_path[512])
{
    SIZE_T used = 0;
    UINT32 count = 0;
    while (used < sizeof(process->Environment) &&
           process->Environment[used] && count < 64) {
        const char *entry = process->Environment + used;
        entries[count++] = entry;
        if (linux_convert_search_path(entry, linux_path))
            entries[count - 1] = linux_path;
        used += strlen(process->Environment + used) + 1;
    }
    return count;
}

static void *map_rw(UINT64 va)
{
    UINT64 pa = MmAllocatePage();
    if (pa == MM_INVALID_PHYS)
        return NULL;
    if (!MmMapPage(va, pa, PTE_USER | PTE_WRITE)) {
        MmFreePages(pa, 1);
        return NULL;
    }
    memset((void *)va, 0, PAGE_SIZE);
    return (void *)va;
}

typedef struct _PE_BOOT_INFO {
    UINT64 Entry;     /* ntdll bootstrap entry (LdrInitializeProcess) */
    UINT64 StackTop;  /* top of the 1 MiB main-thread stack            */
    UINT64 Argument;  /* LDR init block VA, passed to the entry in RCX */
} PE_BOOT_INFO;

/* Map the PE user environment (PEB, shared tables, parameters, TEB, stack,
 * static TLS) into the current address space. CR3 must already be switched
 * to the new image's address space. Returns the thread boot record; the
 * caller either allocates a new thread (process creation) or converts the
 * current one (Linux execve of a PE image). */
static NTSTATUS build_pe_user_environment(PKPROCESS process, const char *name,
                                          const char *command,
                                          UINT64 image_base,
                                          PE_BOOT_INFO *boot)
{
    if (!command || strlen(command) >= PE_COMMAND_MAX)
        return STATUS_BUFFER_TOO_SMALL;
    PPEB peb = map_rw(PROCESS_PEB_VA);
    if (!peb) return STATUS_NO_MEMORY;
    peb->ImageBaseAddress = (PVOID)image_base;

    /* Map the process-owned USER/GDI tables before registering this process
     * with win32k. */
    for (UINT64 off = 0; off < PROCESS_GDI_SHARED_TABLE_SIZE;
         off += PAGE_SIZE) {
        if (!map_rw(PROCESS_GDI_SHARED_TABLE_VA + off))
            return STATUS_NO_MEMORY;
    }
    peb->GdiSharedHandleTable = (PVOID)PROCESS_GDI_SHARED_TABLE_VA;
    for (UINT64 off = 0; off < PROCESS_GDI_ATTR_SIZE; off += PAGE_SIZE) {
        if (!map_rw(PROCESS_GDI_ATTR_VA + off))
            return STATUS_NO_MEMORY;
    }
    W32kInitializeGdi();

    for (UINT64 off = 0; off < PROCESS_USER_SHARED_TABLE_SIZE;
         off += PAGE_SIZE) {
        if (!map_rw(PROCESS_USER_SHARED_TABLE_VA + off))
            return STATUS_NO_MEMORY;
    }
    /* win32k fills HANDLEENTRY cells transiently; user mode only observes
     * them, matching USER32's read-only shared table contract. */
    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA,
                   PROCESS_USER_SHARED_TABLE_SIZE, FALSE);
    for (UINT64 off = 0; off < PROCESS_USER_OBJECT_ARENA_SIZE;
         off += PAGE_SIZE) {
        if (!map_rw(PROCESS_USER_OBJECT_ARENA_VA + off))
            return STATUS_NO_MEMORY;
    }

    for (UINT64 off = 0; off < PROCESS_LDR_SIZE; off += PAGE_SIZE)
        if (!map_rw(PROCESS_LDR_VA + off)) return STATUS_NO_MEMORY;
    LdrBuildProcessModuleList(peb, PROCESS_LDR_VA, PROCESS_LDR_SIZE);

    UINT8 *params = map_rw(PROCESS_PARAMS_VA);
    if (!params) return STATUS_NO_MEMORY;
    UINT16 *envw = map_rw(PROCESS_ENV_VA);
    if (!envw) return STATUS_NO_MEMORY;
    SIZE_T env_chars = 0;
    for (SIZE_T i = 0; i < sizeof(process->Environment) &&
         process->Environment[i];) {
        do {
            envw[env_chars++] = (UINT8)process->Environment[i++];
        } while (i < sizeof(process->Environment) &&
                 process->Environment[i]);
        envw[env_chars++] = 0;
        /* Advance past the separator; otherwise the outer loop sees the same
         * NUL again and exposes only the first environment variable. */
        i++;
    }
    envw[env_chars] = 0;
    *(void **)(params + 0x80) = envw;
    UINT16 *imagew = (UINT16 *)(params + 0x200);
    UINT16 *cmdw = (UINT16 *)(params + 0x400);
    UINT16 *cwdw = (UINT16 *)(params + 0xa00);
    UINT16 image_n = 0, cmd_n = 0;
    while (name[image_n] && image_n < 200) {
        imagew[image_n] = (UINT8)name[image_n]; image_n++;
    }
    while (command[cmd_n] && cmd_n < PE_COMMAND_MAX - 1) {
        cmdw[cmd_n] = (UINT8)command[cmd_n]; cmd_n++;
    }
    *(UINT16 *)(params + 0x60) = image_n * 2;
    *(UINT16 *)(params + 0x62) = image_n * 2 + 2;
    *(void **)(params + 0x68) = imagew;
    *(UINT16 *)(params + 0x70) = cmd_n * 2;
    *(UINT16 *)(params + 0x72) = cmd_n * 2 + 2;
    *(void **)(params + 0x78) = cmdw;
    UINT16 cwd_n = 0;
    while (process->CurrentDirectory[cwd_n] && cwd_n < 259) {
        cwdw[cwd_n] = (UINT8)process->CurrentDirectory[cwd_n];
        cwd_n++;
    }
    cwdw[cwd_n] = 0;
    /* RTL_USER_PROCESS_PARAMETERS.CurrentDirectory.DosPath at +0x38. */
    *(UINT16 *)(params + 0x38) = cwd_n * 2;
    *(UINT16 *)(params + 0x3a) = 260 * sizeof(UINT16);
    *(void **)(params + 0x40) = cwdw;
    peb->ProcessParameters = params;
    *(HANDLE *)(params + 0x20) = process->StandardInput;
    *(HANDLE *)(params + 0x28) = process->StandardOutput;
    *(HANDLE *)(params + 0x30) = process->StandardError;
    /* Wide command line ends at 0xa00, the cwd at 0xc08, and the ANSI
     * copy occupies 0xd00..0xfff. Keep all three strings disjoint. */
    char *cmda = (char *)(params + 0xd00);
    for (UINT16 i = 0; i <= cmd_n; i++) cmda[i] = command[i];
    LdrSeedCrtCommandLine((UINT64)cmda, (UINT64)cmdw);

    for (UINT64 off = 0; off < PROCESS_TEB_SIZE; off += PAGE_SIZE)
        if (!map_rw(PROCESS_MAIN_TEB_VA + off)) return STATUS_NO_MEMORY;
    PTEB teb = (PTEB)PROCESS_MAIN_TEB_VA;
    teb->NtTib.Self = (struct _NT_TIB *)PROCESS_MAIN_TEB_VA;
    teb->ProcessEnvironmentBlock = (PVOID)PROCESS_PEB_VA;
    /* Avoid USER32's obsolete Windows-3.x fast path when a genuine GUI DLL is
     * loaded later.  This is CLIENTINFO.dwExpWinVer at TEB+0x810. */
    *(UINT32 *)((UINT8 *)teb + 0x810) = 0x0A00;
    teb->ClientId.UniqueProcess =
        (HANDLE)(ULONG_PTR)process->ProcessId;

    UINT64 stack_base = CHILD_STACK_TOP - CHILD_STACK_PAGES * PAGE_SIZE;
    for (UINT64 off = 0; off < CHILD_STACK_PAGES * PAGE_SIZE; off += PAGE_SIZE)
        if (!map_rw(stack_base + off)) return STATUS_NO_MEMORY;
    teb->NtTib.StackBase = (PVOID)CHILD_STACK_TOP;
    teb->NtTib.StackLimit = (PVOID)stack_base;

    /* Static TLS: allocate this thread's block vector now that every module
     * (and so every TLS index) is known, and publish the index count so
     * kernel32's TlsAlloc continues the same space. */
    NTSTATUS tls_status = LdrApplyThreadTls(PROCESS_MAIN_TEB_VA);
    if (!NT_SUCCESS(tls_status)) return tls_status;
    peb->LdrpTlsNextIndex = LdrTlsIndexCount();
    peb->LdrpTlsSlotCount = 64;

    boot->StackTop = CHILD_STACK_TOP;
    return STATUS_SUCCESS;
}

static NTSTATUS build_environment(PROCESS_OBJECT *object, const char *command,
                                  UINT64 image_base, UINT64 entry,
                                  UINT64 argument, PKTHREAD *thread_out)
{
    PE_BOOT_INFO boot;
    boot.Entry = entry;
    NTSTATUS status = build_pe_user_environment(&object->Process,
                                                object->Name, command,
                                                image_base, &boot);
    if (!NT_SUCCESS(status)) return status;
    PKTHREAD thread = KeCreateUserThreadForProcess(
        &object->Process, object->Name, boot.Entry, boot.StackTop,
        PROCESS_MAIN_TEB_VA, argument, object->Process.BasePriority);
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

#include "ProcessTerminate.inc"

NTSTATUS PsQuerySystemProcessInformation(void *buffer, ULONG length,
                                         ULONG *return_length)
{
    if (return_length && !MmProbeForWrite((UINT64)return_length, sizeof(ULONG)))
        return STATUS_ACCESS_VIOLATION;
    while (__atomic_exchange_n(&g_system_snapshot_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");

    ULONG process_count = 0, cursor = 0;
    while (process_count < PS_SYSTEM_SNAPSHOT_LIMIT) {
        ULONG pid = 0;
        POBJECT object = PsReferenceNextProcess(cursor, &pid);
        if (!object) break;
        g_system_snapshot_processes[process_count] = object;
        PKPROCESS process = PsGetProcessFromObject(object);
        ULONG threads = KeSnapshotProcessThreads(process, NULL, 0);
        if (threads > 128) {
            ObDereferenceObject(object);
            break;
        }
        g_system_snapshot_thread_counts[process_count] = threads;
        KeSnapshotProcessThreads(process, g_system_snapshot_threads[process_count],
                                 threads);
        process_count++;
        cursor = pid;
    }

    ULONG required = 0;
    for (ULONG i = 0; i < process_count; ++i) {
        PKPROCESS process = PsGetProcessFromObject(g_system_snapshot_processes[i]);
        const char *name = process->ImagePath;
        ULONG chars = 0, base = 0;
        while (name[chars] && chars < 259) {
            if (name[chars] == '\\' || name[chars] == '/') base = chars + 1;
            chars++;
        }
        ULONG visible_chars = chars - base;
        ULONG threads = g_system_snapshot_thread_counts[i];
        ULONG row = 256 + threads * 80 + (visible_chars + 1) * sizeof(UINT16);
        row = (row + 7) & ~7u;
        if (row > 0xffffffffu - required) {
            required = 0xffffffffu;
            break;
        }
        required += row;
    }
    if (return_length) *return_length = required;
    NTSTATUS result = STATUS_SUCCESS;
    if (required == 0xffffffffu || length < required) {
        result = STATUS_INFO_LENGTH_MISMATCH;
        goto cleanup;
    }
    if (required && !MmProbeForWrite((UINT64)buffer, required)) {
        result = STATUS_ACCESS_VIOLATION;
        goto cleanup;
    }

    UINT8 *bytes = buffer;
    ULONG offset = 0;
    for (ULONG i = 0; i < process_count; ++i) {
        PKPROCESS process = PsGetProcessFromObject(g_system_snapshot_processes[i]);
        UINT8 *row = bytes + offset;
        ULONG thread_count = g_system_snapshot_thread_counts[i];
        const char *image = process->ImagePath;
        ULONG chars = 0, base = 0;
        while (image[chars] && chars < 259) {
            if (image[chars] == '\\' || image[chars] == '/') base = chars + 1;
            chars++;
        }
        ULONG visible_chars = chars - base;
        ULONG row_size = 256 + thread_count * 80 +
                         (visible_chars + 1) * sizeof(UINT16);
        row_size = (row_size + 7) & ~7u;
        for (ULONG z = 0; z < row_size; ++z) row[z] = 0;
        *(ULONG *)(row + 4) = thread_count;
        *(ULONG *)(row + 16) = 0; /* hard faults are not counted per process */
        *(UINT64 *)(row + 32) = 0; /* process create time not yet accounted */
        *(UINT64 *)(row + 40) = __atomic_load_n(&process->UserTime,__ATOMIC_RELAXED);
        *(UINT64 *)(row + 48) = __atomic_load_n(&process->KernelTime,__ATOMIC_RELAXED);
        *(UINT16 *)(row + 56) = (UINT16)(visible_chars * sizeof(UINT16));
        *(UINT16 *)(row + 58) = (UINT16)((visible_chars + 1) * sizeof(UINT16));
        *(UINT64 *)(row + 64) = (UINT64)(row + 256 + thread_count * 80);
        *(LONG *)(row + 72) = process->BasePriority;
        *(UINT64 *)(row + 80) = process->ProcessId;
        *(UINT64 *)(row + 88) = process->ParentProcessId;
        *(ULONG *)(row + 96) = ObCountOpenHandles(process->HandleTable);
        *(ULONG *)(row + 100) = 0; /* this build has one interactive session */
        UINT64 virtual_bytes = 0, committed_bytes = 0;
        for (ULONG r = 0; r < MM_MAX_PROCESS_REGIONS; ++r) {
            MM_PROCESS_REGION *region = &process->VmRegions[r];
            if (!region->InUse) continue;
            virtual_bytes += region->ReserveSize;
            committed_bytes += region->CommittedPages * PAGE_SIZE;
        }
        *(UINT64 *)(row + 112) = virtual_bytes;
        *(UINT64 *)(row + 120) = virtual_bytes;
        UINT64 flags = KiIrqSave();
        UINT64 private_pages;
        UINT64 working_set = MmCountResidentUserPages(process->DirectoryTableBase,
                                                      &private_pages) * PAGE_SIZE;
        if (working_set > process->PeakWorkingSetBytes)
            process->PeakWorkingSetBytes = working_set;
        UINT64 sampled_peak = process->PeakWorkingSetBytes;
        KiIrqRestore(flags);
        *(UINT64 *)(row + 8) = private_pages * PAGE_SIZE;
        *(UINT64 *)(row + 136) = sampled_peak;
        *(UINT64 *)(row + 144) = working_set;
        *(UINT64 *)(row + 184) = committed_bytes;
        *(UINT64 *)(row + 192) = committed_bytes;
        *(UINT64 *)(row + 200) = committed_bytes;
        for (ULONG c = 0; c < visible_chars; ++c)
            *(UINT16 *)(row + 256 + thread_count * 80 + c * 2) =
                (UINT8)image[base + c];
        UINT8 *thread_out = row + 256;
        for (ULONG t = 0; t < thread_count; ++t) {
            KE_PROCESS_THREAD_INFO *thread = &g_system_snapshot_threads[i][t];
            UINT8 *entry = thread_out + t * 80;
            *(UINT64 *)(entry + 0) = thread->KernelTime;
            *(UINT64 *)(entry + 8) = thread->UserTime;
            *(UINT64 *)(entry + 40) = process->ProcessId;
            *(UINT64 *)(entry + 48) = thread->ThreadId;
            *(LONG *)(entry + 56) = thread->Priority;
            *(LONG *)(entry + 60) = thread->BasePriority;
            *(ULONG *)(entry + 64) = thread->ContextSwitches;
            *(ULONG *)(entry + 68) = thread->State;
        }
        if (i + 1 < process_count) *(ULONG *)row = row_size;
        offset += row_size;
    }

cleanup:
    for (ULONG i = 0; i < process_count; ++i)
        ObDereferenceObject(g_system_snapshot_processes[i]);
    __atomic_store_n(&g_system_snapshot_lock, 0, __ATOMIC_RELEASE);
    return result;
}

UINT64 SvNtReadProcessMemory(UINT64 *a)
{
    HANDLE handle = (HANDLE)(ULONG_PTR)a[0];
    UINT64 source = a[1], destination = a[2], size = a[3];
    UINT64 *bytes_read = (UINT64 *)a[4];
    if (bytes_read && !MmProbeForWrite((UINT64)bytes_read, sizeof(*bytes_read)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (bytes_read) *bytes_read = 0;
    if (size > 0x7fffffffULL || source + size < source ||
        (size && !MmProbeForWrite(destination, size)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    PKPROCESS process = NULL;
    POBJECT referenced = NULL;
    if ((ULONG_PTR)handle == ~(ULONG_PTR)0) {
        PKTHREAD thread = KeGetCurrentThread();
        process = thread ? thread->Process : NULL;
    } else {
        NTSTATUS status = ObReferenceObjectByHandle(handle, 0x0010u, NULL,
                                                    &referenced);
        if (!NT_SUCCESS(status)) return (UINT64)status;
        process = PsGetProcessFromObject(referenced);
    }
    if (!process) {
        if (referenced) ObDereferenceObject(referenced);
        return (UINT64)STATUS_INVALID_HANDLE;
    }
    UINT64 copied = 0;
    while (copied < size) {
        UINT64 address = source + copied;
        UINT64 physical = MmGetPhysicalAddressInAddressSpace(
            process->DirectoryTableBase, address);
        if (physical == MM_INVALID_PHYS) break;
        UINT64 chunk = PAGE_SIZE - (address & PAGE_MASK);
        if (chunk > size - copied) chunk = size - copied;
        const UINT8 *input = MmPhysToVirt(physical);
        UINT8 *output = (UINT8 *)(ULONG_PTR)(destination + copied);
        for (UINT64 i = 0; i < chunk; ++i) output[i] = input[i];
        copied += chunk;
    }
    if (bytes_read) *bytes_read = copied;
    if (referenced) ObDereferenceObject(referenced);
    return copied == size ? (UINT64)STATUS_SUCCESS
                          : (UINT64)STATUS_ACCESS_VIOLATION;
}

/* Return a referenced process with the smallest ID greater than the cursor.
 * Callers can enumerate safely without retaining the process-list lock while
 * they inspect or act on a process. */
void *PsReferenceNextProcess(ULONG after_process_id, ULONG *process_id)
{
    if (process_id) *process_id = 0;
    POBJECT found = NULL;
    ULONG best_id = ~(ULONG)0;
    KIRQL old_irql;
    KeAcquireSpinLock(&g_process_list_lock, &old_irql);
    for (PLIST_ENTRY entry = g_process_list.Flink;
         entry != &g_process_list; entry = entry->Flink) {
        PROCESS_OBJECT *object = CONTAINING_RECORD(entry, PROCESS_OBJECT,
                                                    GlobalEntry);
        ULONG id = object->Process.ProcessId;
        if (id <= after_process_id || id >= best_id) continue;
        found = (POBJECT)object;
        best_id = id;
    }
    if (found) ObReferenceObject(found);
    KeReleaseSpinLock(&g_process_list_lock, old_irql);
    if (found && process_id) *process_id = best_id;
    return found;
}

/* NtOpenProcess(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PCLIENT_ID). */
UINT64 NtOpenProcess(UINT64 *a)
{
    typedef struct { HANDLE Process; HANDLE Thread; } CLIENT_ID_LOCAL;
    PHANDLE output = (PHANDLE)a[0];
    const CLIENT_ID_LOCAL *id = (const CLIENT_ID_LOCAL *)a[3];
    if (!MmProbeForWrite((UINT64)output, sizeof(*output)) ||
        !MmProbeForRead((UINT64)id, sizeof(*id)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    *output = NULL;
    if (a[2]) return (UINT64)STATUS_NOT_SUPPORTED;
    ULONG pid = (ULONG)(ULONG_PTR)id->Process;
    if (!pid || id->Thread) return (UINT64)STATUS_INVALID_PARAMETER;
    POBJECT found = NULL;
    KIRQL old_irql;
    KeAcquireSpinLock(&g_process_list_lock, &old_irql);
    for (PLIST_ENTRY entry = g_process_list.Flink;
         entry != &g_process_list; entry = entry->Flink) {
        PROCESS_OBJECT *object = CONTAINING_RECORD(entry, PROCESS_OBJECT,
                                                    GlobalEntry);
        if (object->Process.ProcessId == pid) {
            found = (POBJECT)object;
            ObReferenceObject(found);
            break;
        }
    }
    KeReleaseSpinLock(&g_process_list_lock, old_irql);
    if (!found) return (UINT64)STATUS_NOT_FOUND;
    NTSTATUS status = ObCreateHandle(found, (ACCESS_MASK)a[1], output);
    ObDereferenceObject(found);
    return (UINT64)status;
}

static NTSTATUS reference_duplicate_process(HANDLE handle, PKPROCESS *process,
                                             POBJECT *referenced)
{
    *referenced = NULL;
    if ((ULONG_PTR)handle == ~(ULONG_PTR)0) {
        *process = KeGetCurrentThread()->Process;
        return STATUS_SUCCESS;
    }
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0x0040, NULL,
                                                referenced);
    if (!NT_SUCCESS(status)) return status;
    *process = PsGetProcessFromObject(*referenced);
    if (!*process) {
        ObDereferenceObject(*referenced);
        *referenced = NULL;
        return STATUS_INVALID_HANDLE;
    }
    return STATUS_SUCCESS;
}

UINT64 NtDuplicateObject(UINT64 *a)
{
    PHANDLE output = (PHANDLE)a[3];
    ULONG options = (ULONG)a[6];
    if (!MmProbeForWrite((UINT64)output, sizeof(*output)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (a[5] || (options & ~3u))
        return (UINT64)STATUS_NOT_SUPPORTED;
    PKPROCESS source, target;
    POBJECT source_ref, target_ref;
    NTSTATUS status = reference_duplicate_process((HANDLE)(ULONG_PTR)a[0],
                                                   &source, &source_ref);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    status = reference_duplicate_process((HANDLE)(ULONG_PTR)a[2],
                                          &target, &target_ref);
    if (NT_SUCCESS(status)) {
        status = ObDuplicateHandleBetweenTables(source->HandleTable,
                                                 (HANDLE)(ULONG_PTR)a[1],
                                                 target->HandleTable,
                                                 (ACCESS_MASK)a[4],
                                                 (options & 2u) != 0,
                                                 (options & 1u) != 0,
                                                 output);
        if (target_ref) ObDereferenceObject(target_ref);
    }
    if (source_ref) ObDereferenceObject(source_ref);
    return (UINT64)status;
}

static NTSTATUS create_process_captured(const char *filename,
                                        const char *command,
                                        const char *current_directory,
                                        HANDLE *out, ULONG *pid_out,
                                        HANDLE std_input, HANDLE std_output,
                                        HANDLE std_error,
                                        const char *const *environment,
                                        UINT32 environment_count)
{
    if (!g_process_type)
        g_process_type = ObCreateObjectType("Process", delete_process);
    POBJECT raw;
    NTSTATUS status = ObCreateObject(g_process_type, sizeof(PROCESS_OBJECT),
                                     &raw);
    if (!NT_SUCCESS(status)) return status;
    PROCESS_OBJECT *object = (PROCESS_OBJECT *)raw;
    memset(object, 0, sizeof(*object));
    InitializeListHead(&object->GlobalEntry);
    object->Header.Type = ThreadObject;
    InitializeListHead(&object->Header.WaitListHead);
    object->Process.ProcessId = __sync_fetch_and_add(&g_next_process_id, 4);
    PKTHREAD creator = KeGetCurrentThread();
    object->Process.ParentProcessId =
        creator && creator->Process ? creator->Process->ProcessId : 0;
    object->Process.SecurityUserRid =
        creator && creator->Process ? creator->Process->SecurityUserRid : 1000;
    object->Process.PriorityClass =
        creator && creator->Process ? creator->Process->PriorityClass : 2;
    object->Process.BasePriority =
        creator && creator->Process ? creator->Process->BasePriority : 8;
    object->Process.ExitStatus = STATUS_PENDING;
    object->Process.ShutdownLevel = 0x280;
    object->Process.ErrorMode =
        creator && creator->Process ? creator->Process->ErrorMode : 0;
    if (creator && creator->Process)
        memcpy(object->Process.Environment, creator->Process->Environment,
               sizeof(object->Process.Environment));
    if (environment) {
        status = PsSetProcessEnvironment(&object->Process, environment,
                                         environment_count);
        if (!NT_SUCCESS(status)) {
            ObDereferenceObject(raw);
            return status;
        }
    }
    object->Process.Object = raw;
    InitializeListHead(&object->Process.JobEntry);
    object->Process.UserAllocationNext = 0x0000000020000000ULL;
    object->Process.ThreadAllocationNext = 0x0000000030000000ULL;
    const char *cwd = current_directory && current_directory[0]
                          ? current_directory :
                      creator && creator->Process &&
                      creator->Process->CurrentDirectory[0]
                          ? creator->Process->CurrentDirectory : "C:\\";
    UINT32 cwd_n = 0;
    while (cwd[cwd_n] && cwd_n + 1 < sizeof(object->Process.CurrentDirectory)) {
        object->Process.CurrentDirectory[cwd_n] = cwd[cwd_n];
        cwd_n++;
    }
    object->Process.CurrentDirectory[cwd_n] = 0;
    UINT32 image_n = 0;
    const char *image = filename;
    if (!(filename[0] && filename[1] == ':')) {
        const char *prefix = filename[0] == '\\' || filename[0] == '/'
                                 ? "C:" : object->Process.CurrentDirectory;
        while (prefix[image_n] && image_n + 1 <
               sizeof(object->Process.ImagePath)) {
            object->Process.ImagePath[image_n] = prefix[image_n];
            image_n++;
        }
        if (filename[0] != '\\' && filename[0] != '/' && image_n &&
            object->Process.ImagePath[image_n - 1] != '\\' &&
            image_n + 1 < sizeof(object->Process.ImagePath))
            object->Process.ImagePath[image_n++] = '\\';
    }
    while (*image && image_n + 1 < sizeof(object->Process.ImagePath))
        object->Process.ImagePath[image_n++] = *image++;
    object->Process.ImagePath[image_n] = 0;
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

    PKTHREAD thread = NULL;
    void *image_data = NULL;
    SIZE_T image_size = 0;
    status = FatLoadFile(object->Process.ImagePath, &image_data, &image_size,
                         NULL);
    if (NT_SUCCESS(status) && LinuxIsElfImage(image_data, image_size)) {
        status = ensure_elf_environment_defaults(&object->Process,
                                                  object->Process.ImagePath);
        if (NT_SUCCESS(status)) {
            if (process_image_is_feh(object->Process.ImagePath)) {
                KeLog("[ps]   feh environment: HOME=%u FEH_SKIP_MAGIC=%u IMLIB2_LOADER_PATH=%u\n",
                      process_environment_has(&object->Process, "HOME"),
                      process_environment_has(&object->Process, "FEH_SKIP_MAGIC"),
                      process_environment_has(&object->Process, "IMLIB2_LOADER_PATH"));
            }
            const char *linux_environment[64];
            char linux_path[512];
            UINT32 linux_environment_count = process_environment_vector(
                &object->Process, linux_environment, linux_path);
            UINT64 parent_space = MmGetCurrentAddressSpace();
            MmSwitchAddressSpace(object->Process.DirectoryTableBase);
            status = LinuxCreateInitialThread(&object->Process, object->Name,
                                              command, image_data, image_size,
                                              linux_environment,
                                              linux_environment_count,
                                              &thread);
            MmSwitchAddressSpace(parent_space);
        }
        ExFreePool(image_data);
    } else if (NT_SUCCESS(status) &&
               DosIsImage(filename, image_data, image_size)) {
        PVOID dos_context = NULL;
        status = DosCreateVm(image_data, image_size, command, &dos_context);
        ExFreePool(image_data);
        if (NT_SUCCESS(status)) {
            thread = KeCreateKernelThreadForProcess(&object->Process,
                       object->Name, DosRun, dos_context, &object->Header,
                       object->Process.BasePriority);
            if (!thread) {
                DosDestroyVm(dos_context);
                status = STATUS_NO_MEMORY;
            }
        }
    } else {
        if (image_data) ExFreePool(image_data);
        UINT64 parent_space = MmGetCurrentAddressSpace();
        PLDR_PROCESS_CONTEXT previous_loader = LdrSetThreadProcessContext(
            object->Process.LoaderContext);
        MmSwitchAddressSpace(object->Process.DirectoryTableBase);
        LdrResetProcessContext();
        UINT64 entry, base, argument;
        status = LdrLoadExecutable(filename, &entry, &base, &argument);
        if (NT_SUCCESS(status))
            status = build_environment(object, command, base, entry, argument,
                                       &thread);
        MmSwitchAddressSpace(parent_space);
        LdrSetThreadProcessContext(previous_loader);
    }
    if (NT_SUCCESS(status) && !thread->ProcessMainThread) {
        ObReferenceObject(raw); /* held until the main thread signals exit */
        thread->TerminationObject = &object->Header;
        thread->ProcessMainThread = TRUE;
    }
    if (!NT_SUCCESS(status)) {
        if (handle)
            ObCloseHandle(handle);
        ObDereferenceObject(raw);
        return status;
    }
    if (out) *out = handle;
    if (pid_out) *pid_out = object->Process.ProcessId;
    KIRQL old_irql;
    KeAcquireSpinLock(&g_process_list_lock, &old_irql);
    InsertTailList(&g_process_list, &object->GlobalEntry);
    KeReleaseSpinLock(&g_process_list_lock, old_irql);
    ObDereferenceObject(raw); /* handle + running thread retain it */
    KeLog("[ps]   process '%s' pid %u -> handle %p, CR3 0x%lx\n",
          filename, object->Process.ProcessId, handle,
          (unsigned long)object->Process.DirectoryTableBase);
    return STATUS_SUCCESS;
}

NTSTATUS PsForkLinuxProcess(PKTHREAD parent, HANDLE *handle_out,
                            ULONG *pid_out)
{
    if (!parent || !parent->Process || !parent->Process->LinuxContext ||
        !handle_out || !pid_out)
        return STATUS_INVALID_PARAMETER;
    PKPROCESS source = parent->Process;
    if (!g_process_type)
        g_process_type = ObCreateObjectType("Process", delete_process);
    POBJECT raw = NULL;
    NTSTATUS status = ObCreateObject(g_process_type, sizeof(PROCESS_OBJECT),
                                     &raw);
    if (!NT_SUCCESS(status)) return status;
    PROCESS_OBJECT *object = (PROCESS_OBJECT *)raw;
    memset(object, 0, sizeof(*object));
    InitializeListHead(&object->GlobalEntry);
    object->Header.Type = ThreadObject;
    InitializeListHead(&object->Header.WaitListHead);
    PKPROCESS target = &object->Process;
    target->ProcessId = __sync_fetch_and_add(&g_next_process_id, 4);
    target->ParentProcessId = source->ProcessId;
    target->SecurityUserRid = source->SecurityUserRid;
    target->PriorityClass = source->PriorityClass;
    target->BasePriority = source->BasePriority;
    target->ExitStatus = STATUS_PENDING;
    target->Object = raw;
    target->UserAllocationNext = source->UserAllocationNext;
    target->ThreadAllocationNext = source->ThreadAllocationNext;
    memcpy(target->CurrentDirectory, source->CurrentDirectory,
           sizeof(target->CurrentDirectory));
    memcpy(target->ImagePath, source->ImagePath, sizeof(target->ImagePath));
    memcpy(target->Environment, source->Environment,
           sizeof(target->Environment));
    SIZE_T name_length = strlen(source->Name);
    if (name_length >= sizeof(object->Name))
        name_length = sizeof(object->Name) - 1;
    memcpy(object->Name, source->Name, name_length);
    object->Name[name_length] = 0;
    target->Name = object->Name;
    InitializeListHead(&target->ThreadListHead);
    InitializeListHead(&target->JobEntry);

    target->HandleTable = ObCreateHandleTable();
    if (!target->HandleTable) { status = STATUS_NO_MEMORY; goto failed; }
    status = LinuxCloneProcessContext(source, target);
    if (!NT_SUCCESS(status)) {
        KeLog("[ps]   Linux fork descriptor clone failed: 0x%08x\n",
              (unsigned)status);
        goto failed;
    }
    target->DirectoryTableBase = MmCloneUserAddressSpace(
        source->DirectoryTableBase, LinuxSharedPage, source);
    if (target->DirectoryTableBase == MM_INVALID_PHYS) {
        target->DirectoryTableBase = 0;
        status = STATUS_NO_MEMORY;
        goto failed;
    }

    HANDLE handle = NULL;
    status = ObCreateHandle(raw, GENERIC_ALL, &handle);
    if (!NT_SUCCESS(status)) goto failed;
    PKTHREAD child = NULL;
    status = LinuxCreateForkThread(parent, target, &object->Header, &child);
    if (!NT_SUCCESS(status)) {
        ObCloseHandle(handle);
        goto failed;
    }
    KIRQL old_irql;
    KeAcquireSpinLock(&g_process_list_lock, &old_irql);
    InsertTailList(&g_process_list, &object->GlobalEntry);
    KeReleaseSpinLock(&g_process_list_lock, old_irql);
    *handle_out = handle;
    *pid_out = target->ProcessId;
    ObDereferenceObject(raw);
    return STATUS_SUCCESS;

failed:
    ObDereferenceObject(raw);
    return status;
}

/* Linux execve of a PE image: replace the calling thread's Linux execution
 * state with a full Windows environment in the address space the caller
 * already switched to (CR3 must be the new image's). The process keeps its
 * pid and handle table; the caller has already published the converted
 * standard handles in process->StandardInput/Output/Error and still owns
 * the Linux personality teardown. On return the thread re-enters ring 3
 * through the syscall exit stub's Windows-exec path. */
NTSTATUS PsSwitchThreadToWindows(PKTHREAD thread, PKPROCESS process,
                                 const char *image_path, const char *command)
{
    if (!thread || !process || !image_path || !command ||
        !process->LoaderContext)
        return STATUS_INVALID_PARAMETER;
    PE_BOOT_INFO boot;
    memset(&boot, 0, sizeof(boot));
    /* The process's loader context was created with the process and never
     * used by the Linux personality; it now owns this PE image's modules.
     * A failure restores the thread's previous loader context so the Linux
     * image keeps running untouched. */
    PLDR_PROCESS_CONTEXT previous_loader = LdrSetThreadProcessContext(
        process->LoaderContext);
    LdrResetProcessContext();
    UINT64 base = 0;
    NTSTATUS status = LdrLoadExecutable(image_path, &boot.Entry, &base,
                                        &boot.Argument);
    if (!NT_SUCCESS(status)) {
        LdrSetThreadProcessContext(previous_loader);
        return status;
    }
    status = build_pe_user_environment(process, process->Name, command,
                                       base, &boot);
    if (!NT_SUCCESS(status)) {
        LdrSetThreadProcessContext(previous_loader);
        return status;
    }

    PTEB teb = (PTEB)PROCESS_MAIN_TEB_VA;
    teb->ClientId.UniqueThread = (HANDLE)(ULONG_PTR)thread->ThreadId;
    thread->UserEntry = boot.Entry;
    thread->UserStack = boot.StackTop;
    thread->UserArg = boot.Argument;
    thread->UserGsBase = PROCESS_MAIN_TEB_VA;
    thread->UserFsBase = 0;
    thread->UserRip = boot.Entry;
    thread->UserRsp = boot.StackTop;
    KeSetUserFsBase(0);
    KeSetUserGsBase(PROCESS_MAIN_TEB_VA);
    if (thread->LinuxThread)
        LinuxThreadBeginWindowsExec(thread, boot.Argument);
    return STATUS_SUCCESS;
}

NTSTATUS PsCreateInitialProcess(const char *application,
                                const char *command_line)
{
    return create_process_captured(application, command_line, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, 0);
}

NTSTATUS PsCreateLinuxSpawn(PKPROCESS creator, const char *image,
                            const char *command_line,
                            const char *current_directory,
                            HANDLE standard_input, HANDLE standard_output,
                            HANDLE standard_error,
                            const char *const *environment,
                            UINT32 environment_count,
                            HANDLE *process_handle, ULONG *process_id)
{
    if (!creator || !creator->LinuxContext || !image || !command_line ||
        !process_handle || !process_id)
        return STATUS_INVALID_PARAMETER;
    return create_process_captured(image, command_line, current_directory,
                                   process_handle, process_id,
                                   standard_input, standard_output,
                                   standard_error, environment,
                                   environment_count);
}

UINT64 NtCreateUserProcessSimple(UINT64 *a)
{
    char filename[260], command[PE_COMMAND_MAX], current_directory[260];
    char environment_block[2048];
    const char *environment[64];
    UINT32 environment_count = 0;
    PHANDLE out = (PHANDLE)a[2];
    ULONG *pid_out = (ULONG *)a[3];
    KeLog("[ps]   NtCreateUserProcessSimple request image=%p command=%p\n",
          (void *)a[0], (void *)a[1]);
    if (!MmProbeForWrite((UINT64)out, sizeof(*out)) ||
        (pid_out && !MmProbeForWrite((UINT64)pid_out, sizeof(*pid_out)))) {
        KeLog("[ps]   process request rejected: invalid user buffer\n");
        return (UINT64)STATUS_ACCESS_VIOLATION;
    }
    NTSTATUS capture_status = capture_ascii(a[0], filename, sizeof(filename));
    if (!NT_SUCCESS(capture_status)) return (UINT64)capture_status;
    capture_status = capture_ascii(a[1], command, sizeof(command));
    if (!NT_SUCCESS(capture_status)) return (UINT64)capture_status;
    if (a[7]) {
        capture_status = capture_ascii(a[7], current_directory,
                                       sizeof(current_directory));
        if (!NT_SUCCESS(capture_status)) return (UINT64)capture_status;
    }
    if (a[8]) {
        SIZE_T used = 0;
        for (;;) {
            if (environment_count == 64 || used + 2 > sizeof(environment_block))
                return (UINT64)STATUS_BUFFER_TOO_SMALL;
            environment[environment_count] = environment_block + used;
            for (;;) {
                if (used >= sizeof(environment_block) ||
                    !MmProbeForRead(a[8] + used, 1))
                    return (UINT64)STATUS_ACCESS_VIOLATION;
                char ch = *(const char *)(ULONG_PTR)(a[8] + used);
                environment_block[used++] = ch;
                if (!ch) break;
            }
            if (used == 1 && !environment_block[0]) break;
            environment_count++;
            if (!MmProbeForRead(a[8] + used, 1))
                return (UINT64)STATUS_ACCESS_VIOLATION;
            if (*(const char *)(ULONG_PTR)(a[8] + used) == 0) break;
        }
    }
    return (UINT64)create_process_captured(filename, command,
                                           a[7] ? current_directory : NULL,
                                           out, pid_out,
                                           (HANDLE)a[4], (HANDLE)a[5],
                                           (HANDLE)a[6],
                                           a[8] ? environment : NULL,
                                           environment_count);
}
