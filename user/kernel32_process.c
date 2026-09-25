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
                                      HANDLE error);
extern void SetLastError(DWORD error);
extern long NtQueryInformationProcess(HANDLE process, DWORD info_class,
                                      void *info, DWORD length,
                                      DWORD *return_length);

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

__declspec(dllexport) BOOL CreateProcessA(
    const char *application_name, char *command_line,
    LPVOID process_attributes, LPVOID thread_attributes,
    BOOL inherit_handles, DWORD creation_flags, LPVOID environment,
    const char *current_directory, LPVOID startup_info,
    PROCESS_INFORMATION *process_information)
{
    (void)process_attributes; (void)thread_attributes;
    (void)inherit_handles; (void)creation_flags; (void)environment;
    (void)current_directory;
    if (!application_name || !process_information) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    HANDLE process = 0;
    DWORD pid = 0;
    const char *command = command_line ? command_line : application_name;
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
                                            input, output, error);
    if (status < 0) {
        SetLastError(status == (long)0xC0000034 ? 2 : 193);
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
    app[i] = 0;
    i = 0;
    if (command_line) {
        while (command_line[i] && i < 259) {
            cmd[i] = command_line[i] <= 0x7f ? (char)command_line[i] : '?';
            i++;
        }
    }
    cmd[i] = 0;
    (void)current_directory;
    return CreateProcessA(app, command_line ? cmd : app, process_attributes,
                          thread_attributes, inherit_handles, creation_flags,
                          environment, 0, startup_info, process_information);
}
