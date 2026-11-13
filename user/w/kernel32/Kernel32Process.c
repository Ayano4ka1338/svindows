/* Minimal Win32 process facade, kept separate from the core kernel32 module. */
typedef void *HANDLE;
typedef void *LPVOID;
typedef unsigned long DWORD;
typedef unsigned long long SIZE_T;
typedef int BOOL;

typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD  dwProcessId;
    DWORD  dwThreadId;
} PROCESS_INFORMATION;

extern long NtCreateUserProcessSimple(const char *application,
                                      const char *command_line,
                                      HANDLE *process, DWORD *process_id,
                                      HANDLE input, HANDLE output,
                                      HANDLE error,
                                      const char *current_directory,
                                      const char *environment_block);
extern void SetLastError(DWORD error);
extern DWORD GetCurrentDirectoryW(DWORD size, unsigned short *out);
extern long NtQueryInformationProcess(HANDLE process, DWORD info_class,
                                      void *info, DWORD length,
                                      DWORD *return_length);
extern long NtSetInformationProcess(HANDLE process, DWORD info_class,
                                    const void *info, DWORD length);
extern long NtDuplicateObject(HANDLE source_process, HANDLE source_handle,
                              HANDLE target_process, HANDLE *target_handle,
                              DWORD desired_access, DWORD attributes,
                              DWORD options);
extern long NtOpenProcess(HANDLE *process, DWORD access, void *attributes,
                          const void *client_id);
extern BOOL CloseHandle(HANDLE handle);
extern long SvNtQueryThreadExitStatus(HANDLE thread, DWORD *exit_status);
extern long SvNtReadProcessMemory(HANDLE process, const void *source,
                                  void *destination, SIZE_T size,
                                  SIZE_T *bytes_read);
extern DWORD SvNtQueryGuiResources(DWORD process_id, DWORD kind);
extern long NtQuerySystemInformation(DWORD info_class, void *buffer,
                                     DWORD length, DWORD *return_length);

