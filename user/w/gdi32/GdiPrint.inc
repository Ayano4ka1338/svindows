/* Display and memory DCs cannot own print jobs. A failed operation must
 * leave the caller free to continue editing or select another device. */
__declspec(dllimport) void RtlSetLastWin32Error(DWORD error);

static int print_dc_error(HDC dc)
{
    RtlSetLastWin32Error(object_type(dc) == SV_GDI_TYPE_DC ?
                        50 /* ERROR_NOT_SUPPORTED */ : 6 /* ERROR_INVALID_HANDLE */);
    return -1; /* SP_ERROR */
}

__declspec(dllexport) int StartDocW(HDC dc, const void *document)
{
    (void)document;
    return print_dc_error(dc);
}

__declspec(dllexport) int SetAbortProc(HDC dc, BOOL (*abort_proc)(HDC, int))
{
    (void)abort_proc;
    return print_dc_error(dc);
}

__declspec(dllexport) int StartPage(HDC dc) { return print_dc_error(dc); }
__declspec(dllexport) int EndPage(HDC dc) { return print_dc_error(dc); }
__declspec(dllexport) int EndDoc(HDC dc) { return print_dc_error(dc); }
__declspec(dllexport) int AbortDoc(HDC dc) { return print_dc_error(dc); }
