#ifndef SVINDOWS_UXTHEME_H
#define SVINDOWS_UXTHEME_H
#include <Gdi32.h>
typedef void *HTHEME;
typedef LONG HRESULT;
#ifdef SV_UXTHEME_BUILD
#define UXTHEME_API __declspec(dllexport)
#else
#define UXTHEME_API __declspec(dllimport)
#endif
#define S_OK ((HRESULT)0)
#define E_INVALIDARG ((HRESULT)0x80070057u)
#define E_HANDLE ((HRESULT)0x80070006u)
#define E_NOTIMPL ((HRESULT)0x80004001u)
#define BP_PUSHBUTTON 1
#define BP_RADIOBUTTON 2
#define BP_CHECKBOX 3
#define PBS_NORMAL 1
#define PBS_HOT 2
#define PBS_PRESSED 3
#define PBS_DISABLED 4
#define PBS_DEFAULTED 5
#define EP_EDITTEXT 1
#define EP_EDITBORDER_NOSCROLL 6
#define ETS_NORMAL 1
#define ETS_HOT 2
#define ETS_DISABLED 4
#define ETS_FOCUSED 5
#define ETS_READONLY 6
#define TMT_TEXTCOLOR 3803
UXTHEME_API HTHEME OpenThemeData(void *hwnd, const WCHAR *class_list);
UXTHEME_API HRESULT CloseThemeData(HTHEME theme);
UXTHEME_API BOOL IsThemeActive(void);
UXTHEME_API BOOL IsAppThemed(void);
UXTHEME_API HRESULT DrawThemeBackground(HTHEME theme, HDC dc, int part,
                                        int state, const RECT *rect,
                                        const RECT *clip);
UXTHEME_API HRESULT GetThemeBackgroundContentRect(HTHEME theme, HDC dc,
                 int part, int state, const RECT *rect, RECT *content);
UXTHEME_API HRESULT GetThemeColor(HTHEME theme, int part, int state,
                                  int property, COLORREF *color);
#endif
