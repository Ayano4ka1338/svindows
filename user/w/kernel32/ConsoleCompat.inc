/* The console currently has one fixed OEM code page and a fixed raster. */
__declspec(dllexport) UINT GetConsoleCP(void) { return 437; }
__declspec(dllexport) BOOL SetConsoleCP(UINT codepage)
{
    if (codepage==437) return 1;
    SetLastError(50); return 0;
}
__declspec(dllexport) BOOL SetConsoleOutputCP(UINT codepage)
{ return SetConsoleCP(codepage); }
extern NTSTATUS RtlUnicodeToOemN(char *, DWORD, DWORD *, const WCHAR *, DWORD);
__declspec(dllexport) BOOL FillConsoleOutputCharacterW(HANDLE console,
    WCHAR character, DWORD count, COORD origin, DWORD *written)
{
    char byte='?'; DWORD converted=0;
    NTSTATUS status=RtlUnicodeToOemN(&byte,1,&converted,&character,sizeof(character));
    if (!NT_SUCCESS(status)) {
        if (written) *written=0;
        SetLastError(ntstatus_to_win32(status)); return 0;
    }
    return FillConsoleOutputCharacterA(console,byte,count,origin,written);
}
__declspec(dllexport) COORD GetLargestConsoleWindowSize(HANDLE console)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(console,&info)) return (COORD){0,0};
    return info.MaximumWindowSize;
}
__declspec(dllexport) BOOL SetConsoleScreenBufferSize(HANDLE console, COORD size)
{ (void)console; (void)size; SetLastError(50); return 0; }
__declspec(dllexport) BOOL SetConsoleWindowInfo(HANDLE console, BOOL absolute,
    const SMALL_RECT *window)
{ (void)console; (void)absolute; (void)window; SetLastError(50); return 0; }
__declspec(dllexport) BOOL FlushConsoleInputBuffer(HANDLE console)
{ (void)console; SetLastError(50); return 0; }
__declspec(dllexport) BOOL PeekConsoleInputW(HANDLE console, void *records,
    DWORD capacity, DWORD *read)
{
    (void)console; (void)records; (void)capacity;
    if (read) *read=0;
    SetLastError(50); return 0;
}
__declspec(dllexport) BOOL GetNumberOfConsoleInputEvents(HANDLE console, DWORD *count)
{
    (void)console;
    if (count) *count=0;
    SetLastError(50); return 0;
}
__declspec(dllexport) BOOL SetConsoleCtrlHandler(BOOL (*handler)(DWORD), BOOL add)
{ (void)handler; (void)add; SetLastError(50); return 0; }
