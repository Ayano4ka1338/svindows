#ifndef SVINDOWS_COMCTL32_H
#define SVINDOWS_COMCTL32_H
#include <User32.h>

#ifdef SV_COMCTL32_BUILD
#define COMCTL32_API __declspec(dllexport)
#else
#define COMCTL32_API __declspec(dllimport)
#endif

#define STATUSCLASSNAMEW L"msctls_statusbar32"
#define PROGRESS_CLASSW  L"msctls_progress32"
#define TRACKBAR_CLASSW  L"msctls_trackbar32"
#define WC_TABCONTROLW   L"SysTabControl32"
#define WC_LISTVIEWW     L"SysListView32"
#define TOOLBARCLASSNAMEW L"ToolbarWindow32"
#define ICC_PROGRESS_CLASS 0x00002000u
#define ICC_BAR_CLASSES    0x00000004u
#define ICC_TAB_CLASSES    0x00000008u
#define ICC_LISTVIEW_CLASSES 0x00000001u
#define ICC_COOL_CLASSES 0x00000400u
typedef struct _INITCOMMONCONTROLSEX { DWORD dwSize, dwICC; } INITCOMMONCONTROLSEX;

#define WM_USER         0x0400u
#define PBM_SETPOS      (WM_USER + 2)
#define PBM_SETSTEP     (WM_USER + 4)
#define PBM_STEPIT      (WM_USER + 5)
#define PBM_SETRANGE32  (WM_USER + 6)
#define PBM_GETPOS      (WM_USER + 8)
#define SB_SETTEXTW     (WM_USER + 11)
#define SB_SETPARTS     (WM_USER + 4)
#define SB_SIMPLE       (WM_USER + 9)
#define SBT_NOBORDERS   0x0100u
#define SBT_OWNERDRAW   0x1000u
#define WM_NOTIFY      0x004Eu
#define WM_HSCROLL     0x0114u
#define WM_VSCROLL     0x0115u
#define TBM_GETPOS     (WM_USER + 0)
#define TBM_GETRANGEMIN (WM_USER + 1)
#define TBM_GETRANGEMAX (WM_USER + 2)
#define TBM_SETPOS     (WM_USER + 5)
#define TBM_SETRANGEMIN (WM_USER + 7)
#define TBM_SETRANGEMAX (WM_USER + 8)
#define TBM_SETRANGE   (WM_USER + 6)
#define TBM_SETTICFREQ (WM_USER + 20)
#define TBS_AUTOTICKS  0x0001u
#define TBS_VERT       0x0002u
#define TBS_NOTICKS    0x0010u
#define TBS_TOP        0x0004u
#define TBS_LEFT       0x0004u
#define TB_LINEUP      0
#define TB_LINEDOWN    1
#define TB_PAGEUP      2
#define TB_PAGEDOWN    3
#define TB_THUMBTRACK  5
#define TB_THUMBPOSITION 4
#define TB_BUTTONSTRUCTSIZE (WM_USER + 30)
#define TB_ADDBUTTONSW     (WM_USER + 68)
#define TB_INSERTBUTTONW   (WM_USER + 67)
#define TB_DELETEBUTTON    (WM_USER + 22)
#define TB_GETBUTTON       (WM_USER + 23)
#define TB_BUTTONCOUNT     (WM_USER + 24)
#define TB_GETSTATE        (WM_USER + 18)
#define TB_SETSTATE        (WM_USER + 17)
#define TB_AUTOSIZE        (WM_USER + 33)
#define TB_ADDSTRINGW      (WM_USER + 77)
#define TBSTYLE_BUTTON     0x0000u
#define TBSTYLE_SEP        0x0001u
#define TBSTYLE_CHECK      0x0002u
#define TBSTYLE_FLAT       0x0800u
#define TBSTATE_CHECKED    0x01u
#define TBSTATE_ENABLED    0x04u
#define TBSTATE_PRESSED    0x02u
#define TBSTATE_HIDDEN     0x08u
#define I_IMAGENONE        (-2)
#define ID_SEPARATOR       0
#define TCM_FIRST      0x1300u
#define TCM_GETITEMCOUNT (TCM_FIRST + 4)
#define TCM_GETCURSEL (TCM_FIRST + 11)
#define TCM_SETCURSEL (TCM_FIRST + 12)
#define TCM_ADJUSTRECT (TCM_FIRST + 40)
#define TCM_INSERTITEMW (TCM_FIRST + 62)
#define TCM_DELETEITEM (TCM_FIRST + 8)
#define TCM_DELETEALLITEMS (TCM_FIRST + 9)
#define TCIF_TEXT      0x0001u
#define TCN_FIRST      ((UINT)-550)
#define TCN_SELCHANGE  (TCN_FIRST - 1)
#define LVM_FIRST 0x1000u
#define LVM_GETITEMCOUNT (LVM_FIRST + 4)
#define LVM_DELETEITEM (LVM_FIRST + 8)
#define LVM_DELETEALLITEMS (LVM_FIRST + 9)
#define LVM_GETNEXTITEM (LVM_FIRST + 12)
#define LVM_ENSUREVISIBLE (LVM_FIRST + 19)
#define LVM_GETITEMSTATE (LVM_FIRST + 44)
#define LVM_SETITEMSTATE (LVM_FIRST + 43)
#define LVM_GETSELECTEDCOUNT (LVM_FIRST + 50)
#define LVM_SETEXTENDEDLISTVIEWSTYLE (LVM_FIRST + 54)
#define LVM_GETEXTENDEDLISTVIEWSTYLE (LVM_FIRST + 55)
#define LVM_GETITEMW (LVM_FIRST + 75)
#define LVM_SETITEMW (LVM_FIRST + 76)
#define LVM_INSERTITEMW (LVM_FIRST + 77)
#define LVM_GETITEMTEXTW (LVM_FIRST + 115)
#define LVM_SETITEMTEXTW (LVM_FIRST + 116)
#define LVM_INSERTCOLUMNW (LVM_FIRST + 97)
#define LVM_GETCOLUMNW (LVM_FIRST + 95)
#define LVM_SETCOLUMNW (LVM_FIRST + 96)
#define LVM_GETSUBITEMRECT (LVM_FIRST + 56)
#define LVM_HITTEST (LVM_FIRST + 18)
#define LVS_ICON 0x0000u
#define LVS_REPORT 0x0001u
#define LVS_SMALLICON 0x0002u
#define LVS_LIST 0x0003u
#define LVS_SINGLESEL 0x0004u
#define LVS_NOCOLUMNHEADER 0x4000u
#define LVS_EX_FULLROWSELECT 0x00000020u
#define LVIF_TEXT 0x0001u
#define LVIF_IMAGE 0x0002u
#define LVIF_PARAM 0x0004u
#define LVIF_STATE 0x0008u
#define LVCF_FMT 0x0001u
#define LVCF_WIDTH 0x0002u
#define LVCF_TEXT 0x0004u
#define LVCF_SUBITEM 0x0008u
#define LVIS_FOCUSED 0x0001u
#define LVIS_SELECTED 0x0002u
#define LVNI_SELECTED 0x0002u
#define LVHT_NOWHERE 0x0001u
#define LVHT_ONITEMICON 0x0002u
#define LVHT_ONITEMLABEL 0x0004u
#define LVHT_ONITEMSTATEICON 0x0008u
#define LVHT_ONITEM (LVHT_ONITEMICON|LVHT_ONITEMLABEL|LVHT_ONITEMSTATEICON)
#define LVIR_BOUNDS 0
#define LVIR_ICON 1
#define LVIR_LABEL 2
#define LVIR_SELECTBOUNDS 3
#define LVN_FIRST ((UINT)-100)
#define LVN_ITEMCHANGED (LVN_FIRST-1)
#define LVN_ITEMACTIVATE (LVN_FIRST-14)
#define LVN_COLUMNCLICK (LVN_FIRST-8)
#define LVCFMT_LEFT 0
typedef struct _TCITEMW {
    UINT mask; DWORD dwState, dwStateMask; WCHAR *pszText;
    int cchTextMax, iImage; LPARAM lParam;
} TCITEMW;
typedef struct _NMHDR { HWND hwndFrom; UINT_PTR idFrom; UINT code; } NMHDR;
typedef struct _LVITEMW {
    UINT mask; int iItem, iSubItem; UINT state, stateMask; WCHAR *pszText;
    int cchTextMax, iImage; LPARAM lParam; int iIndent, iGroupId;
    UINT cColumns; UINT *puColumns; int *piColFmt; int iGroup;
} LVITEMW;
typedef struct _LVCOLUMNW {
    UINT mask; int fmt, cx; WCHAR *pszText; int cchTextMax, iSubItem;
    int iImage, iOrder, cxMin, cxDefault, cxIdeal;
} LVCOLUMNW;
typedef struct _NMLISTVIEW {
    NMHDR hdr; int iItem, iSubItem; UINT uNewState,uOldState,uChanged;
    POINT ptAction; LPARAM lParam;
} NMLISTVIEW;
typedef struct _LVHITTESTINFO { POINT pt; UINT flags; int iItem,iSubItem; } LVHITTESTINFO;
typedef struct _TBBUTTON {
    int iBitmap, idCommand;
    BYTE fsState, fsStyle;
    BYTE bReserved[6];
    ULONG_PTR dwData;
    INT_PTR iString;
} TBBUTTON;

