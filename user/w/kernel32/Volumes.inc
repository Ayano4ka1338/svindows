/* The boot FAT image is the sole mounted DOS volume, C:. No directory mount
 * points or volume GUID aliases are currently installed by the I/O manager. */
__declspec(dllexport) UINT GetDriveTypeW(const WCHAR *root)
{
    if (!root || (root[0]=='\\' && !root[1])) return 3; /* DRIVE_FIXED */
    if ((root[0]=='C' || root[0]=='c') && root[1]==':' &&
        root[2]=='\\' && !root[3]) return 3;
    return 1; /* DRIVE_NO_ROOT_DIR */
}
__declspec(dllexport) HANDLE FindFirstVolumeMountPointW(const WCHAR *root,
    WCHAR *name, DWORD capacity)
{
    if (!root || !name || !capacity) SetLastError(87);
    else if (GetDriveTypeW(root)!=3) SetLastError(3);
    else { name[0]=0; SetLastError(18); } /* ERROR_NO_MORE_FILES */
    return (HANDLE)(ULONGLONG)-1;
}
__declspec(dllexport) BOOL FindNextVolumeMountPointW(HANDLE enumeration,
    WCHAR *name, DWORD capacity)
{
    (void)enumeration; (void)name; (void)capacity;
    SetLastError(6); /* No successful enumeration handle can have been issued. */
    return 0;
}
__declspec(dllexport) BOOL FindVolumeMountPointClose(HANDLE enumeration)
{ (void)enumeration; SetLastError(6); return 0; }
__declspec(dllexport) BOOL GetVolumeNameForVolumeMountPointW(const WCHAR *root,
    WCHAR *volume, DWORD capacity)
{
    (void)root; (void)volume; (void)capacity;
    SetLastError(50);
    return 0;
}
__declspec(dllexport) BOOL DeleteVolumeMountPointW(const WCHAR *mount)
{
    (void)mount;
    SetLastError(4390); /* ERROR_NOT_A_REPARSE_POINT */
    return 0;
}
