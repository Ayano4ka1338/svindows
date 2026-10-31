/* Minimal Win32 process facade, kept separate from the core kernel32 module. */
typedef void *HANDLE;
typedef void *LPVOID;
typedef unsigned long DWORD;
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

__declspec(dllexport) HANDLE OpenProcess(DWORD access, BOOL inherit,
                                         DWORD process_id)
{
    if (inherit || !process_id) { SetLastError(87); return 0; }
    struct { HANDLE Process; HANDLE Thread; } id = {
        (HANDLE)(unsigned long long)process_id, 0
    };
    HANDLE process = 0;
    long status = NtOpenProcess(&process, access, 0, &id);
    if (status < 0) {
        SetLastError(status == (long)0xC000000D ||
                     status == (long)0xC0000225 ? 87 : 6);
        return 0;
    }
    return process;
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

__declspec(dllexport) BOOL CreateProcessA(
    const char *application_name, char *command_line,
    LPVOID process_attributes, LPVOID thread_attributes,
    BOOL inherit_handles, DWORD creation_flags, LPVOID environment,
    const char *current_directory, LPVOID startup_info,
    PROCESS_INFORMATION *process_information)
{
    (void)process_attributes; (void)thread_attributes;
    (void)creation_flags;
    if (!application_name || !process_information) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    HANDLE process = 0;
    DWORD pid = 0;
    const char *command = command_line ? command_line : application_name;
    char inherited_directory[260];
    if (!current_directory) {
        unsigned short wide[260];
        DWORD length = GetCurrentDirectoryW(260, wide);
        if (!length || length >= 260) {
            SetLastError(87);
            return 0;
        }
        for (DWORD i = 0; i <= length; i++)
            inherited_directory[i] = wide[i] <= 0x7f ? (char)wide[i] : '?';
        current_directory = inherited_directory;
    }
    HANDLE input = 0, output = 0, error = 0;
    STARTUPINFOA_LOCAL *startup = (STARTUPINFOA_LOCAL *)startup_info;
    if (inherit_handles && startup &&
        (startup->flags & STARTF_USESTDHANDLES)) {
        input = startup->standard_input;
        output = startup->standard_output;
        error = startup->standard_error;
    }
    long status = NtCreateUserProcessSimple(application_name, command,
                                            &process, &pid,
                                            input, output, error,
                                            current_directory,
                                            (const char *)environment);
    if (status < 0) {
        SetLastError(status == (long)0xC0000034 ? 2 :
                     status == (long)0xC0000023 ? 122 :
                     status == (long)0xC0000017 ? 8 :
                     status == (long)0xC0000005 ? 998 : 193);
        return 0;
    }
    process_information->hProcess = process;
    process_information->hThread = 0;
    process_information->dwProcessId = pid;
    process_information->dwThreadId = 0;
    return 1;
}

__declspec(dllexport) BOOL CreateProcessW(
    const unsigned short *application_name, unsigned short *command_line,
    LPVOID process_attributes, LPVOID thread_attributes,
    BOOL inherit_handles, DWORD creation_flags, LPVOID environment,
    const unsigned short *current_directory, LPVOID startup_info,
    PROCESS_INFORMATION *process_information)
{
    char app[260], cmd[260];
    DWORD i = 0;
    if (!application_name) { SetLastError(87); return 0; }
    while (application_name[i] && i < 259) {
        app[i] = application_name[i] <= 0x7f ? (char)application_name[i] : '?';
        i++;
    }
    if (application_name[i]) { SetLastError(87); return 0; }
    app[i] = 0;
    i = 0;
    if (command_line) {
        while (command_line[i] && i < 259) {
            cmd[i] = command_line[i] <= 0x7f ? (char)command_line[i] : '?';
            i++;
        }
        if (command_line[i]) { SetLastError(87); return 0; }
    }
    cmd[i] = 0;
    char cwd[260];
    const char *ascii_cwd = 0;
    if (current_directory) {
        DWORD j = 0;
        while (current_directory[j] && j < sizeof(cwd) - 1) {
            cwd[j] = current_directory[j] <= 0x7f ?
                     (char)current_directory[j] : '?';
            j++;
        }
        if (current_directory[j]) { SetLastError(87); return 0; }
        cwd[j] = 0;
        ascii_cwd = cwd;
    }
    char ansi_environment[2048];
    if (environment && (creation_flags & 0x400u)) {
        const unsigned short *wide = (const unsigned short *)environment;
        DWORD used = 0;
        for (;;) {
            if (used + 2 >= sizeof(ansi_environment)) {
                SetLastError(87);
                return 0;
            }
            unsigned short ch = *wide++;
            ansi_environment[used++] = ch <= 0x7f ? (char)ch : '?';
            if (!ch && !*wide) {
                ansi_environment[used++] = 0;
                break;
            }
        }
        environment = ansi_environment;
    }
    return CreateProcessA(app, command_line ? cmd : app, process_attributes,
                          thread_attributes, inherit_handles, creation_flags,
                          environment, ascii_cwd, startup_info,
                          process_information);
}
