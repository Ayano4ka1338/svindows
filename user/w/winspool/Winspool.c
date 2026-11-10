/* No printers or print service are installed on Svindows. */
#include <stddef.h>

typedef int BOOL;
typedef unsigned long DWORD;
typedef unsigned char BYTE;
typedef wchar_t WCHAR;
typedef void *HANDLE;

__declspec(dllimport) void RtlSetLastWin32Error(DWORD error);

__declspec(dllexport) BOOL OpenPrinterW(const WCHAR *name, HANDLE *printer,
                                        const void *defaults)
{
    (void)defaults;
    if (!printer) {
        RtlSetLastWin32Error(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    *printer = NULL;
    RtlSetLastWin32Error(name && *name ? 1801 /* ERROR_INVALID_PRINTER_NAME */ :
                                        1722 /* RPC_S_SERVER_UNAVAILABLE */);
    return 0;
}

__declspec(dllexport) BOOL ClosePrinter(HANDLE printer)
{
    (void)printer;
    RtlSetLastWin32Error(6); /* ERROR_INVALID_HANDLE */
    return 0;
}

__declspec(dllexport) BOOL GetPrinterDriverW(HANDLE printer, const WCHAR *environment,
    DWORD level, BYTE *buffer, DWORD size, DWORD *required)
{
    (void)environment;
    (void)level;
    (void)buffer;
    (void)size;
    if (required) *required = 0;
    return ClosePrinter(printer);
}
