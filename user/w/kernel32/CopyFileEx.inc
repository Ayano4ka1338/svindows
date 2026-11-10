typedef DWORD (*COPY_PROGRESS)(long long, long long, long long, long long,
    DWORD, DWORD, HANDLE, HANDLE, void *);

__declspec(dllexport) BOOL CopyFileExW(const WCHAR *from, const WCHAR *to,
    COPY_PROGRESS progress, void *context, BOOL *cancel, DWORD flags)
{
    if (flags & ~1u) { SetLastError(50); return 0; }
    if (cancel && *cancel) { SetLastError(1235); return 0; }
    HANDLE source=CreateFileW(from,0x80000000u,1,0,3,0,0);
    if (source==NT_INVALID_HANDLE) return 0;
    SVINDOWS_FILE_INFO info;
    NTSTATUS status=NtQueryFileInfo(source,&info);
    if (!NT_SUCCESS(status)) {
        CloseHandle(source); SetLastError(ntstatus_to_win32(status)); return 0;
    }
    HANDLE destination=CreateFileW(to,0x40000000u,0,0,(flags&1)?1:2,0,0);
    if (destination==NT_INVALID_HANDLE) {
        DWORD error=GetLastError(); CloseHandle(source); SetLastError(error); return 0;
    }
    BYTE *buffer=HeapAlloc(GetProcessHeap(),0,65536);
    DWORD error=buffer ? 0 : 8;
    ULONGLONG transferred=0;
    BOOL keep=0;
    DWORD reason=1; /* CALLBACK_STREAM_SWITCH, then CALLBACK_CHUNK_FINISHED */
    while (!error) {
        if (cancel && *cancel) { error=1235; break; }
        if (progress) {
            DWORD action=progress(info.Size,transferred,info.Size,transferred,
                                  1,reason,source,destination,context);
            if (action==1 || action==2) { keep=action==2; error=1235; break; }
            if (action==3) progress=0; /* PROGRESS_QUIET */
        }
        if (transferred==info.Size) break;
        DWORD count=info.Size-transferred>65536 ? 65536 : (DWORD)(info.Size-transferred);
        DWORD read=0,written=0;
        if (!ReadFile(source,buffer,count,&read,0)) { error=GetLastError(); break; }
        if (!read) { error=38; break; }
        if (!WriteFile(destination,buffer,read,&written,0)) { error=GetLastError(); break; }
        if (written!=read) { error=29; break; }
        transferred+=read;
        reason=0;
    }
    if (buffer) HeapFree(GetProcessHeap(),0,buffer);
    CloseHandle(destination); CloseHandle(source);
    if (error) {
        if (!keep) DeleteFileW(to);
        SetLastError(error); return 0;
    }
    return 1;
}
__declspec(dllexport) BOOL PrivCopyFileExW(const WCHAR *from, const WCHAR *to,
    COPY_PROGRESS progress, void *context, BOOL *cancel, DWORD flags)
{
    return CopyFileExW(from,to,progress,context,cancel,flags);
}
