extern NTSTATUS NtQueryInformationFile(HANDLE, IO_STATUS_BLOCK *, void *, DWORD, DWORD);

__declspec(dllexport) BOOL GetFileInformationByHandleEx(HANDLE file, UINT kind,
    void *buffer, DWORD size)
{
    if (!buffer) { SetLastError(87); return 0; }
    DWORD native;
    switch (kind) {
    case 0: native=4; break;  /* FileBasicInfo */
    case 1: native=5; break;  /* FileStandardInfo */
    case 2: native=9; break;  /* FileNameInfo */
    case 9: native=35; break; /* FileAttributeTagInfo */
    case 18: native=48; break; /* FileNormalizedNameInfo */
    default: SetLastError(50); return 0;
    }
    IO_STATUS_BLOCK io={0};
    NTSTATUS status=NtQueryInformationFile(file,&io,buffer,size,native);
    if (!NT_SUCCESS(status)) { SetLastError(ntstatus_to_win32(status)); return 0; }
    return 1;
}
__declspec(dllexport) BOOL SetFileInformationByHandle(HANDLE file, UINT kind,
    void *buffer, DWORD size)
{
    if (!buffer) { SetLastError(87); return 0; }
    DWORD native;
    switch (kind) {
    case 3: native=10; break; /* FileRenameInfo */
    case 4: native=13; break; /* FileDispositionInfo */
    case 6: native=20; break; /* FileEndOfFileInfo */
    default: SetLastError(50); return 0;
    }
    IO_STATUS_BLOCK io={0};
    NTSTATUS status=NtSetInformationFile(file,&io,buffer,size,native);
    if (!NT_SUCCESS(status)) { SetLastError(ntstatus_to_win32(status)); return 0; }
    return 1;
}
__declspec(dllexport) BOOL SetFileAttributesW(const WCHAR *path, DWORD attributes)
{
    (void)attributes;
    if (GetFileAttributesW(path)==(DWORD)-1) return 0;
    SetLastError(50); /* FAT attribute mutation is not exposed by native I/O. */
    return 0;
}
__declspec(dllexport) BOOL SetFileTime(HANDLE file, const FILETIME *created,
    const FILETIME *accessed, const FILETIME *written)
{
    SVINDOWS_FILE_INFO info;
    NTSTATUS status=NtQueryFileInfo(file,&info);
    if (!NT_SUCCESS(status)) { SetLastError(ntstatus_to_win32(status)); return 0; }
    if (!created && !accessed && !written) return 1;
    SetLastError(50); return 0;
}
__declspec(dllexport) BOOL LocalFileTimeToFileTime(const FILETIME *local, FILETIME *utc)
{
    if (!local || !utc) { SetLastError(87); return 0; }
    *utc=*local; /* GetTimeZoneInformation currently reports UTC year-round. */
    return 1;
}
__declspec(dllexport) BOOL IsDBCSLeadByte(BYTE character)
{ return IsDBCSLeadByteEx(GetACP(),character); }

__declspec(dllexport) BOOL DeviceIoControl(HANDLE file, DWORD code, void *input,
    DWORD input_size, void *output, DWORD output_size, DWORD *returned, void *overlapped)
{
    (void)file; (void)code; (void)input; (void)input_size;
    (void)output; (void)output_size; (void)overlapped;
    if (returned) *returned=0;
    SetLastError(50); return 0; /* No user device-control dispatch service yet. */
}
/* No serial device handles are issued by the native I/O namespace. */
__declspec(dllexport) BOOL GetCommState(HANDLE file, void *state)
{ (void)file; (void)state; SetLastError(6); return 0; }
__declspec(dllexport) BOOL SetCommState(HANDLE file, const void *state)
{ (void)file; (void)state; SetLastError(6); return 0; }
__declspec(dllexport) BOOL GetCommTimeouts(HANDLE file, void *timeouts)
{ (void)file; (void)timeouts; SetLastError(6); return 0; }
__declspec(dllexport) BOOL SetCommTimeouts(HANDLE file, const void *timeouts)
{ (void)file; (void)timeouts; SetLastError(6); return 0; }
