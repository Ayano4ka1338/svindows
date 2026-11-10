/* Local SCM: services are configured under HKLM\SYSTEM\CurrentControlSet\Services.
 * A SERVICE_WIN32_OWN_PROCESS entry launches its configured executable.
 * Handles have generations; closing one cannot revive it through slot reuse. */
extern int CreateProcessW(const WCHAR *application, WCHAR *command,
                          void *process_attributes, void *thread_attributes,
                          int inherit, DWORD flags, void *environment,
                          const WCHAR *current_directory, void *startup,
                          void *process_info);
extern int GetExitCodeProcess(HANDLE process, DWORD *exit_code);
extern int CloseHandle(HANDLE handle);
extern DWORD GetLastError(void);

#define SCM_SLOTS 32
#define SCM_SERVICES 32
#define SCM_MANAGER 1u
#define SCM_SERVICE 2u
#define SCM_CONNECT 0x0001u
#define SERVICE_QUERY_STATUS 0x0004u
#define SERVICE_QUERY_CONFIG 0x0001u
#define SERVICE_START_ACCESS 0x0010u
#define SERVICE_STOPPED 1u
#define SERVICE_RUNNING 4u
#define SERVICE_WIN32_OWN_PROCESS 0x10u
#define ERROR_SERVICE_DOES_NOT_EXIST 1060u
#define ERROR_SERVICE_ALREADY_RUNNING 1056u
#define ERROR_SERVICE_DISABLED 1058u

typedef struct {
    HANDLE Process;
    WCHAR Name[80];
    DWORD Type;
    DWORD StartMode;
} SCM_SERVICE_RECORD;
typedef struct {
    DWORD Generation, Kind, Access, ServiceIndex;
} SCM_HANDLE_SLOT;
typedef struct {
    HANDLE Process, Thread;
    DWORD ProcessId, ThreadId;
} SCM_PROCESS_INFO;
typedef struct {
    DWORD ServiceType, CurrentState, ControlsAccepted, Win32ExitCode;
    DWORD ServiceSpecificExitCode, CheckPoint, WaitHint;
} SCM_STATUS;
typedef struct {
    DWORD ServiceType, StartType, ErrorControl;
    WCHAR *BinaryPathName, *LoadOrderGroup;
    DWORD *TagId;
    WCHAR *Dependencies, *ServiceStartName, *DisplayName;
} SCM_QUERY_CONFIG;

static SCM_SERVICE_RECORD g_scm_services[SCM_SERVICES];
static SCM_HANDLE_SLOT g_scm_handles[SCM_SLOTS];
static volatile DWORD g_scm_lock;

