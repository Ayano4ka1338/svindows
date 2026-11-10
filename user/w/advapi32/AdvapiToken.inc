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
