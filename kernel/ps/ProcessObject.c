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
#include <nt/Pe.h>
#include <svindows/Session.h>
#include <svindows/Console.h>
#include <abi/ProcessCreate.h>

typedef struct _PS_CAPTURED_PROCESS_OPTIONS {
    ULONG Flags;
    BOOLEAN InheritHandles;
    PKPROCESS ParentOverride;
    const UINT16 *Desktop;
    const char *EnvironmentBlock;
    SIZE_T EnvironmentBytes;
    SE_DESCRIPTOR *ProcessDescriptor, *ThreadDescriptor;
    BOOLEAN InheritProcessHandle, InheritThreadHandle;
    ULONG StartupFlags, ShowWindow, X, Y, XSize, YSize, XChars, YChars, FillAttribute;
} PS_CAPTURED_PROCESS_OPTIONS;
static PS_CONSOLE_PROCESS_INITIALIZER g_process_console_initialize;
static void (*g_process_console_published)(PKPROCESS);
void PsRegisterConsoleProcessInitializer(PS_CONSOLE_PROCESS_INITIALIZER initialize)
{ g_process_console_initialize = initialize; }
void PsRegisterConsoleProcessPublished(void (*published)(PKPROCESS))
{ g_process_console_published = published; }

/* Console teardown is provided by the hosted-console subsystem. */
void ConDetachProcessConsole(PKPROCESS process);
NTSTATUS ConWaitForProcessConsoleHost(PKPROCESS process);

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

/* The two scheduler-owned records have no user-mode process object to pin.
 * PID 0 is described separately by the active processor's idle context. */
static PKPROCESS system_snapshot_process(ULONG index)
{
    if (!index) return NULL;
    if (index == 1) return KeGetSystemProcess();
    return PsGetProcessFromObject(g_system_snapshot_processes[index]);
}

static const char *system_snapshot_name(PKPROCESS process)
{
    if (!process) return ""; /* NT idle record has an empty image name */
    return process->ImagePath[0] ? process->ImagePath : process->Name;
}

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
    W32kCleanupProcessInputIdle(&object->Process);
    ConDetachProcessConsole(&object->Process);
    W32kCleanupProcessSession(&object->Process);
    PsCleanupProcessEnvironment(&object->Process);
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

/* Keep a minimal POSIX user environment for ELF children even when the PE
 * parent was created without a complete Windows environment block. */
