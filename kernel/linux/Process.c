/* Linux process startup: ELF mapping, SysV initial stack, ring-3 thread. */
#include "Internal.h"
#include <svindows/Mm.h>
#include <svindows/Rtl.h>
#include <svindows/Ex.h>
#include <svindows/Io.h>
#include <svindows/Ob.h>

#define LINUX_STACK_TOP 0x70000000ULL
#define LINUX_STACK_PAGES 32u
#define LINUX_ARG_MAX 64u
#define LINUX_ENV_MAX 64u
#define LINUX_MAIN_PIE_BASE 0x20000000ULL
#define LINUX_INTERP_BASE 0x30000000ULL

static UINT64 linux_startup_random_word(void)
{
    UINT32 eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    if (ecx & (1u << 30)) {
        for (UINT32 attempt = 0; attempt < 10; attempt++) {
            UINT64 value;
            UINT8 valid;
            __asm__ volatile("rdrand %0; setc %1"
                             : "=r"(value), "=qm"(valid));
            if (valid) return value;
        }
    }
    /* Boot-time fallback until the kernel has a general entropy pool. */
    UINT32 low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    UINT64 value = ((UINT64)high << 32 | low) ^ KeQuerySystemTimeFileTime();
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    return value;
}

static NTSTATUS load_interpreter(const char *path, LINUX_ELF_INFO *info)
{
    /* PT_INTERP is an absolute Linux path; FAT uses a C: root. */
    char raw[260] = {'C', ':', '\\'};
    SIZE_T n = 3;
    for (SIZE_T i = 1; path[i]; i++) {
        if (n + 1 >= sizeof(raw)) return STATUS_INVALID_IMAGE_FORMAT;
        raw[n++] = path[i] == '/' ? '\\' : path[i];
    }
    raw[n] = 0;
    char canonical[260];
    if (!IoCanonicalizeDosPathKernel(raw, canonical))
        return STATUS_INVALID_IMAGE_FORMAT;
    void *image = NULL;
    SIZE_T size = 0;
    NTSTATUS status = FatLoadFile(canonical, &image, &size, NULL);
    if (!NT_SUCCESS(status)) return status;
    status = LinuxLoadElf64(image, size, LINUX_INTERP_BASE, TRUE, info);
    ExFreePool(image);
    return status;
}

static BOOLEAN push_bytes(UINT64 *cursor, UINT64 bottom, const void *source,
                          SIZE_T length, UINT64 *address)
{
    if (length > *cursor - bottom) return FALSE;
    *cursor -= length;
    memcpy((void *)*cursor, source, length);
    if (address) *address = *cursor;
    return TRUE;
}

static NTSTATUS linux_parse_command_argument(const char **cursor,
                                             char out[768], BOOLEAN *present)
{
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t') p++;
    *present = *p != 0;
    if (!*present) { *cursor = p; return STATUS_SUCCESS; }
    SIZE_T n = 0;
    BOOLEAN quoted = FALSE;
    while (*p) {
        SIZE_T slashes = 0;
        while (*p == '\\') { slashes++; p++; }
        if (*p == '"') {
            for (SIZE_T i = 0; i < slashes / 2; i++) {
                if (n + 1 >= 768) return STATUS_BUFFER_TOO_SMALL;
                out[n++] = '\\';
            }
            if (slashes & 1) {
                if (n + 1 >= 768) return STATUS_BUFFER_TOO_SMALL;
                out[n++] = '"';
                p++;
            } else if (quoted && p[1] == '"') {
                if (n + 1 >= 768) return STATUS_BUFFER_TOO_SMALL;
                out[n++] = '"';
                p += 2;
            } else {
                quoted = !quoted;
                p++;
            }
            continue;
        }
        while (slashes--) {
            if (n + 1 >= 768) return STATUS_BUFFER_TOO_SMALL;
            out[n++] = '\\';
        }
        if (!*p || (!quoted && (*p == ' ' || *p == '\t'))) break;
        if (n + 1 >= 768) return STATUS_BUFFER_TOO_SMALL;
        out[n++] = *p++;
    }
    out[n] = 0;
    *cursor = p;
    return STATUS_SUCCESS;
}

