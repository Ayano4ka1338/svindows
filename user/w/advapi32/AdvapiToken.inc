/* Win32 token access backed by the kernel object manager. */
extern NTSTATUS NtOpenProcessToken(HANDLE process, DWORD access, HANDLE *out);
extern NTSTATUS NtOpenThreadToken(HANDLE thread, DWORD access, BYTE as_self,
                                 HANDLE *out);
extern NTSTATUS NtQueryInformationToken(HANDLE token, DWORD cls, void *buffer,
                                        DWORD length, DWORD *required);
extern DWORD RtlNtStatusToDosError(NTSTATUS status);

typedef struct { void *Sid; DWORD Attributes; DWORD Padding; } TOKEN_SID_ATTR;

static BOOL_K token_result(NTSTATUS status)
{
    if (NT_SUCCESS(status)) return TRUE_K;
    SetLastError(RtlNtStatusToDosError(status));
    return FALSE_K;
}

__declspec(dllexport) BOOL_K OpenProcessToken(HANDLE process,
                                              DWORD desired_access,
                                              HANDLE *token_handle)
{
    if (!token_handle) {
        SetLastError(87);
        return FALSE_K;
    }
    return token_result(NtOpenProcessToken(process, desired_access,
                                          token_handle));
}

__declspec(dllexport) BOOL_K OpenThreadToken(HANDLE thread,
                                             DWORD desired_access,
                                             BOOL_K open_as_self,
                                             HANDLE *token_handle)
{
    if (!token_handle) {
        SetLastError(87);
        return FALSE_K;
    }
    return token_result(NtOpenThreadToken(thread, desired_access,
                                         (BYTE)open_as_self, token_handle));
}

__declspec(dllexport) BOOL_K GetTokenInformation(HANDLE token, DWORD cls,
                                                  void *buffer, DWORD length,
                                                  DWORD *required)
{
    if (!required) {
        SetLastError(87);
        return FALSE_K;
    }
    return token_result(NtQueryInformationToken(token, cls, buffer,
                                                length, required));
}

__declspec(dllexport) BOOL_K CheckTokenMembership(void *token, void *sid,
                                                  BOOL_K *is_member)
{
    if (!sid_valid(sid) || !is_member) {
        SetLastError(87);
        return FALSE_K;
    }
    *is_member = FALSE_K;
    HANDLE own = 0;
    if (!token) {
        if (!OpenProcessToken((HANDLE)-1, 0x0008, &own)) return FALSE_K;
        token = own;
    }
    BYTE buffer[256];
    DWORD required = 0;
    BOOL_K ok = GetTokenInformation(token, 2, buffer, sizeof(buffer), &required);
    if (ok) {
        DWORD count = *(DWORD *)buffer;
        TOKEN_SID_ATTR *groups = (TOKEN_SID_ATTR *)(buffer + 8);
        for (DWORD i = 0; i < count; i++) {
            if ((groups[i].Attributes & 4) && EqualSid(groups[i].Sid, sid)) {
                *is_member = TRUE_K;
                break;
            }
        }
    }
    if (own) NtClose(own);
    return ok;
}

typedef struct { DWORD LowPart; LONG HighPart; } TOKEN_LUID;

extern int CompareStringOrdinal(const WCHAR *, int, const WCHAR *, int, BOOL_K);
extern BOOL_K GetComputerNameW(WCHAR *buffer, DWORD *length);
extern NTSTATUS NtAdjustPrivilegesToken(HANDLE token, BYTE disable_all,
    const void *state, DWORD length, void *previous, DWORD *required);

static BOOL_K advapi_local_system(const WCHAR *system)
{
    if (!system || !*system) return TRUE_K;
    if (system[0] == '\\' && system[1] == '\\') system += 2;
    if (CompareStringOrdinal(system, -1, L".", -1, TRUE_K) == 2) return TRUE_K;

    WCHAR local[16];
    DWORD length = sizeof(local) / sizeof(local[0]);
    return GetComputerNameW(local, &length) &&
           CompareStringOrdinal(system, -1, local, -1, TRUE_K) == 2;
}

__declspec(dllexport) BOOL_K LookupPrivilegeValueW(const WCHAR *system,
    const WCHAR *name, TOKEN_LUID *luid)
{
    static const struct { const WCHAR *Name; DWORD Id; } privileges[] = {
        { L"SeSecurityPrivilege", 8 },
        { L"SeBackupPrivilege", 17 },
        { L"SeRestorePrivilege", 18 }
    };
    if (!name || !luid) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE_K;
    }
    if (!advapi_local_system(system)) {
        SetLastError(1722); /* RPC_S_SERVER_UNAVAILABLE */
        return FALSE_K;
    }
    for (unsigned i = 0; i < sizeof(privileges) / sizeof(privileges[0]); i++) {
        if (CompareStringOrdinal(name, -1, privileges[i].Name, -1, TRUE_K) == 2) {
            *luid = (TOKEN_LUID){privileges[i].Id, 0};
            return TRUE_K;
        }
    }
    SetLastError(1313); /* ERROR_NO_SUCH_PRIVILEGE */
    return FALSE_K;
}