static BOOLEAN process_environment_has(PKPROCESS process, const char *name)
{
    SIZE_T name_length = strlen(name);
    SIZE_T bytes, used = 0;
    const char *data = PsProcessEnvironmentData(process, &bytes);
    while (used < bytes && data[used]) {
        const char *entry = data + used;
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
    SIZE_T bytes, used = 0;
    const char *data = PsProcessEnvironmentData(process, &bytes);
    while (used < bytes && data[used]) {
        if (count == 64) return STATUS_BUFFER_TOO_SMALL;
        entries[count++] = data + used;
        used += strlen(data + used) + 1;
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
    SIZE_T bytes, used = 0;
    UINT32 count = 0;
    const char *data = PsProcessEnvironmentData(process, &bytes);
    while (used < bytes && data[used] && count < 64) {
        const char *entry = data + used;
        entries[count++] = entry;
        if (linux_convert_search_path(entry, linux_path))
            entries[count - 1] = linux_path;
        used += strlen(entry) + 1;
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

/* Unused portions of the ABI-sized USER/GDI handle tables all read as zero.
 * Alias one global read-only frame there instead of allocating hundreds of
 * private zero frames for every process. */
static UINT64 g_process_zero_page = MM_INVALID_PHYS;

static UINT64 process_zero_page(void)
{
    UINT64 page = __atomic_load_n(&g_process_zero_page, __ATOMIC_ACQUIRE);
    if (page != MM_INVALID_PHYS) return page;
    UINT64 fresh = MmAllocatePage();
    if (fresh == MM_INVALID_PHYS) return fresh;
    memset(MmPhysToVirt(fresh), 0, PAGE_SIZE);
    UINT64 expected = MM_INVALID_PHYS;
    if (!__atomic_compare_exchange_n(&g_process_zero_page, &expected, fresh,
                                     FALSE, __ATOMIC_RELEASE,
                                     __ATOMIC_ACQUIRE)) {
        MmFreePages(fresh, 1);
        fresh = expected;
    }
    return fresh;
}

static BOOLEAN map_rw_range(UINT64 base, UINT64 size)
{
    if (!size || (size & PAGE_MASK)) return FALSE;
    SIZE_T pages = size / PAGE_SIZE;
    UINT64 contiguous = MmAllocatePages(pages);
    if (contiguous != MM_INVALID_PHYS) {
        if (MmMapContiguousPages(base, contiguous, pages,
                                 PTE_USER | PTE_WRITE)) {
            memset((void *)base, 0, size);
            if (pages >= 64) KeYield();
            return TRUE;
        }
        MmFreePages(contiguous, pages);
    }
    for (UINT64 off = 0; off < size; off += PAGE_SIZE) {
        if (!map_rw(base + off)) return FALSE;
    }
    if (pages >= 64) KeYield();
    return TRUE;
}

static BOOLEAN map_sparse_table(UINT64 base, UINT64 size, UINT64 committed)
{
    UINT64 zero = process_zero_page();
    if (zero == MM_INVALID_PHYS) return FALSE;
    if (!map_rw_range(base, committed)) return FALSE;
    SIZE_T aliases = (size - committed) / PAGE_SIZE;
    if (aliases && !MmMapAliasedPages(base + committed, zero, aliases,
                                      PTE_USER | PTE_SHARED))
        return FALSE;
    KeYield();
    return TRUE;
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
    if (!map_sparse_table(PROCESS_GDI_SHARED_TABLE_VA,
                          PROCESS_GDI_SHARED_TABLE_SIZE,
                          PROCESS_GDI_SHARED_TABLE_COMMIT_SIZE))
        return STATUS_NO_MEMORY;
    peb->GdiSharedHandleTable = (PVOID)PROCESS_GDI_SHARED_TABLE_VA;
    if (!map_rw_range(PROCESS_GDI_ATTR_VA, PROCESS_GDI_ATTR_SIZE))
        return STATUS_NO_MEMORY;
    W32kInitializeGdi();

    if (!map_sparse_table(PROCESS_USER_SHARED_TABLE_VA,
                          PROCESS_USER_SHARED_TABLE_SIZE,
                          PROCESS_USER_SHARED_TABLE_COMMIT_SIZE))
        return STATUS_NO_MEMORY;
    /* win32k fills HANDLEENTRY cells transiently; user mode only observes
     * them, matching USER32's read-only shared table contract. */
    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA,
                   PROCESS_USER_SHARED_TABLE_COMMIT_SIZE, FALSE);
    if (!map_rw_range(PROCESS_USER_OBJECT_ARENA_VA,
                      PROCESS_USER_OBJECT_ARENA_SIZE))
        return STATUS_NO_MEMORY;

    if (!map_rw_range(PROCESS_LDR_VA, PROCESS_LDR_SIZE))
        return STATUS_NO_MEMORY;
    LdrBuildProcessModuleList(peb, PROCESS_LDR_VA, PROCESS_LDR_SIZE);

    UINT8 *params = map_rw(PROCESS_PARAMS_VA);
    if (!params) return STATUS_NO_MEMORY;
    SIZE_T environment_bytes, environment_chars;
    const char *environment_data = PsProcessEnvironmentData(process, &environment_bytes);
    NTSTATUS environment_status = PsEnvironmentToWide(environment_data, environment_bytes,
                                                      NULL, 0, &environment_chars);
    if (!NT_SUCCESS(environment_status)) return environment_status;
    UINT64 environment_address = PROCESS_ENV_VA;
    SIZE_T environment_pages = (environment_chars * 2 + PAGE_SIZE - 1) / PAGE_SIZE;
    if (environment_pages > 1) {
        environment_address = PAGE_ALIGN_UP(process->UserAllocationNext);
        if (environment_address > MM_USER_MAX - environment_pages * PAGE_SIZE)
            return STATUS_NO_MEMORY;
        process->UserAllocationNext = environment_address + environment_pages * PAGE_SIZE;
    }
    for (SIZE_T page = 0; page < environment_pages; page++)
        if (!map_rw(environment_address + page * PAGE_SIZE)) return STATUS_NO_MEMORY;
    UINT16 *envw = (UINT16 *)environment_address;
    environment_status = PsEnvironmentToWide(environment_data, environment_bytes,
                                              envw, environment_chars, &environment_chars);
    if (!NT_SUCCESS(environment_status)) return environment_status;
    *(void **)(params + 0x80) = envw;
    *(ULONG *)(params + 0x88) = process->StartupX;
    *(ULONG *)(params + 0x8C) = process->StartupY;
    *(ULONG *)(params + 0x90) = process->StartupXSize;
    *(ULONG *)(params + 0x94) = process->StartupYSize;
    *(ULONG *)(params + 0x98) = process->StartupXChars;
    *(ULONG *)(params + 0x9C) = process->StartupYChars;
    *(ULONG *)(params + 0xA0) = process->StartupFillAttribute;
    *(ULONG *)(params + 0xA4) = process->StartupFlags;
    *(ULONG *)(params + 0xA8) = process->StartupShowWindow;
    ULONG desktop_chars = 0;
    NTSTATUS desktop_status = W32kQueryProcessDesktopName(process, NULL, 0, &desktop_chars);
    if (desktop_status != STATUS_BUFFER_TOO_SMALL || !desktop_chars) return desktop_status;
    UINT64 desktop_address = (UINT64)(params + 0xC20);
    if (desktop_chars > (PAGE_SIZE - 0xC20) / 2) {
        desktop_address = PAGE_ALIGN_UP(process->UserAllocationNext);
        SIZE_T pages = ((SIZE_T)desktop_chars * 2 + PAGE_SIZE - 1) / PAGE_SIZE;
        process->UserAllocationNext = desktop_address + pages * PAGE_SIZE;
        for (SIZE_T page = 0; page < pages; page++)
            if (!map_rw(desktop_address + page * PAGE_SIZE)) return STATUS_NO_MEMORY;
    }
    desktop_status = W32kQueryProcessDesktopName(process, (UINT16 *)desktop_address, desktop_chars, &desktop_chars);
    if (!NT_SUCCESS(desktop_status)) return desktop_status;
    *(UINT16 *)(params + 0xC0) = (UINT16)((desktop_chars - 1) * 2);
    *(UINT16 *)(params + 0xC2) = (UINT16)(desktop_chars * 2);
    *(UINT64 *)(params + 0xC8) = desktop_address;
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

    if (!map_rw_range(PROCESS_MAIN_TEB_VA, PROCESS_TEB_SIZE))
        return STATUS_NO_MEMORY;
    PTEB teb = (PTEB)PROCESS_MAIN_TEB_VA;
    teb->NtTib.Self = (struct _NT_TIB *)PROCESS_MAIN_TEB_VA;
    teb->ProcessEnvironmentBlock = (PVOID)PROCESS_PEB_VA;
    /* Avoid USER32's obsolete Windows-3.x fast path when a genuine GUI DLL is
     * loaded later.  This is CLIENTINFO.dwExpWinVer at TEB+0x810. */
    *(UINT32 *)((UINT8 *)teb + 0x810) = 0x0A00;
    teb->ClientId.UniqueProcess =
        (HANDLE)(ULONG_PTR)process->ProcessId;

    UINT64 stack_base = CHILD_STACK_TOP - CHILD_STACK_PAGES * PAGE_SIZE;
    if (!map_rw_range(stack_base, CHILD_STACK_PAGES * PAGE_SIZE))
        return STATUS_NO_MEMORY;
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
    /* Never publish a thread whose initial frame cannot be constructed in its
     * own page tables.  Besides producing a useful process-creation failure,
     * this catches any future address-space attachment regression before a
     * supervisor write in KiEnterUserMode turns it into a kernel bugcheck. */
    if (MmGetPhysicalAddressInAddressSpace(
            object->Process.DirectoryTableBase, boot.StackTop - 0x28) ==
            MM_INVALID_PHYS ||
        MmGetPhysicalAddressInAddressSpace(
            object->Process.DirectoryTableBase, PROCESS_MAIN_TEB_VA) ==
            MM_INVALID_PHYS ||
        MmGetPhysicalAddressInAddressSpace(
            object->Process.DirectoryTableBase, boot.Entry) ==
            MM_INVALID_PHYS) {
        KeLog("[ps]   refusing process '%s': incomplete initial user mapping\n",
              object->Name);
        return STATUS_ACCESS_VIOLATION;
    }
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

    PKPRCB cpu = KeGetProcessorBlock(0);
    ULONG process_count = 2, cursor = 0;
    g_system_snapshot_thread_counts[0] = 1;
    g_system_snapshot_threads[0][0] = (KE_PROCESS_THREAD_INFO){
        .KernelTime = __atomic_load_n(&cpu->IdleTime, __ATOMIC_RELAXED),
        .State = cpu->CurrentThread == cpu->IdleThread ? 2 : 1
    };
    ULONG system_threads = KeSnapshotProcessThreads(KeGetSystemProcess(),
                                                    g_system_snapshot_threads[1], 128);
    g_system_snapshot_thread_counts[1] = system_threads < 128 ? system_threads : 128;
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
        PKPROCESS process = system_snapshot_process(i);
        const char *name = system_snapshot_name(process);
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
        PKPROCESS process = system_snapshot_process(i);
        UINT8 *row = bytes + offset;
        ULONG thread_count = g_system_snapshot_thread_counts[i];
        const char *image = system_snapshot_name(process);
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
        *(UINT64 *)(row + 40) = process ? __atomic_load_n(&process->UserTime,__ATOMIC_RELAXED) : 0;
        *(UINT64 *)(row + 48) = process ? __atomic_load_n(&process->KernelTime,__ATOMIC_RELAXED)
                                      : g_system_snapshot_threads[0][0].KernelTime;
        *(UINT16 *)(row + 56) = (UINT16)(visible_chars * sizeof(UINT16));
        *(UINT16 *)(row + 58) = (UINT16)((visible_chars + 1) * sizeof(UINT16));
        if (visible_chars) *(UINT64 *)(row + 64) = (UINT64)(row + 256 + thread_count * 80);
        *(LONG *)(row + 72) = process ? process->BasePriority : 0;
        *(UINT64 *)(row + 80) = process ? process->ProcessId : 0;
        *(UINT64 *)(row + 88) = process ? process->ParentProcessId : 0;
        *(ULONG *)(row + 96) = process ? ObCountOpenHandles(process->HandleTable) : 0;
        *(ULONG *)(row + 100) = 0; /* this build has one interactive session */
        UINT64 virtual_bytes = 0, committed_bytes = 0;
        for (ULONG r = 0; process && r < MM_MAX_PROCESS_REGIONS; ++r) {
            MM_PROCESS_REGION *region = &process->VmRegions[r];
            if (!region->InUse) continue;
            virtual_bytes += region->ReserveSize;
            committed_bytes += region->CommittedPages * PAGE_SIZE;
        }
        *(UINT64 *)(row + 112) = virtual_bytes;
        *(UINT64 *)(row + 120) = virtual_bytes;
        UINT64 flags = KiIrqSave();
        UINT64 private_pages;
        UINT64 working_set = MmCountResidentUserPages(i >= 2 ? process->DirectoryTableBase : 0,
                                                      &private_pages) * PAGE_SIZE;
        if (process && working_set > process->PeakWorkingSetBytes)
            process->PeakWorkingSetBytes = working_set;
        UINT64 sampled_peak = process ? process->PeakWorkingSetBytes : 0;
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
            *(UINT64 *)(entry + 40) = process ? process->ProcessId : 0;
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
    for (ULONG i = 2; i < process_count; ++i)
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
    BOOLEAN inherit = FALSE;
    if (a[2]) {
        OBJECT_ATTRIBUTES attributes;
        if (!MmProbeForRead(a[2], sizeof(attributes))) return (UINT64)STATUS_ACCESS_VIOLATION;
        memcpy(&attributes, (void *)a[2], sizeof(attributes));
        if (attributes.Length != sizeof(attributes) || attributes.ObjectName || attributes.RootDirectory ||
            (attributes.Attributes & 0x200u)) return (UINT64)STATUS_INVALID_PARAMETER;
        inherit = (attributes.Attributes & 2u) != 0;
    }
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
    PKTHREAD caller = KeGetCurrentThread();
    SE_DESCRIPTOR *descriptor = SeReferenceObjectDescriptor(found);
    ACCESS_MASK granted = 0;
    NTSTATUS status = SeAccessCheck(descriptor, caller->Process->SecurityUserRid, (ACCESS_MASK)a[1],
                                    &ObHeaderFromObject(found)->Type->GenericMapping, &granted);
    SeDereferenceDescriptor(descriptor);
    if (NT_SUCCESS(status)) status = ObCreateHandle(found, granted, output);
    if (NT_SUCCESS(status) && inherit)
        ObSetHandleAttributesInTable(caller->Process->HandleTable, *output, OB_HANDLE_INHERIT, OB_HANDLE_INHERIT);
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

static NTSTATUS PspCopyStandardHandle(PKPROCESS parent, PKPROCESS process,
                                     HANDLE source, HANDLE *target)
{
    *target = NULL;
    if (!source) return STATUS_SUCCESS;
    if (!parent || !parent->HandleTable) return STATUS_INVALID_HANDLE;
    POBJECT object, existing = NULL;
    NTSTATUS status = ObReferenceObjectByHandleInTable(parent->HandleTable,
                                      source, 0, NULL, &object, NULL);
    if (!NT_SUCCESS(status)) return status;
    status = ObReferenceObjectByHandleInTable(process->HandleTable,
                                      source, 0, NULL, &existing, NULL);
    if (NT_SUCCESS(status)) {
        if (object == existing) *target = source;
        ObDereferenceObject(existing);
    }
    if (!*target) status = ObDuplicateHandleBetweenTables(parent->HandleTable,
                        source, process->HandleTable, 0, TRUE, FALSE, target);
    else status = STATUS_SUCCESS;
    ObDereferenceObject(object); return status;
}

static void PspCloseStandardHandle(PKPROCESS process, HANDLE handle)
{
    POBJECT object;
    if (handle && NT_SUCCESS(ObReferenceObjectByHandleInTable(process->HandleTable,
                                        handle, 0, NULL, &object, NULL))) {
        ObCloseHandleForObject(process->HandleTable, handle, object);
        ObDereferenceObject(object);
    }
}

static NTSTATUS create_process_captured_extended(const char *filename,
                                        const char *command,
                                        const char *current_directory,
                                        HANDLE *out, ULONG *pid_out,
                                        HANDLE std_input, HANDLE std_output,
                                        HANDLE std_error,
                                        const char *const *environment,
                                        UINT32 environment_count,
                                        const PS_CAPTURED_PROCESS_OPTIONS *options,
                                        HANDLE *thread_out, ULONG *thread_id_out)
{
    if (!g_process_type)
        g_process_type = ObCreateObjectType("Process", delete_process);
    SeConfigureType(g_process_type, 0x21410u, 0x20BEBu, 0x120000u, 0x1FFFFFu);
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
    PKPROCESS parent = options && options->ParentOverride ? options->ParentOverride :
                       creator ? creator->Process : NULL;
    object->Process.ParentProcessId =
        parent ? parent->ProcessId : 0;
    object->Process.SecurityUserRid =
        parent ? parent->SecurityUserRid : 1000;
    if (options && options->ProcessDescriptor) SeAssignObjectDescriptor(raw, options->ProcessDescriptor);
    else if (SeDescriptorOwnerRid(ObHeaderFromObject(raw)->SecurityDescriptor) != object->Process.SecurityUserRid) {
        SE_DESCRIPTOR *descriptor = NULL;
        status = SeCreateDefaultDescriptor(object->Process.SecurityUserRid, &descriptor);
        if (!NT_SUCCESS(status)) { ObDereferenceObject(raw); return status; }
        SeAssignObjectDescriptor(raw, descriptor); SeDereferenceDescriptor(descriptor);
    }
    object->Process.PriorityClass =
        parent ? parent->PriorityClass : 2;
    object->Process.BasePriority =
        parent ? parent->BasePriority : 8;
    object->Process.ExitStatus = STATUS_PENDING;
    object->Process.ShutdownLevel = 0x280;
    object->Process.ErrorMode =
        parent ? parent->ErrorMode : 0;
    if (options) {
        object->Process.CreationFlags = options->Flags;
        object->Process.StartupFlags = options->StartupFlags;
        object->Process.StartupShowWindow = options->ShowWindow;
        object->Process.StartupX = options->X; object->Process.StartupY = options->Y;
        object->Process.StartupXSize = options->XSize; object->Process.StartupYSize = options->YSize;
        object->Process.StartupXChars = options->XChars; object->Process.StartupYChars = options->YChars;
        object->Process.StartupFillAttribute = options->FillAttribute;
        ULONG priority = options->Flags & 0xC1E0u;
        if (priority) {
            object->Process.PriorityClass = priority == 0x40 ? 1 : priority == 0x80 ? 3 :
                priority == 0x100 ? 4 : priority == 0x4000 ? 5 : priority == 0x8000 ? 6 : 2;
            object->Process.BasePriority = priority == 0x40 ? 4 : priority == 0x80 ? 13 :
                priority == 0x100 ? 24 : priority == 0x4000 ? 6 : priority == 0x8000 ? 10 : 8;
        }
        if (options->Flags & 0x04000000u) object->Process.ErrorMode = 0;
    } else if (std_input || std_output || std_error) object->Process.StartupFlags = 0x100u;
    if (parent) PsCloneProcessEnvironment(&object->Process, parent);
    if (options && options->EnvironmentBlock) {
        status = PsSetProcessEnvironmentBlock(&object->Process,
            options->EnvironmentBlock, options->EnvironmentBytes);
        if (!NT_SUCCESS(status)) { ObDereferenceObject(raw); return status; }
    } else if (environment) {
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
                      parent && parent->CurrentDirectory[0]
                          ? parent->CurrentDirectory : "C:\\";
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
    if (options && options->InheritHandles && parent) {
        status = W32kInheritSessionHandles(&object->Process, parent);
        if (!NT_SUCCESS(status)) { ObDereferenceObject(raw); return status; }
    }
    status = W32kInitializeProcessSession(&object->Process, parent,
                    options && options->ParentOverride ? NULL : creator);
    if (NT_SUCCESS(status)) status = W32kAssignProcessStartupDesktop(&object->Process,
                                                   options ? options->Desktop : NULL);
    if (!NT_SUCCESS(status)) { ObDereferenceObject(raw); return status; }
    object->Process.LoaderContext = LdrCreateProcessContext();
    if (!object->Process.LoaderContext) {
        ObDestroyHandleTable(object->Process.HandleTable);
        object->Process.HandleTable = NULL;
        ObDereferenceObject(raw);
        return STATUS_NO_MEMORY;
    }
    KeTrace("[ps]   process object %p owns loader context %p\n", raw,
          object->Process.LoaderContext);
    void *image_data = NULL;
    SIZE_T image_size = 0;
    status = FatLoadFile(object->Process.ImagePath, &image_data, &image_size, NULL);
    if (!NT_SUCCESS(status)) { ObDereferenceObject(raw); return status; }
    if (image_size >= sizeof(IMAGE_DOS_HEADER)) {
        const IMAGE_DOS_HEADER *dos = image_data;
        SIZE_T offset = dos->e_lfanew;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && offset <= image_size &&
            sizeof(IMAGE_NT_HEADERS64) <= image_size - offset) {
            const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((UINT8 *)image_data + offset);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                object->Process.ImageSubsystem = nt->OptionalHeader.Subsystem;
        }
    }
    if (g_process_console_initialize) {
        status = g_process_console_initialize(&object->Process, parent,
            object->Process.CreationFlags, object->Process.ImageSubsystem,
            std_input, std_output, std_error);
        if (!NT_SUCCESS(status)) { ExFreePool(image_data); ObDereferenceObject(raw); return status; }
    }
    BOOLEAN explicit_standard = (object->Process.StartupFlags & 0x100u) != 0;
    BOOLEAN inherit_console_standard = parent &&
        ((parent->NativeConsole && object->Process.NativeConsole) ||
         (parent->LegacyConsole && object->Process.LegacyConsole)) &&
        !object->Process.StandardInput && !object->Process.StandardOutput &&
        !object->Process.StandardError;
    if (explicit_standard || inherit_console_standard) {
        HANDLE source[3] = {explicit_standard ? std_input : parent->StandardInput,
            explicit_standard ? std_output : parent->StandardOutput,
            explicit_standard ? std_error : parent->StandardError};
        HANDLE old[3] = {object->Process.StandardInput, object->Process.StandardOutput,
                         object->Process.StandardError};
        HANDLE copied[3] = {0};
        for (ULONG i = 0; i < 3; i++) {
            status = PspCopyStandardHandle(parent, &object->Process, source[i], &copied[i]);
            if (!NT_SUCCESS(status)) { ExFreePool(image_data); ObDereferenceObject(raw); return status; }
        }
        object->Process.StandardInput = copied[0]; object->Process.StandardOutput = copied[1];
        object->Process.StandardError = copied[2];
        for (ULONG i = 0; i < 3; i++) {
            BOOLEAN keep = !old[i];
            for (ULONG j = 0; j < 3; j++) if (old[i] == copied[j]) keep = TRUE;
            for (ULONG j = 0; j < i; j++) if (old[i] == old[j]) keep = TRUE;
            if (!keep) PspCloseStandardHandle(&object->Process, old[i]);
        }
    }
    object->Process.DirectoryTableBase = MmCreateAddressSpace();
    if (object->Process.DirectoryTableBase == MM_INVALID_PHYS) {
        LdrDestroyProcessContext(object->Process.LoaderContext);
        object->Process.LoaderContext = NULL;
        ObDestroyHandleTable(object->Process.HandleTable);
        object->Process.HandleTable = NULL;
        ObDereferenceObject(raw);
        ExFreePool(image_data);
        return STATUS_NO_MEMORY;
    }

    HANDLE handle = NULL;
    if (out) {
        status = ObCreateHandle(raw, GENERIC_ALL, &handle);
        if (!NT_SUCCESS(status)) {
            ObDereferenceObject(raw);
            ExFreePool(image_data);
            return status;
        }
    }

    PVOID main_thread_object = NULL;
    HANDLE main_thread_handle = NULL;
    status = PsCreateThreadObject(&main_thread_object, thread_out ? &main_thread_handle : NULL);
    if (!NT_SUCCESS(status)) {
        if (handle) ObCloseHandle(handle);
        ExFreePool(image_data); ObDereferenceObject(raw); return status;
    }
    if (options && options->ThreadDescriptor) SeAssignObjectDescriptor(main_thread_object, options->ThreadDescriptor);
    else if (SeDescriptorOwnerRid(ObHeaderFromObject(main_thread_object)->SecurityDescriptor) != object->Process.SecurityUserRid) {
        SE_DESCRIPTOR *descriptor = NULL;
        status = SeCreateDefaultDescriptor(object->Process.SecurityUserRid, &descriptor);
        if (!NT_SUCCESS(status)) {
            if (main_thread_handle) ObCloseHandle(main_thread_handle);
            ObDereferenceObject(main_thread_object);
            if (handle) ObCloseHandle(handle);
            ExFreePool(image_data); ObDereferenceObject(raw); return status;
        }
        SeAssignObjectDescriptor(main_thread_object, descriptor); SeDereferenceDescriptor(descriptor);
    }
    if (options && options->InheritProcessHandle && handle)
        ObSetHandleAttributesInTable(parent->HandleTable, handle, OB_HANDLE_INHERIT, OB_HANDLE_INHERIT);
    if (options && options->InheritThreadHandle && main_thread_handle)
        ObSetHandleAttributesInTable(parent->HandleTable, main_thread_handle, OB_HANDLE_INHERIT, OB_HANDLE_INHERIT);
    PKTHREAD thread = NULL;
    if (NT_SUCCESS(status) && LinuxIsElfImage(image_data, image_size)) {
        status = ensure_elf_environment_defaults(&object->Process,
                                                  object->Process.ImagePath);
        if (NT_SUCCESS(status)) {
            if (process_image_is_feh(object->Process.ImagePath)) {
                KeTrace("[ps]   feh environment: HOME=%u FEH_SKIP_MAGIC=%u IMLIB2_LOADER_PATH=%u\n",
                      process_environment_has(&object->Process, "HOME"),
                      process_environment_has(&object->Process, "FEH_SKIP_MAGIC"),
                      process_environment_has(&object->Process, "IMLIB2_LOADER_PATH"));
            }
            const char *linux_environment[64];
            char linux_path[512];
            UINT32 linux_environment_count = process_environment_vector(
                &object->Process, linux_environment, linux_path);
            KAPC_STATE attach_state;
            KeStackAttachProcess(&object->Process, &attach_state);
            status = LinuxCreateInitialThread(&object->Process, object->Name,
                                              command, image_data, image_size,
                                              linux_environment,
                                              linux_environment_count,
                                              &thread);
            KeUnstackDetachProcess(&attach_state);
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
        PLDR_PROCESS_CONTEXT previous_loader = LdrSetThreadProcessContext(
            object->Process.LoaderContext);
        KAPC_STATE attach_state;
        KeStackAttachProcess(&object->Process, &attach_state);
        LdrResetProcessContext();
        UINT64 entry, base, argument;
        status = LdrLoadExecutableImage(filename, image_data, image_size,
                                        &entry, &base, &argument);
        if (image_data) {
            ExFreePool(image_data);
            image_data = NULL;
        }
        if (NT_SUCCESS(status))
            status = build_environment(object, command, base, entry, argument,
                                       &thread);
        KeUnstackDetachProcess(&attach_state);
        LdrSetThreadProcessContext(previous_loader);
    }
    if (NT_SUCCESS(status)) {
        PsAttachThreadObject(main_thread_object, thread);
        thread->ProcessMainThread = TRUE;
        status = W32kInitializeThreadSession(thread);
        if (NT_SUCCESS(status) && options && (options->Flags & 4u))
            status = KeSuspendThread(thread, NULL);
        if (!NT_SUCCESS(status)) KeTerminateProcess(&object->Process, status);
    }
    ObDereferenceObject(main_thread_object);
    if (!NT_SUCCESS(status)) {
        if (main_thread_handle) ObCloseHandle(main_thread_handle);
        if (handle)
            ObCloseHandle(handle);
        ObDereferenceObject(raw);
        return status;
    }
    if (out) *out = handle;
    if (pid_out) *pid_out = object->Process.ProcessId;
    if (thread_out) *thread_out = main_thread_handle;
    if (thread_id_out) *thread_id_out = thread->ThreadId;
    KIRQL old_irql;
    KeAcquireSpinLock(&g_process_list_lock, &old_irql);
    InsertTailList(&g_process_list, &object->GlobalEntry);
    KeReleaseSpinLock(&g_process_list_lock, old_irql);
    if (g_process_console_published) g_process_console_published(&object->Process);
    /* Console presentation is asynchronous. The child thread waits for its
     * host in KiThreadBootstrap, but CreateProcess must return immediately so
     * the shell and desktop do not freeze while CONHOST loads its GUI stack. */
    KeLog("[ps]   process '%s' pid %u -> handle %p, CR3 0x%lx\n",
          filename, object->Process.ProcessId, handle,
          (unsigned long)object->Process.DirectoryTableBase);
    ObDereferenceObject(raw); /* handle + running thread retain it */
    return STATUS_SUCCESS;
}

static NTSTATUS create_process_captured(const char *filename, const char *command,
    const char *directory, HANDLE *out, ULONG *pid, HANDLE input, HANDLE output,
    HANDLE error, const char *const *environment, UINT32 count)
{
    return create_process_captured_extended(filename, command, directory, out, pid,
        input, output, error, environment, count, NULL, NULL, NULL);
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
    SeConfigureType(g_process_type, 0x21410u, 0x20BEBu, 0x120000u, 0x1FFFFFu);
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
    PsCloneProcessEnvironment(target, source);
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
    /* A forked ELF child can later exec a PE image (xterm -> cmd.exe). Carry
     * the parent's station/desktop identity into that child now, just as the
     * normal process-creation path does, so the PE process parameters can
     * publish a valid startup desktop during the personality switch. */
    status = W32kInitializeProcessSession(target, source, parent);
    if (!NT_SUCCESS(status)) goto failed;
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
        KeLog("[linux-exec] loader rejected '%s': status 0x%08x\n",
              image_path, (unsigned)status);
        LdrSetThreadProcessContext(previous_loader);
        return status;
    }
    status = build_pe_user_environment(process, process->Name, command,
                                       base, &boot);
    if (!NT_SUCCESS(status)) {
        KeLog("[linux-exec] PE environment for '%s' failed: status 0x%08x\n",
              image_path, (unsigned)status);
        LdrSetThreadProcessContext(previous_loader);
        return status;
    }
    status = W32kInitializeThreadSession(thread);
    if (!NT_SUCCESS(status)) {
        KeLog("[linux-exec] desktop attachment for '%s' failed: status 0x%08x\n",
              image_path, (unsigned)status);
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
    /* The early console is a real File device. Explicit handles preserve
     * serial/headless boot and let system startup log before a desktop exists. */
    HANDLE console = NULL;
    NTSTATUS status = IoOpenConsoleKernel(&console);
    if (!NT_SUCCESS(status)) return status;
    status = create_process_captured(application, command_line, NULL, NULL,
                         NULL, console, console, console, NULL, 0);
    ObCloseHandle(console); return status;
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
    KeTrace("[ps]   NtCreateUserProcessSimple request image=%p command=%p\n",
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



NTSTATUS PsCreateConsoleHost(PKPROCESS owner, const char *command, HANDLE *host)
{
    if (!owner || owner->Exiting || !command || !host) return STATUS_INVALID_PARAMETER;
    PS_CAPTURED_PROCESS_OPTIONS options = {.ParentOverride = owner};
    return create_process_captured_extended("C:\\WINDOWS\\SYSTEM32\\CONHOST.EXE",
        command, owner->CurrentDirectory, host, NULL, NULL, NULL, NULL,
        NULL, 0, &options, NULL, NULL);
}

static NTSTATUS PspGrowCapturedBytes(char **data, SIZE_T *capacity,
                                     SIZE_T used, SIZE_T extra)
{
    if (extra > ~(SIZE_T)0 - used) return STATUS_NO_MEMORY;
    if (used + extra <= *capacity) return STATUS_SUCCESS;
    SIZE_T wanted = *capacity ? *capacity : 128;
    while (wanted < used + extra) {
        if (wanted > ~(SIZE_T)0 / 2) { wanted = used + extra; break; }
        wanted *= 2;
    }
    char *replacement = ExAllocatePoolWithTag(NonPagedPool, wanted, 'txCP');
    if (!replacement) return STATUS_NO_MEMORY;
    if (used) memcpy(replacement, *data, used);
    if (*data) ExFreePool(*data);
    *data = replacement; *capacity = wanted; return STATUS_SUCCESS;
}

static NTSTATUS PspCaptureWideBytes(UINT64 address, SIZE_T limit,
    BOOLEAN environment, char **result, SIZE_T *bytes)
{
    if (!address) return STATUS_INVALID_PARAMETER;
    char *data = NULL; SIZE_T capacity = 0, used = 0, units = 0;
    NTSTATUS status = STATUS_SUCCESS;
    for (;;) {
        if (units >= limit || address + units * 2 < address ||
            !MmProbeForRead(address + units * 2, 2)) { status = STATUS_ACCESS_VIOLATION; break; }
        UINT32 cp = *(const UINT16 *)(address + units++ * 2);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (units >= limit || !MmProbeForRead(address + units * 2, 2)) { status = STATUS_ACCESS_VIOLATION; break; }
            UINT16 low = *(const UINT16 *)(address + units * 2);
            if (low >= 0xDC00 && low <= 0xDFFF) { units++; cp = 0x10000 + ((cp - 0xD800) << 10) + low - 0xDC00; }
            else if (!environment) { status = STATUS_INVALID_PARAMETER; break; }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF && !environment) { status = STATUS_INVALID_PARAMETER; break; }
        SIZE_T extra = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        status = PspGrowCapturedBytes(&data, &capacity, used, extra + 1);
        if (!NT_SUCCESS(status)) break;
        if (cp < 0x80) data[used++] = (char)cp;
        else if (cp < 0x800) { data[used++] = (char)(0xC0 | (cp >> 6)); data[used++] = (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) {
            data[used++] = (char)(0xE0 | (cp >> 12)); data[used++] = (char)(0x80 | ((cp >> 6) & 63)); data[used++] = (char)(0x80 | (cp & 63));
        } else {
            data[used++] = (char)(0xF0 | (cp >> 18)); data[used++] = (char)(0x80 | ((cp >> 12) & 63));
            data[used++] = (char)(0x80 | ((cp >> 6) & 63)); data[used++] = (char)(0x80 | (cp & 63));
        }
        if (!cp) {
            if (!environment) break;
            if (units >= limit || !MmProbeForRead(address + units * 2, 2)) { status = STATUS_ACCESS_VIOLATION; break; }
            if (!*(const UINT16 *)(address + units * 2)) { data[used++] = 0; break; }
        }
    }
    if (!NT_SUCCESS(status)) { if (data) ExFreePool(data); return status; }
    *result = data; *bytes = used; return STATUS_SUCCESS;
}

UINT64 SvNtSetProcessEnvironment(UINT64 *args)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (!thread || !thread->Process || !thread->UserGsBase || (ULONG)args[1] < 2)
        return (UINT64)STATUS_INVALID_PARAMETER;
    char *block = NULL; SIZE_T bytes = 0;
    NTSTATUS status = PspCaptureWideBytes(args[0], (ULONG)args[1], TRUE, &block, &bytes);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    UINT64 peb_address = 0, parameters = 0;
    if (!MmProbeForRead(thread->UserGsBase + 0x60, 8)) status = STATUS_ACCESS_VIOLATION;
    else memcpy(&peb_address, (void *)(thread->UserGsBase + 0x60), 8);
    if (NT_SUCCESS(status) && !MmProbeForRead(peb_address + 0x20, 8)) status = STATUS_ACCESS_VIOLATION;
    else if (NT_SUCCESS(status)) memcpy(&parameters, (void *)(peb_address + 0x20), 8);
    if (NT_SUCCESS(status) && !MmProbeForWrite(parameters + 0x80, 8)) status = STATUS_ACCESS_VIOLATION;
    if (NT_SUCCESS(status)) status = PsSetProcessEnvironmentBlock(thread->Process, block, bytes);
    if (NT_SUCCESS(status)) __atomic_store_n((UINT64 *)(parameters + 0x80), args[0], __ATOMIC_RELEASE);
    ExFreePool(block); return (UINT64)status;
}

UINT64 SvNtHandleInformation(UINT64 *args)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (!thread || !thread->Process) return (UINT64)STATUS_INVALID_PARAMETER;
    if ((ULONG)args[1] == 1)
        return (UINT64)ObSetHandleAttributesInTable(thread->Process->HandleTable,
                    (HANDLE)args[0], (ULONG)args[2], (ULONG)args[3]);
    if (args[1]) return (UINT64)STATUS_INVALID_PARAMETER;
    ULONG value;
    if (!MmProbeForWrite(args[4], sizeof(value))) return (UINT64)STATUS_ACCESS_VIOLATION;
    NTSTATUS status = ObQueryHandleAttributesInTable(thread->Process->HandleTable, (HANDLE)args[0], &value);
    if (NT_SUCCESS(status)) *(ULONG *)args[4] = value;
    return (UINT64)status;
}

UINT64 SvNtCreateUserProcessEx(UINT64 *args)
{
    SV_PROCESS_CREATE_REQUEST request;
    SV_PROCESS_CREATE_RESULT result = {0};
    if (!MmProbeForRead(args[0], sizeof(request)) || !MmProbeForWrite(args[1], sizeof(result)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    memcpy(&request, (void *)args[0], sizeof(request));
    if (request.Size != sizeof(request) || request.Reserved || request.Padding || request.InheritHandles > 1)
        return (UINT64)STATUS_INVALID_PARAMETER;
    ULONG priority = request.Flags & 0xC1E0u;
    if (priority && (priority & (priority - 1))) return (UINT64)STATUS_INVALID_PARAMETER;
    const ULONG supported_flags = 0x08000000u | 0x04000000u | 0x0000C1E0u | 0x00000400u | 0x00000200u | 0x10u | 0x8u | 0x4u;
    if (request.Flags & ~supported_flags) return (UINT64)STATUS_NOT_SUPPORTED;
    PKTHREAD caller = KeGetCurrentThread();
    if (!caller || !caller->Process) return (UINT64)STATUS_INVALID_PARAMETER;
    SE_DESCRIPTOR *process_descriptor = NULL, *thread_descriptor = NULL;
    BOOLEAN inherit_process_handle = FALSE, inherit_thread_handle = FALSE;
    NTSTATUS security_status = SeCaptureAttributes(request.ProcessAttributes, caller->Process->SecurityUserRid,
                                          &process_descriptor, &inherit_process_handle);
    if (NT_SUCCESS(security_status)) security_status = SeCaptureAttributes(request.ThreadAttributes,
                   caller->Process->SecurityUserRid, &thread_descriptor, &inherit_thread_handle);
    if (!NT_SUCCESS(security_status)) {
        SeDereferenceDescriptor(process_descriptor); SeDereferenceDescriptor(thread_descriptor);
        return (UINT64)security_status;
    }
    char *image = NULL, *command = NULL, *directory = NULL, *environment = NULL;
    SIZE_T image_bytes = 0, command_bytes = 0, directory_bytes = 0, environment_bytes = 0;
    UINT16 *desktop = NULL;
    NTSTATUS status = PspCaptureWideBytes(request.Image, 32768, FALSE, &image, &image_bytes);
    if (NT_SUCCESS(status) && request.CommandLine)
        status = PspCaptureWideBytes(request.CommandLine, 32768, FALSE, &command, &command_bytes);
    if (NT_SUCCESS(status) && request.CurrentDirectory)
        status = PspCaptureWideBytes(request.CurrentDirectory, 32768, FALSE, &directory, &directory_bytes);
    if (NT_SUCCESS(status) && request.Environment)
        status = PspCaptureWideBytes(request.Environment, ~(SIZE_T)0 / 2, TRUE, &environment, &environment_bytes);
    if (NT_SUCCESS(status) && request.Desktop) {
        SIZE_T units = 0;
        while (units < 32768 && MmProbeForRead(request.Desktop + units * 2, 2) &&
               *(const UINT16 *)(request.Desktop + units * 2)) units++;
        if (units == 32768 || !MmProbeForRead(request.Desktop + units * 2, 2)) status = STATUS_ACCESS_VIOLATION;
        else {
            desktop = ExAllocatePoolWithTag(NonPagedPool, (units + 1) * 2, 'txCP');
            if (!desktop) status = STATUS_NO_MEMORY;
            else memcpy(desktop, (void *)request.Desktop, (units + 1) * 2);
        }
    }
    if (NT_SUCCESS(status) && (!image[0] || image_bytes > 260 || directory_bytes > 260 ||
        (command && command_bytes > PE_COMMAND_MAX))) status = STATUS_BUFFER_TOO_SMALL;
    if (NT_SUCCESS(status)) {
        PS_CAPTURED_PROCESS_OPTIONS options = {.Flags = request.Flags,
            .InheritHandles = request.InheritHandles != 0, .Desktop = desktop,
            .EnvironmentBlock = environment, .EnvironmentBytes = environment_bytes,
            .ProcessDescriptor = process_descriptor, .ThreadDescriptor = thread_descriptor,
            .InheritProcessHandle = inherit_process_handle, .InheritThreadHandle = inherit_thread_handle,
            .StartupFlags = request.StartupFlags, .ShowWindow = request.ShowWindow,
            .X = request.X, .Y = request.Y, .XSize = request.XSize, .YSize = request.YSize,
            .XChars = request.XChars, .YChars = request.YChars, .FillAttribute = request.FillAttribute};
        HANDLE process = NULL, thread = NULL; ULONG pid = 0, tid = 0;
        status = create_process_captured_extended(image, command ? command : image,
            directory, &process, &pid, (HANDLE)request.StandardInput,
            (HANDLE)request.StandardOutput, (HANDLE)request.StandardError,
            NULL, 0, &options, &thread, &tid);
        if (NT_SUCCESS(status)) {
            result.Process = (UINT64)process; result.Thread = (UINT64)thread;
            result.ProcessId = pid; result.ThreadId = tid;
            memcpy((void *)args[1], &result, sizeof(result));
        }
    }
    if (desktop) ExFreePool(desktop);
    if (environment) ExFreePool(environment);
    if (directory) ExFreePool(directory);
    if (command) ExFreePool(command);
    if (image) ExFreePool(image);
    SeDereferenceDescriptor(process_descriptor); SeDereferenceDescriptor(thread_descriptor);
    return (UINT64)status;
}
