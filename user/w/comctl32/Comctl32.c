#include "Internal.h"

__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(HANDLE, DWORD, ULONG_PTR);
__declspec(dllimport) BOOL HeapFree(HANDLE, DWORD, void *);

void *SvComctlAlloc(ULONG_PTR bytes)
{ return HeapAlloc(GetProcessHeap(), 8, bytes ? bytes : 1); }
void SvComctlFree(void *memory)
{ if (memory) HeapFree(GetProcessHeap(), 0, memory); }

void SvComctlFill(HDC dc, const RECT *rect, COLORREF color)
{
    if (!dc || !rect || rect->right <= rect->left || rect->bottom <= rect->top) return;
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush) return;
    HGDIOBJ old = SelectObject(dc, brush);
    PatBlt(dc, rect->left, rect->top, rect->right - rect->left,
           rect->bottom - rect->top, PATCOPY);
    SelectObject(dc, old);
    DeleteObject(brush);
}

static BOOL progress_registered, status_registered, trackbar_registered, tab_registered, listview_registered, toolbar_registered;
COMCTL32_API BOOL InitCommonControlsEx(const INITCOMMONCONTROLSEX *controls)
{
    if (!controls || controls->dwSize != sizeof(*controls)) return FALSE;
    DWORD flags = controls->dwICC;
    if ((flags & ICC_PROGRESS_CLASS) && !progress_registered) {
        if (!SvRegisterProgress()) return FALSE;
        progress_registered = TRUE;
    }
    if ((flags & ICC_BAR_CLASSES) && !status_registered) {
        if (!SvRegisterStatus()) return FALSE;
        status_registered = TRUE;
    }
    if ((flags & ICC_BAR_CLASSES) && !trackbar_registered) {
        if (!SvRegisterTrackbar()) return FALSE;
        trackbar_registered = TRUE;
    }
    if ((flags & ICC_TAB_CLASSES) && !tab_registered) {
        if (!SvRegisterTab()) return FALSE;
        tab_registered = TRUE;
    }
    if ((flags & ICC_LISTVIEW_CLASSES) && !listview_registered) {
        if (!SvRegisterListView()) return FALSE;
        listview_registered = TRUE;
    }
    if ((flags & ICC_COOL_CLASSES) && !toolbar_registered) {
        if (!SvRegisterToolbar()) return FALSE;
        toolbar_registered = TRUE;
    }
    return TRUE;
}

COMCTL32_API void InitCommonControls(void)
{
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_COOL_CLASSES };
    (void)InitCommonControlsEx(&controls);
}

COMCTL32_API HWND CreateStatusWindowW(LONG style, const WCHAR *text,
                                      HWND parent, UINT id)
{
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_BAR_CLASSES };
    if (!parent || !InitCommonControlsEx(&controls)) return NULL;
    return CreateWindowExW(0, STATUSCLASSNAMEW, text ? text : L"",
                           WS_CHILD | WS_VISIBLE | (DWORD)style,
                           0, 0, 0, 0, parent, (HMENU)(UINT_PTR)id, NULL, NULL);
}