static void scm_lock(void)
{
    while (__atomic_exchange_n(&g_scm_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static void scm_unlock(void)
{ __atomic_store_n(&g_scm_lock, 0, __ATOMIC_RELEASE); }

static int scm_name_equal(const WCHAR *a, const WCHAR *b)
{
    for (;;) {
        WCHAR x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static SCM_HANDLE_SLOT *scm_slot(HANDLE handle, DWORD kind)
{
    ULONGLONG bits = (ULONGLONG)handle;
    DWORD index = (DWORD)bits;
    if (!index || index > SCM_SLOTS) return 0;
    SCM_HANDLE_SLOT *slot = &g_scm_handles[index - 1];
    return slot->Kind == kind && slot->Generation == (DWORD)(bits >> 32)
        ? slot : 0;
}

static HANDLE scm_new_handle(DWORD kind, DWORD access, DWORD service_index)
{
    for (DWORD i = 0; i < SCM_SLOTS; i++) {
        SCM_HANDLE_SLOT *slot = &g_scm_handles[i];
        if (slot->Kind) continue;
        if (!++slot->Generation) ++slot->Generation;
        slot->Access = access;
        slot->ServiceIndex = service_index;
        slot->Kind = kind;
        return (HANDLE)(((ULONGLONG)slot->Generation << 32) | (i + 1));
    }
    return 0;
}

static HKEY scm_open_config(const WCHAR *name)
{
    static const WCHAR prefix[] = {
        'S','Y','S','T','E','M','\\','C','u','r','r','e','n','t','C','o','n','t',
        'r','o','l','S','e','t','\\','S','e','r','v','i','c','e','s','\\',0
    };
    WCHAR path[128];
    DWORD i = 0, j = 0;
    while (prefix[i]) { path[i] = prefix[i]; i++; }
    while (name[j] && i + 1 < 128) path[i++] = name[j++];
    if (name[j] || !j) return 0;
    path[i] = 0;
    HKEY key = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key)) return 0;
    return key;
}

static int scm_read_dword(HKEY key, const WCHAR *name, DWORD *out)
{
    DWORD type = 0, size = sizeof(*out);
    return RegQueryValueExW(key, name, 0, &type, (BYTE *)out, &size) == 0 &&
           type == 4 && size == sizeof(*out);
}

__declspec(dllexport) HANDLE OpenSCManagerW(const WCHAR *machine,
                                              const WCHAR *database,
                                              DWORD access)
{
    if (machine && machine[0]) { SetLastError(53); return 0; }
    static const WCHAR active[] = {
        'S','e','r','v','i','c','e','s','A','c','t','i','v','e',0
    };
    if (database && !scm_name_equal(database, active)) {
        SetLastError(1065); return 0;
    }
    scm_lock();
    HANDLE result = scm_new_handle(SCM_MANAGER, access, 0);
    scm_unlock();
    if (!result) SetLastError(8);
    return result;
}

__declspec(dllexport) HANDLE OpenServiceW(HANDLE manager,
                                           const WCHAR *name, DWORD access)
{
    if (!name || !name[0]) { SetLastError(123); return 0; }
    scm_lock();
    SCM_HANDLE_SLOT *parent = scm_slot(manager, SCM_MANAGER);
    if (!parent) { scm_unlock(); SetLastError(6); return 0; }
    if (!(parent->Access & SCM_CONNECT)) {
        scm_unlock(); SetLastError(5); return 0;
    }
    scm_unlock();
    HKEY config = scm_open_config(name);
    if (!config) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return 0; }
    DWORD type = 0, start = 3;
    int valid = scm_read_dword(config, L"Type", &type);
    scm_read_dword(config, L"Start", &start);
    RegCloseKey(config);
    if (!valid) { SetLastError(13); return 0; }
    scm_lock();
    DWORD index;
    for (index = 0; index < SCM_SERVICES; index++) {
        if (g_scm_services[index].Name[0] &&
            scm_name_equal(g_scm_services[index].Name, name)) break;
    }
    if (index == SCM_SERVICES) {
        for (index = 0; index < SCM_SERVICES; index++)
            if (!g_scm_services[index].Name[0]) break;
        if (index < SCM_SERVICES) {
            DWORD i = 0;
            while (name[i] && i < 79) {
                g_scm_services[index].Name[i] = name[i]; i++;
            }
            g_scm_services[index].Name[i] = 0;
        }
    }
    HANDLE result = 0;
    if (index < SCM_SERVICES) {
        g_scm_services[index].Type = type;
        g_scm_services[index].StartMode = start;
        result = scm_new_handle(SCM_SERVICE, access, index);
    }
    scm_unlock();
    if (!result) SetLastError(8);
    return result;
}

__declspec(dllexport) int CloseServiceHandle(HANDLE handle)
{
    scm_lock();
    SCM_HANDLE_SLOT *slot = scm_slot(handle, SCM_MANAGER);
    if (!slot) slot = scm_slot(handle, SCM_SERVICE);
    if (!slot) { scm_unlock(); SetLastError(6); return 0; }
    slot->Kind = 0;
    scm_unlock();
    return 1;
}

static DWORD scm_config_string(HKEY key, const WCHAR *value_name,
                               WCHAR *buffer, DWORD capacity)
{
    DWORD type = 0, bytes = capacity * sizeof(WCHAR);
    LONG status = RegQueryValueExW(key, value_name, 0, &type,
                                   (BYTE *)buffer, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) { buffer[0] = 0; return 1; }
    if (status || (type != 1 && type != 2 && type != 7) ||
        bytes < sizeof(WCHAR) ||
        bytes > capacity * sizeof(WCHAR)) return 0;
    buffer[capacity - 1] = 0;
    return 1;
}

static DWORD scm_wide_length(const WCHAR *text)
{
    DWORD n = 0;
    while (text[n]) n++;
    return n;
}

static WCHAR *scm_pack_string(WCHAR **cursor, const WCHAR *source)
{
    WCHAR *result = *cursor;
    DWORD n = scm_wide_length(source) + 1;
    for (DWORD i = 0; i < n; i++) result[i] = source[i];
    *cursor += n;
    return result;
}

__declspec(dllexport) int QueryServiceConfigW(HANDLE handle,
        SCM_QUERY_CONFIG *config, DWORD capacity, DWORD *needed)
{
    scm_lock();
    SCM_HANDLE_SLOT *slot = scm_slot(handle, SCM_SERVICE);
    DWORD access = slot ? slot->Access : 0;
    DWORD service_index = slot ? slot->ServiceIndex : 0;
    WCHAR service_name[80] = {0};
    if (slot) {
        for (DWORD i = 0; i < 80; i++) {
            service_name[i] = g_scm_services[service_index].Name[i];
            if (!service_name[i]) break;
        }
    }
    scm_unlock();
    if (!slot) { SetLastError(6); return 0; }
    if (!(access & SERVICE_QUERY_CONFIG)) { SetLastError(5); return 0; }
    HKEY key = scm_open_config(service_name);
    if (!key) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return 0; }

    WCHAR binary[260] = {0}, group[128] = {0}, dependencies[256] = {0};
    WCHAR account[128] = { 'L','o','c','a','l','S','y','s','t','e','m',0 };
    WCHAR display[128] = {0};
    DWORD type = 0, start = 3, error_control = 1;
    int valid = scm_read_dword(key, L"Type", &type);
    scm_read_dword(key, L"Start", &start);
    scm_read_dword(key, L"ErrorControl", &error_control);
    valid = valid && scm_config_string(key, L"ImagePath", binary, 260) &&
        scm_config_string(key, L"Group", group, 128) &&
        scm_config_string(key, L"DependOnService", dependencies, 256) &&
        scm_config_string(key, L"ObjectName", account, 128) &&
        scm_config_string(key, L"DisplayName", display, 128);
    RegCloseKey(key);
    if (!valid) { SetLastError(13); return 0; }

    DWORD dep_units = dependencies[0] ? 1 : 2;
    if (dependencies[0]) {
        while (dependencies[dep_units]) dep_units++;
        dep_units++;
    }
    DWORD units = (DWORD)(sizeof(SCM_QUERY_CONFIG) / sizeof(WCHAR)) +
        scm_wide_length(binary) + 1 + scm_wide_length(group) + 1 + dep_units +
        scm_wide_length(account) + 1 + scm_wide_length(display) + 1;
    DWORD required = units * sizeof(WCHAR);
    if (needed) *needed = required;
    if (!config || capacity < required) {
        SetLastError(122);
        return 0;
    }
    config->ServiceType = type;
    config->StartType = start;
    config->ErrorControl = error_control;
    config->TagId = 0;
    WCHAR *cursor = (WCHAR *)((BYTE *)config + sizeof(*config));
    config->BinaryPathName = scm_pack_string(&cursor, binary);
    config->LoadOrderGroup = scm_pack_string(&cursor, group);
    config->Dependencies = cursor;
    if (dependencies[0]) {
        for (DWORD i = 0; i < dep_units; i++) cursor[i] = dependencies[i];
        cursor += dep_units;
    } else { *cursor++ = 0; *cursor++ = 0; }
    config->ServiceStartName = scm_pack_string(&cursor, account);
    config->DisplayName = scm_pack_string(&cursor, display);
    return 1;
}

__declspec(dllexport) int QueryServiceStatus(HANDLE handle,
                                              SCM_STATUS *status)
{
    if (!status) { SetLastError(87); return 0; }
    scm_lock();
    SCM_HANDLE_SLOT *slot = scm_slot(handle, SCM_SERVICE);
    if (!slot) { scm_unlock(); SetLastError(6); return 0; }
    if (!(slot->Access & SERVICE_QUERY_STATUS)) {
        scm_unlock(); SetLastError(5); return 0;
    }
    SCM_SERVICE_RECORD *service = &g_scm_services[slot->ServiceIndex];
    HANDLE process = service->Process;
    DWORD type = service->Type;
    scm_unlock();
    DWORD exit_code = 0;
    int alive = process && GetExitCodeProcess(process, &exit_code) &&
                exit_code == 259; /* STILL_ACTIVE */
    status->ServiceType = type;
    status->CurrentState = alive ? SERVICE_RUNNING : SERVICE_STOPPED;
    status->ControlsAccepted = 0;
    status->Win32ExitCode = alive ? 0 : exit_code;
    status->ServiceSpecificExitCode = 0;
    status->CheckPoint = status->WaitHint = 0;
    return 1;
}

__declspec(dllexport) int StartServiceW(HANDLE handle, DWORD argc,
                                         const WCHAR *const *argv)
{
    if (argc && !argv) { SetLastError(87); return 0; }
    if (argc) { SetLastError(87); return 0; } /* argument transport pending */
    scm_lock();
    SCM_HANDLE_SLOT *slot = scm_slot(handle, SCM_SERVICE);
    if (!slot) { scm_unlock(); SetLastError(6); return 0; }
    if (!(slot->Access & SERVICE_START_ACCESS)) {
        scm_unlock(); SetLastError(5); return 0;
    }
    SCM_SERVICE_RECORD *service = &g_scm_services[slot->ServiceIndex];
    DWORD type = service->Type, start = service->StartMode;
    WCHAR name[80];
    DWORD i = 0;
    while ((name[i] = service->Name[i]) != 0 && i < 79) i++;
    HANDLE previous = service->Process;
    scm_unlock();
    if (type != SERVICE_WIN32_OWN_PROCESS) {
        SetLastError(50); return 0;
    }
    if (start == 4) { SetLastError(ERROR_SERVICE_DISABLED); return 0; }
    DWORD exit_code = 0;
    if (previous && GetExitCodeProcess(previous, &exit_code) &&
        exit_code == 259) {
        SetLastError(ERROR_SERVICE_ALREADY_RUNNING); return 0;
    }
    HKEY config = scm_open_config(name);
    if (!config) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return 0; }
    WCHAR image[260];
    DWORD image_type = 0, image_bytes = sizeof(image);
    LONG st = RegQueryValueExW(config, L"ImagePath", 0, &image_type,
                               (BYTE *)image, &image_bytes);
    RegCloseKey(config);
    if (st || image_type != 1 || image_bytes < 2 ||
        image_bytes > sizeof(image)) {
        SetLastError(2); return 0;
    }
    image[259] = 0;
    SCM_PROCESS_INFO child = {0};
    if (!CreateProcessW(image, image, 0, 0, 0, 0, 0, 0, 0, &child))
        return 0;
    scm_lock();
    service->Process = child.Process;
    scm_unlock();
    if (previous) CloseHandle(previous);
    if (child.Thread) CloseHandle(child.Thread);
    return 1;
}

__declspec(dllexport) BOOL_K ControlService(HANDLE handle, DWORD control,
    SCM_STATUS *status)
{
    if (!status) { SetLastError(87); return FALSE_K; }
    scm_lock();
    SCM_HANDLE_SLOT *slot=scm_slot(handle,SCM_SERVICE);
    scm_unlock();
    if (!slot) { SetLastError(6); return FALSE_K; }
    (void)control;
    /* A child executable is not a service-control dispatcher. */
    SetLastError(1061); /* ERROR_SERVICE_CANNOT_ACCEPT_CTRL */
    return FALSE_K;
}