__declspec(dllexport) BOOL GetExitCodeThread(HANDLE thread, DWORD *exit_code)
{
    if (!exit_code) { SetLastError(87); return 0; }
    DWORD status = 0;
    long result = SvNtQueryThreadExitStatus(thread, &status);
    if (result < 0) { SetLastError(6); return 0; }
    *exit_code = status == 0x00000103u ? 259u : status;
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL GetLogicalProcessorInformationEx(
    DWORD relationship, void *buffer, DWORD *returned_length)
{
    const DWORD RELATION_PROCESSOR_CORE = 0;
    const DWORD RELATION_GROUP = 4;
    const DWORD RELATION_ALL = 0xffff;
    if (!returned_length || (relationship != RELATION_PROCESSOR_CORE &&
        relationship != RELATION_GROUP && relationship != RELATION_ALL)) {
        SetLastError(relationship == RELATION_PROCESSOR_CORE ||
                     relationship == RELATION_GROUP ||
                     relationship == RELATION_ALL ? 87 : 50);
        return 0;
    }

    struct {
        DWORD Reserved, TimerResolution, PageSize, PhysicalPages;
        DWORD LowestPage, HighestPage, AllocationGranularity, Padding;
        unsigned long long MinimumAddress, MaximumAddress, ActiveMask;
        unsigned char ProcessorCount;
        unsigned char Tail[7];
    } basic;
    DWORD actual = 0;
    long status = NtQuerySystemInformation(0, &basic, sizeof(basic), &actual);
    if (status < 0 || !basic.ProcessorCount || basic.ProcessorCount > 64) {
        SetLastError(50);
        return 0;
    }
    DWORD core_count = 0;
    for (DWORD i = 0; i < basic.ProcessorCount; ++i)
        if (basic.ActiveMask & (1ull << i)) core_count++;
    if (!core_count) { SetLastError(50); return 0; }
    DWORD need = (relationship == RELATION_GROUP ? 0 : core_count * 48) +
                 (relationship == RELATION_PROCESSOR_CORE ? 0 : 80);
    DWORD capacity = *returned_length;
    *returned_length = need;
    if (!buffer || capacity < need) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    unsigned char *out = buffer;
    DWORD offset = 0;
    if (relationship != RELATION_GROUP) {
        for (DWORD i = 0; i < basic.ProcessorCount; ++i) {
            unsigned long long affinity = 1ull << i;
            if (!(basic.ActiveMask & affinity)) continue;
            unsigned char *record = out + offset;
            for (DWORD j = 0; j < 48; ++j) record[j] = 0;
            *(DWORD *)(record + 0) = RELATION_PROCESSOR_CORE;
            *(DWORD *)(record + 4) = 48;
            *(unsigned short *)(record + 30) = 1; /* one processor group */
            *(unsigned long long *)(record + 32) = affinity;
            offset += 48;
        }
    }
    if (relationship != RELATION_PROCESSOR_CORE) {
        unsigned char *record = out + offset;
        for (DWORD j = 0; j < 80; ++j) record[j] = 0;
        *(DWORD *)(record + 0) = RELATION_GROUP;
        *(DWORD *)(record + 4) = 80;
        *(unsigned short *)(record + 8) = 1;  /* maximum processor groups */
        *(unsigned short *)(record + 10) = 1; /* active processor groups */
        record[32] = basic.ProcessorCount;
        record[33] = (unsigned char)core_count;
        *(unsigned long long *)(record + 72) = basic.ActiveMask;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL GetNumaHighestNodeNumber(DWORD *highest_node)
{
    if (!highest_node) { SetLastError(87); return 0; }
    /* The physical memory manager has one uniform allocation domain. */
    *highest_node = 0;
    SetLastError(0);
    return 1;
}

__declspec(dllexport) HANDLE OpenProcess(DWORD access, BOOL inherit,
                                         DWORD process_id)
{
    if (!process_id) { SetLastError(87); return 0; }
    struct { HANDLE Process; HANDLE Thread; } id = {
        (HANDLE)(unsigned long long)process_id, 0
    };
    HANDLE process = 0;
    struct { DWORD Length; HANDLE Root; void *Name; DWORD Flags; void *Descriptor,*Quality; } attributes = {0};
    attributes.Length=sizeof(attributes);attributes.Flags=inherit?2u:0;
    long status = NtOpenProcess(&process, access, &attributes, &id);
    if (status < 0) {
        SetLastError(status == (long)0xC000000D ||
                     status == (long)0xC0000225 ? 87 : 6);
        return 0;
    }
    return process;
}

__declspec(dllexport) BOOL ProcessIdToSessionId(DWORD process_id,
                                                DWORD *session_id)
{
    if (!process_id || !session_id) { SetLastError(87); return 0; }
    HANDLE process = OpenProcess(0x1000u, 0, process_id);
    if (!process) return 0;
    DWORD session=0;
    long status=NtQueryInformationProcess(process,24,&session,sizeof(session),0);
    CloseHandle(process);
    if(status<0){SetLastError(6);return 0;}
    *session_id=session;
    SetLastError(0);
    return 1;
}

/* Private process information class; the kernel owns these values so they
 * follow the process rather than a particular kernel32 DLL instance. */
#define PROCESS_SHUTDOWN_PARAMETERS 0x1000u
#define PROCESS_ERROR_MODE 0x1001u

__declspec(dllexport) DWORD SetErrorMode(DWORD mode)
{
    DWORD parameters[2] = {mode, 0};
    long status = NtSetInformationProcess((HANDLE)-1, PROCESS_ERROR_MODE,
                                          parameters, sizeof(parameters));
    if (status < 0) { SetLastError(status == (long)0xC000000D ? 87 : 6); return 0; }
    return parameters[1];
}

__declspec(dllexport) DWORD GetErrorMode(void)
{
    DWORD mode = 0;
    long status = NtQueryInformationProcess((HANDLE)-1, PROCESS_ERROR_MODE,
                                             &mode, sizeof(mode), 0);
    if (status < 0) { SetLastError(6); return 0; }
    return mode;
}

__declspec(dllexport) BOOL SetProcessShutdownParameters(DWORD level,
                                                          DWORD flags)
{
    DWORD parameters[2] = {level, flags};
    long status = NtSetInformationProcess((HANDLE)-1,
                                          PROCESS_SHUTDOWN_PARAMETERS,
                                          parameters, sizeof(parameters));
    if (status < 0) {
        SetLastError(status == (long)0xC000000D ? 87 : 6);
        return 0;
    }
    return 1;
}

__declspec(dllexport) BOOL GetProcessShutdownParameters(DWORD *level,
                                                          DWORD *flags)
{
    if (!level || !flags) { SetLastError(87); return 0; }
    DWORD parameters[2];
    long status = NtQueryInformationProcess((HANDLE)-1,
                                             PROCESS_SHUTDOWN_PARAMETERS,
                                             parameters, sizeof(parameters), 0);
    if (status < 0) { SetLastError(6); return 0; }
    *level = parameters[0];
    *flags = parameters[1];
    return 1;
}

__declspec(dllexport) BOOL DuplicateHandle(HANDLE source_process,
                                            HANDLE source_handle,
                                            HANDLE target_process,
                                            HANDLE *target_handle,
                                            DWORD desired_access,
                                            BOOL inherit_handle,
                                            DWORD options)
{
    if (!target_handle) { SetLastError(87); return 0; }
    long status = NtDuplicateObject(source_process, source_handle,
                                     target_process, target_handle,
                                     desired_access,
                                     inherit_handle ? 2u : 0u, options);
    if (status < 0) {
        SetLastError(status == (long)0xC00000BB ? 50 : 6);
        return 0;
    }
    return 1;
}

typedef struct { unsigned char Foreground, PriorityClass; } PRIORITY_CLASS_INFO;

static DWORD win32_priority_class(unsigned char native_class)
{
    static const DWORD classes[] = {0, 0x40, 0x20, 0x80, 0x100,
                                    0x4000, 0x8000};
    return native_class <= 6 ? classes[native_class] : 0;
}

__declspec(dllexport) DWORD GetPriorityClass(HANDLE process)
{
    PRIORITY_CLASS_INFO info;
    long status = NtQueryInformationProcess(process, 18, &info, sizeof(info), 0);
    if (status < 0) {
        SetLastError(6);
        return 0;
    }
    return win32_priority_class(info.PriorityClass);
}

__declspec(dllexport) BOOL SetPriorityClass(HANDLE process, DWORD priority)
{
    PRIORITY_CLASS_INFO info = {0, 0};
    for (unsigned char i = 1; i <= 6; i++)
        if (win32_priority_class(i) == priority) info.PriorityClass = i;
    if (!info.PriorityClass) {
        SetLastError(87);
        return 0;
    }
    long status = NtSetInformationProcess(process, 18, &info, sizeof(info));
    if (status < 0) {
        SetLastError(status == (long)0xC000000D ? 87 : 6);
        return 0;
    }
    return 1;
}

typedef struct _PROCESS_BASIC_INFORMATION_LOCAL {
    long   ExitStatus;
    DWORD  Padding;
    void  *PebBaseAddress;
    unsigned long long AffinityMask;
    long   BasePriority;
    DWORD  Padding2;
    unsigned long long UniqueProcessId;
    unsigned long long InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION_LOCAL;

typedef struct _STARTUPINFOA_LOCAL {
    DWORD cb;
    char *reserved;
    char *desktop;
    char *title;
    DWORD x, y, x_size, y_size, x_chars, y_chars, fill, flags;
    unsigned short show_window, reserved2_size;
    unsigned char *reserved2;
    HANDLE standard_input, standard_output, standard_error;
} STARTUPINFOA_LOCAL;

#define STARTF_USESTDHANDLES 0x00000100u

__declspec(dllexport) BOOL GetExitCodeProcess(HANDLE process, DWORD *exit_code)
{
    PROCESS_BASIC_INFORMATION_LOCAL info;
    if (!exit_code) {
        SetLastError(87);
        return 0;
    }
    long status = NtQueryInformationProcess(process, 0, &info, sizeof(info), 0);
    if (status < 0) {
        SetLastError(6);
        return 0;
    }
    *exit_code = (DWORD)info.ExitStatus;
    SetLastError(0);
    return 1;
}

__declspec(dllexport) DWORD GetProcessId(HANDLE process)
{
    PROCESS_BASIC_INFORMATION_LOCAL info;
    long status = NtQueryInformationProcess(process, 0, &info, sizeof(info), 0);
    if (status < 0) {
        SetLastError(6);
        return 0;
    }
    return (DWORD)info.UniqueProcessId;
}

__declspec(dllexport) DWORD Kernel32GetGuiResources(HANDLE process, DWORD flags)
{
    if (flags > 1) { SetLastError(87); return 0; }
    DWORD process_id = GetProcessId(process);
    if (!process_id) return 0;
    DWORD count = SvNtQueryGuiResources(process_id, flags);
    SetLastError(0);
    return count;
}

__declspec(dllexport) DWORD GetGuiResources(HANDLE process, DWORD flags)
{
    return Kernel32GetGuiResources(process, flags);
}

__declspec(dllexport) BOOL ReadProcessMemory(HANDLE process,
    const void *source, void *destination, SIZE_T size, SIZE_T *bytes_read)
{
    SIZE_T copied = 0;
    long status = SvNtReadProcessMemory(process, source, destination, size,
                                       &copied);
    if (bytes_read) *bytes_read = copied;
    if (status < 0) {
        SetLastError(copied ? 299 : 6); /* partial copy / invalid access */
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL QueryFullProcessImageNameW(HANDLE process,
                                                       DWORD flags,
                                                       unsigned short *out,
                                                       DWORD *size)
{
    if (!out || !size || flags != 0) {
        SetLastError(87);
        return 0;
    }
    struct {
        unsigned short Length, MaximumLength;
        unsigned short *Buffer;
        unsigned short Storage[260];
    } information;
    long status = NtQueryInformationProcess(process, 27, &information,
                                             sizeof(information), 0);
    if (status < 0) {
        SetLastError(6);
        return 0;
    }
    DWORD chars = information.Length / sizeof(unsigned short);
    if (*size <= chars) {
        *size = chars + 1;
        SetLastError(122);
        return 0;
    }
    for (DWORD i = 0; i <= chars; i++) out[i] = information.Buffer[i];
    *size = chars;
    return 1;
}


#include "../../../kernel/include/abi/ProcessCreate.h"
extern long SvNtCreateUserProcessEx(const SV_PROCESS_CREATE_REQUEST *, SV_PROCESS_CREATE_RESULT *);
extern DWORD RtlNtStatusToDosError(long status);
extern HANDLE GetProcessHeap(void);
extern LPVOID HeapAlloc(HANDLE, DWORD, SIZE_T);
extern BOOL HeapFree(HANDLE, DWORD, LPVOID);
extern int MultiByteToWideChar(unsigned int, DWORD, const char *, int, unsigned short *, int);
extern DWORD SearchPathW(const unsigned short *, const unsigned short *, const unsigned short *, DWORD, unsigned short *, unsigned short **);
extern DWORD GetFileAttributesW(const unsigned short *);

static unsigned short *process_widen(const char *text, int bytes)
{
    if (!text) return 0;
    int count = MultiByteToWideChar(0, 0, text, bytes, 0, 0);
    if (!count) return 0;
    unsigned short *wide = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)count * 2);
    if (!wide) { SetLastError(8); return 0; }
    if (!MultiByteToWideChar(0, 0, text, bytes, wide, count)) {
        HeapFree(GetProcessHeap(), 0, wide); return 0;
    }
    return wide;
}

static unsigned short *process_ansi_environment(const char *environment)
{
    SIZE_T bytes = 0;
    do {
        while (environment[bytes]) bytes++;
        bytes++;
    } while (environment[bytes]);
    bytes++;
    if (bytes > 0x7FFFFFFF) { SetLastError(8); return 0; }
    return process_widen(environment, (int)bytes);
}

static BOOL create_process_wide(const unsigned short *application,
    unsigned short *command, BOOL inherit, DWORD flags, LPVOID environment,
    const unsigned short *directory, const STARTUPINFOA_LOCAL *startup,
    const unsigned short *desktop, LPVOID process_attributes, LPVOID thread_attributes, PROCESS_INFORMATION *out)
{
    if (!out || !startup || startup->cb < sizeof(*startup) || (!application && !command)) {
        SetLastError(87); return 0;
    }
    unsigned short first[260];
    if (!application) {
        const unsigned short *scan = command;
        while (*scan == ' ' || *scan == '\t') scan++;
        unsigned int count = 0; int quoted = 0;
        while (*scan) {
            if (*scan == '"') { quoted = !quoted; scan++; continue; }
            if (!quoted && (*scan == ' ' || *scan == '\t')) break;
            if (count + 1 == 260) { SetLastError(206); return 0; }
            first[count++] = *scan++;
        }
        first[count] = 0; application = first;
    }
    if (!*application) { SetLastError(87); return 0; }
    unsigned short resolved[260];
    DWORD attributes = GetFileAttributesW(application);
    if (attributes == (DWORD)-1 || (attributes & 0x10)) {
        static const unsigned short extension[] = {'.','e','x','e',0};
        DWORD length = SearchPathW(0, application, extension, 260, resolved, 0);
        if (!length) return 0;
        if (length >= 260) { SetLastError(206); return 0; }
        application = resolved;
    }
    SV_PROCESS_CREATE_REQUEST request = {0};
    request.Size = sizeof(request); request.Flags = flags;
    request.InheritHandles = inherit != 0;
    request.Image = (unsigned long long)application;
    request.CommandLine = (unsigned long long)(command ? command : application);
    request.CurrentDirectory = (unsigned long long)directory;
    request.Desktop = (unsigned long long)desktop;
    request.Environment = (unsigned long long)environment;
    request.ProcessAttributes = (unsigned long long)process_attributes;
    request.ThreadAttributes = (unsigned long long)thread_attributes;
    request.StartupFlags = startup->flags; request.ShowWindow = startup->show_window;
    request.X = startup->x; request.Y = startup->y;
    request.XSize = startup->x_size; request.YSize = startup->y_size;
    request.XChars = startup->x_chars; request.YChars = startup->y_chars;
    request.FillAttribute = startup->fill;
    if (startup->flags & STARTF_USESTDHANDLES) {
        request.StandardInput = (unsigned long long)startup->standard_input;
        request.StandardOutput = (unsigned long long)startup->standard_output;
        request.StandardError = (unsigned long long)startup->standard_error;
    }
    SV_PROCESS_CREATE_RESULT result = {0};
    long status = SvNtCreateUserProcessEx(&request, &result);
    if (status < 0) { SetLastError(RtlNtStatusToDosError(status)); return 0; }
    out->hProcess = (HANDLE)result.Process; out->hThread = (HANDLE)result.Thread;
    out->dwProcessId = result.ProcessId; out->dwThreadId = result.ThreadId;
    return 1;
}

__declspec(dllexport) BOOL CreateProcessW(const unsigned short *application,
    unsigned short *command, LPVOID process_attributes, LPVOID thread_attributes,
    BOOL inherit, DWORD flags, LPVOID environment, const unsigned short *directory,
    LPVOID startup_info, PROCESS_INFORMATION *out)
{
    STARTUPINFOA_LOCAL *startup = startup_info;
    unsigned short *wide_environment = 0;
    if (environment && !(flags & 0x400u)) {
        wide_environment = process_ansi_environment(environment);
        if (!wide_environment) return 0;
        environment = wide_environment;
    }
    BOOL result = create_process_wide(application, command, inherit, flags,
        environment, directory, startup, startup ? (const unsigned short *)startup->desktop : 0, process_attributes, thread_attributes, out);
    if (wide_environment) HeapFree(GetProcessHeap(), 0, wide_environment);
    return result;
}

__declspec(dllexport) BOOL CreateProcessA(const char *application, char *command,
    LPVOID process_attributes, LPVOID thread_attributes, BOOL inherit, DWORD flags,
    LPVOID environment, const char *directory, LPVOID startup_info,
    PROCESS_INFORMATION *out)
{
    STARTUPINFOA_LOCAL *startup = startup_info;
    unsigned short *image = process_widen(application, -1);
    if (application && !image) return 0;
    unsigned short *line = process_widen(command, -1);
    unsigned short *cwd = process_widen(directory, -1);
    unsigned short *desktop = startup && startup->desktop ? process_widen(startup->desktop, -1) : 0;
    unsigned short *wide_environment = 0;
    if (environment && !(flags & 0x400u)) wide_environment = process_ansi_environment(environment);
    BOOL valid = (!command || line) && (!directory || cwd) &&
        (!startup || !startup->desktop || desktop) &&
        (!environment || (flags & 0x400u) || wide_environment);
    BOOL result = valid ? create_process_wide(image, line, inherit, flags,
        wide_environment ? wide_environment : environment, cwd, startup, desktop, process_attributes, thread_attributes, out) : 0;
    if (wide_environment) HeapFree(GetProcessHeap(), 0, wide_environment);
    if (desktop) HeapFree(GetProcessHeap(), 0, desktop);
    if (cwd) HeapFree(GetProcessHeap(), 0, cwd);
    if (line) HeapFree(GetProcessHeap(), 0, line);
    if (image) HeapFree(GetProcessHeap(), 0, image);
    return result;
}