__declspec(dllexport) BOOL_K AdjustTokenPrivileges(HANDLE token, BOOL_K disable_all,
    const void *state, DWORD length, void *previous, DWORD *required)
{
    if ((!disable_all && !state) || (previous && !required)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE_K;
    }
    NTSTATUS status = NtAdjustPrivilegesToken(token, disable_all != 0,
        state, length, previous, required);
    if (!token_result(status)) return FALSE_K;
    SetLastError(status == 0x106 ? 1300 /* ERROR_NOT_ALL_ASSIGNED */ : 0);
    return TRUE_K;
}

/* Well-known accounts used in local tokens and registry descriptors. */
__declspec(dllexport) BOOL_K LookupAccountSidW(const WCHAR *system, const void *sid,
    WCHAR *name, DWORD *name_size, WCHAR *domain, DWORD *domain_size, DWORD *use)
{
    static const struct {
        DWORD Authority, First, Second;
        BYTE Count;
        DWORD Use;
        const WCHAR *Name, *Domain;
    } accounts[] = {
        {1, 0, 0, 1, 5, L"Everyone", L""},
        {5, 4, 0, 1, 5, L"INTERACTIVE", L"NT AUTHORITY"},
        {5, 11, 0, 1, 5, L"Authenticated Users", L"NT AUTHORITY"},
        {5, 18, 0, 1, 5, L"SYSTEM", L"NT AUTHORITY"},
        {5, 19, 0, 1, 5, L"LOCAL SERVICE", L"NT AUTHORITY"},
        {5, 20, 0, 1, 5, L"NETWORK SERVICE", L"NT AUTHORITY"},
        {5, 32, 544, 2, 4, L"Administrators", L"BUILTIN"},
        {5, 32, 545, 2, 4, L"Users", L"BUILTIN"},
        {5, 32, 546, 2, 4, L"Guests", L"BUILTIN"}
    };
    if (!name_size || !domain_size || !use) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE_K;
    }
    if (!sid_valid(sid)) {
        SetLastError(1337); /* ERROR_INVALID_SID */
        return FALSE_K;
    }
    if (!advapi_local_system(system)) {
        SetLastError(1722); /* RPC_S_SERVER_UNAVAILABLE */
        return FALSE_K;
    }

    const BYTE *raw = sid;
    for (unsigned i = 0; i < sizeof(accounts) / sizeof(accounts[0]); i++) {
        if (raw[1] != accounts[i].Count || sid_authority(raw) != accounts[i].Authority ||
            sid_subauthority(raw, 0) != accounts[i].First ||
            (raw[1] == 2 && sid_subauthority(raw, 1) != accounts[i].Second))
            continue;
        DWORD need_name = (DWORD)wide_len(accounts[i].Name) + 1;
        DWORD need_domain = (DWORD)wide_len(accounts[i].Domain) + 1;
        if (!name || *name_size < need_name || !domain || *domain_size < need_domain) {
            *name_size = need_name;
            *domain_size = need_domain;
            SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
            return FALSE_K;
        }
        sid_copy(name, accounts[i].Name, need_name * sizeof(*name));
        sid_copy(domain, accounts[i].Domain, need_domain * sizeof(*domain));
        *name_size = need_name - 1;
        *domain_size = need_domain - 1;
        *use = accounts[i].Use;
        return TRUE_K;
    }
    SetLastError(1332); /* ERROR_NONE_MAPPED */
    return FALSE_K;
}

/* Native threads currently have no impersonation-token state. */
__declspec(dllexport) BOOL_K ImpersonateLoggedOnUser(HANDLE token)
{ (void)token; SetLastError(50); return FALSE_K; }
__declspec(dllexport) BOOL_K RevertToSelf(void)
{ return TRUE_K; }
__declspec(dllexport) BOOL_K DuplicateTokenEx(HANDLE token, DWORD access,
    const void *security, int level, int type, HANDLE *copy)
{
    (void)token; (void)access; (void)security;
    if (!copy || level<0 || level>3 || (type!=1 && type!=2)) {
        SetLastError(87); return FALSE_K;
    }
    *copy=0;
    SetLastError(50); return FALSE_K;
}
__declspec(dllexport) BOOL_K SetTokenInformation(HANDLE token, DWORD information,
    const void *buffer, DWORD length)
{
    (void)token; (void)information; (void)buffer; (void)length;
    SetLastError(50); return FALSE_K;
}