static NTSTATUS build_stack(PKPROCESS process, const char *command,
                            const char *const *exec_argv, UINT32 exec_argc,
                            const char *const *exec_env, UINT32 exec_envc,
                            const LINUX_ELF_INFO *main_image,
                            const LINUX_ELF_INFO *interpreter,
                            UINT64 *stack_out)
{
    UINT64 bottom = LINUX_STACK_TOP - LINUX_STACK_PAGES * PAGE_SIZE;
    for (UINT64 page = bottom; page < LINUX_STACK_TOP; page += PAGE_SIZE) {
        UINT64 physical = MmAllocatePage();
        if (physical == MM_INVALID_PHYS) return STATUS_NO_MEMORY;
        if (!MmMapPage(page, physical, PTE_USER | PTE_WRITE | PTE_NX)) {
            MmFreePages(physical, 1);
            return STATUS_NO_MEMORY;
        }
        memset((void *)page, 0, PAGE_SIZE);
    }

    UINT64 cursor = LINUX_STACK_TOP;
    UINT64 arguments[LINUX_ARG_MAX];
    UINT64 environment[LINUX_ENV_MAX];
    UINT32 argc = 0;
    if (exec_argc > LINUX_ARG_MAX || exec_envc > LINUX_ENV_MAX)
        return STATUS_INVALID_PARAMETER;
    /* Native process paths are DOS paths; Linux argv[0] and AT_EXECFN must
     * use the same /-rooted namespace that Linux file syscalls expose. */
    char image_path[260];
    const char *native_path = process->ImagePath;
    if (native_path[0] && native_path[1] == ':' &&
        (native_path[2] == '\\' || native_path[2] == '/'))
        native_path += 2;
    SIZE_T length = 0;
    while (native_path[length] && length + 1 < sizeof(image_path)) {
        image_path[length] = native_path[length] == '\\' ? '/' :
                             native_path[length];
        length++;
    }
    if (native_path[length] || !length || image_path[0] != '/')
        return STATUS_INVALID_PARAMETER;
    image_path[length] = 0;
    UINT64 execfn;
    if (!push_bytes(&cursor, bottom, image_path, length + 1, &execfn))
        return STATUS_INVALID_PARAMETER;

    if (command) {
        /* The typed program name becomes the resolved ELF path in argv[0]. */
        arguments[argc++] = execfn;
        const char *p = command;
        char ignored[768];
        BOOLEAN present;
        NTSTATUS parsed = linux_parse_command_argument(&p, ignored, &present);
        if (!NT_SUCCESS(parsed)) return parsed;
        while (TRUE) {
            char argument[768];
            parsed = linux_parse_command_argument(&p, argument, &present);
            if (!NT_SUCCESS(parsed)) return parsed;
            if (!present) break;
            if (argc == LINUX_ARG_MAX) return STATUS_BUFFER_TOO_SMALL;
            SIZE_T n = strlen(argument) + 1;
            if (!push_bytes(&cursor, bottom, argument, n,
                            &arguments[argc++]))
                return STATUS_BUFFER_TOO_SMALL;
        }
    } else {
        for (UINT32 i = 0; i < exec_argc; i++) {
            SIZE_T n = strlen(exec_argv[i]) + 1;
            if (!push_bytes(&cursor, bottom, exec_argv[i], n,
                            &arguments[argc++]))
                return STATUS_INVALID_PARAMETER;
        }
        if (!argc) arguments[argc++] = execfn;
    }
    for (UINT32 i = 0; i < exec_envc; i++) {
        SIZE_T n = strlen(exec_env[i]) + 1;
        if (!push_bytes(&cursor, bottom, exec_env[i], n, &environment[i]))
            return STATUS_INVALID_PARAMETER;
    }

    UINT64 random_words[2] = {linux_startup_random_word(),
                              linux_startup_random_word()};
    UINT64 random_address;
    if (!push_bytes(&cursor, bottom, random_words, sizeof(random_words),
                    &random_address))
        return STATUS_INVALID_PARAMETER;

    cursor &= ~15ULL;

    /* argc, argv[], NULL, envp NULL, then Linux auxv pairs. */
    const UINT64 aux[][2] = {
        {3, main_image->ProgramHeaders}, {4, 56},
        {5, main_image->ProgramCount}, {6, PAGE_SIZE},
        {7, interpreter ? interpreter->LoadBase : 0}, {8, 0},
        {9, main_image->Entry}, {11, 0}, {12, 0}, {13, 0}, {14, 0},
        {16, 0}, {17, 100}, {23, 0}, {25, random_address},
        {26, 0}, {31, execfn}, {0, 0}
    };
    UINT64 words = 1 + argc + 1 + exec_envc + 1 +
                   2 * (sizeof(aux) / sizeof(aux[0]));
    UINT64 bytes = words * sizeof(UINT64);
    if (bytes > cursor - bottom) return STATUS_INVALID_PARAMETER;
    cursor = (cursor - bytes) & ~15ULL;
    UINT64 *stack = (UINT64 *)cursor;
    UINT64 index = 0;
    stack[index++] = argc;
    for (UINT32 i = 0; i < argc; i++) stack[index++] = arguments[i];
    stack[index++] = 0;
    for (UINT32 i = 0; i < exec_envc; i++)
        stack[index++] = environment[i];
    stack[index++] = 0;
    for (unsigned i = 0; i < sizeof(aux) / sizeof(aux[0]); i++) {
        stack[index++] = aux[i][0];
        stack[index++] = aux[i][1];
    }
    *stack_out = cursor;
    return STATUS_SUCCESS;
}

