typedef unsigned short WCHAR;
typedef unsigned long DWORD;
__declspec(dllimport) void SetLastError(DWORD);
__declspec(dllexport) unsigned char GetUserNameExW(int format, WCHAR *name, DWORD *size)
{
    (void)format; (void)name;
    if (!size) { SetLastError(87); return 0; }
    /* There is no logon identity provider; ADVAPI32 also exposes no account name. */
    *size = 0;
    SetLastError(1312); /* ERROR_NO_SUCH_LOGON_SESSION */
    return 0;
}