COMCTL32_API BOOL InitCommonControlsEx(const INITCOMMONCONTROLSEX *controls);
COMCTL32_API void InitCommonControls(void);
COMCTL32_API HWND CreateStatusWindowW(LONG style, const WCHAR *text,
                                      HWND parent, UINT id);

/* Date/time common controls, matching the Win64 SYSTEMTIME/NM layouts. */
#define ICC_DATE_CLASSES 0x00000100u
#define ICC_UPDOWN_CLASS 0x00000010u
#define DATETIMEPICK_CLASSW L"SysDateTimePick32"
#define MONTHCAL_CLASSW L"SysMonthCal32"
#define UPDOWN_CLASSW L"msctls_updown32"
typedef struct { WORD wYear,wMonth,wDayOfWeek,wDay,wHour,wMinute,wSecond,wMilliseconds; } SV_SYSTEMTIME;
#define DTM_GETSYSTEMTIME 0x1001u
#define DTM_SETSYSTEMTIME 0x1002u
#define DTM_GETRANGE 0x1003u
#define DTM_SETRANGE 0x1004u
#define DTM_GETMONTHCAL 0x1008u
#define DTM_SETFORMATW 0x1032u
#define GDT_VALID 0
#define GDT_NONE 1
#define GDT_ERROR (-1)
#define GDTR_MIN 1u
#define GDTR_MAX 2u
#define DTS_SHOWNONE 2u
#define DTN_DATETIMECHANGE ((UINT)-759)
typedef struct { NMHDR nmhdr; DWORD dwFlags; SV_SYSTEMTIME st; } SV_NMDATETIMECHANGE;

#endif