NTSTATUS LinuxCreateInitialThread(PKPROCESS process, const char *name,
                                  const char *command, const void *image,
                                  SIZE_T size,
                                  const char *const *environment,
                                  UINT32 environment_count,
                                  PKTHREAD *thread_out)
{
    LINUX_ELF_INFO main_image, interpreter;
    UINT64 stack;
    NTSTATUS status = LinuxLoadElf64(image, size, LINUX_MAIN_PIE_BASE,
                                    FALSE, &main_image);
    if (!NT_SUCCESS(status)) return status;
    BOOLEAN has_interpreter = main_image.Interpreter[0] != 0;
    if (has_interpreter) {
        status = load_interpreter(main_image.Interpreter, &interpreter);
        if (!NT_SUCCESS(status)) return status;
    }
    status = build_stack(process, command, NULL, 0, environment,
                         environment_count, &main_image,
                         has_interpreter ? &interpreter : NULL, &stack);
    if (!NT_SUCCESS(status)) return status;
    process->LinuxContext = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*process->LinuxContext), 'corL');
    if (!process->LinuxContext) return STATUS_NO_MEMORY;
    memset(process->LinuxContext, 0, sizeof(*process->LinuxContext));
    process->LinuxContext->SessionId = process->ProcessId;
    process->LinuxContext->ProcessGroupId = process->ProcessId;
    UINT64 image_end = main_image.ImageEnd;
    if (has_interpreter && interpreter.ImageEnd > image_end)
        image_end = interpreter.ImageEnd;
    status = LinuxInitializeMemory(process, image_end);
    if (!NT_SUCCESS(status)) {
        LinuxDestroyProcess(process);
        return status;
    }
    LinuxInitializeFiles(process);
    LINUX_THREAD_CONTEXT *linux_thread = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*linux_thread), 'rhtL');
    if (!linux_thread) return STATUS_NO_MEMORY;
    memset(linux_thread, 0, sizeof(*linux_thread));
    linux_thread->Tid = process->ProcessId;
    KeInitializeEvent(&linux_thread->FutexEvent, FALSE, FALSE);
    PKTHREAD thread = KeCreateUserThreadForProcess(process, name,
                    has_interpreter ? interpreter.Entry : main_image.Entry,
                                                   stack, 0, 0,
                                                   process->BasePriority);
    if (!thread) {
        ExFreePool(linux_thread);
        return STATUS_NO_MEMORY;
    }
    thread->LinuxThread = linux_thread;
    *thread_out = thread;
    return STATUS_SUCCESS;
}

