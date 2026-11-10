#ifndef SVINDOWS_COMCTL32_INTERNAL_H
#define SVINDOWS_COMCTL32_INTERNAL_H
#include "Comctl32.h"
BOOL SvRegisterProgress(void);
BOOL SvRegisterStatus(void);
BOOL SvRegisterTrackbar(void);
BOOL SvRegisterTab(void);
BOOL SvRegisterListView(void);
BOOL SvRegisterTreeView(void);
BOOL SvRegisterToolbar(void);
BOOL SvRegisterDateTime(void);
BOOL SvRegisterUpDown(void);
LRESULT SvProgressProc(HWND, UINT, WPARAM, LPARAM);
LRESULT SvStatusProc(HWND, UINT, WPARAM, LPARAM);
LRESULT SvTrackbarProc(HWND, UINT, WPARAM, LPARAM);
LRESULT SvTabProc(HWND, UINT, WPARAM, LPARAM);
LRESULT SvListViewProc(HWND, UINT, WPARAM, LPARAM);
LRESULT SvToolbarProc(HWND, UINT, WPARAM, LPARAM);
void *SvComctlAlloc(ULONG_PTR bytes);
void SvComctlFree(void *memory);
void SvComctlFill(HDC dc, const RECT *rect, COLORREF color);
#endif
