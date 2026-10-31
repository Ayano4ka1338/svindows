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