NTSTATUS LinuxLoadExecImage(PKPROCESS process, const void *image, SIZE_T size,
                            const char *const *argv, UINT32 argc,
                            const char *const *env, UINT32 envc,
                            UINT64 *entry_out, UINT64 *stack_out)
{
    LINUX_ELF_INFO main_image, interpreter;
    NTSTATUS status = LinuxLoadElf64(image, size, LINUX_MAIN_PIE_BASE,
                                    FALSE, &main_image);
    if (!NT_SUCCESS(status)) return status;
    BOOLEAN has_interpreter = main_image.Interpreter[0] != 0;
    if (has_interpreter) {
        status = load_interpreter(main_image.Interpreter, &interpreter);
        if (!NT_SUCCESS(status)) return status;
    }
    status = build_stack(process, NULL, argv, argc, env, envc, &main_image,
                         has_interpreter ? &interpreter : NULL, stack_out);
    if (!NT_SUCCESS(status)) return status;
    UINT64 image_end = main_image.ImageEnd;
    if (has_interpreter && interpreter.ImageEnd > image_end)
        image_end = interpreter.ImageEnd;
    memset(&process->LinuxContext->Memory, 0,
           sizeof(process->LinuxContext->Memory));
    status = LinuxInitializeMemory(process, image_end);
    if (!NT_SUCCESS(status)) return status;
    *entry_out = has_interpreter ? interpreter.Entry : main_image.Entry;
    return STATUS_SUCCESS;
}

void LinuxDestroyProcess(PKPROCESS process)
{
    if (!process->LinuxContext) return;
    LinuxFlockReleaseProcess(process);
    /* Device-backed user PTEs are aliases, not allocator-owned RAM. The
     * generic address-space destructor frees all remaining user leaves. */
    LINUX_MEMORY_CONTEXT *memory = &process->LinuxContext->Memory;
    for (UINT32 i = 0; i < 32; i++) {
        HANDLE child = process->LinuxContext->Children[i].ProcessHandle;
        if (child)
            ObCloseHandleForObject(process->HandleTable, child, NULL);
    }
    for (UINT32 i = 0; i < LINUX_MAX_FDS; i++) {
        LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[i];
        if (entry->Used && entry->Kind == LINUX_FD_DIRECTORY)
            ExFreePool(entry->DirectoryPath);
        if (entry->Used && entry->Kind == LINUX_FD_EPOLL)
            LinuxEpollClose(entry->Epoll);
        if (entry->Used && entry->Kind == LINUX_FD_SOCKET)
            LinuxSocketClose(entry->Socket);
        if (entry->Used && entry->Kind == LINUX_FD_TTY)
            LinuxVtClose(process, entry->Terminal);
        if (entry->Used && entry->Kind == LINUX_FD_PTY)
            LinuxPtyClose(entry->Pty, entry->PtyMaster);
    }
    if (process->LinuxContext->ControllingPty)
        LinuxPtyControlClose(process->LinuxContext->ControllingPty);
    LinuxVtProcessExit(process);
    for (UINT32 i = 0; i < LINUX_MAP_REGIONS; i++) {
        const LINUX_MAP_REGION *region = &memory->Regions[i];
        if (region->Kind != LINUX_MAP_FRAMEBUFFER) continue;
        for (UINT64 page = region->Base;
             page < region->Base + region->Size; page += PAGE_SIZE)
            MmUnmapPageInAddressSpace(process->DirectoryTableBase, page);
    }
    LinuxReleaseFramebufferOwner(memory);
    ExFreePool(process->LinuxContext);
    process->LinuxContext = NULL;
}
