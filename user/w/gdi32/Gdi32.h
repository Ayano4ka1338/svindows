/*
 * user/w/gdi32/Gdi32.h - the GDI surface Svindows implements.
 *
 * This is the GDI half of the client-side Win32 layer: everything either
 * reaches a NtGdi* service (kernel/win32k/gdi) or is composed from the services
 * that exist. Where the kernel has no service at all (Ellipse, Polyline, the
 * stock object table, logical fonts) the implementation says so in a comment
 * rather than pretending the feature is present.
 *
 * The base Win32 types live here and are shared with User32.h, which includes
 * this header first: exactly the way wingdi.h and winuser.h divide the same
 * typedefs on Windows.
 */
#ifndef SVINDOWS_GDI32_H_
#define SVINDOWS_GDI32_H_

#ifndef SVINDOWS_WIN32_BASE_TYPES
#define SVINDOWS_WIN32_BASE_TYPES

typedef void              *HANDLE;
typedef void              *LPVOID;
typedef const void        *LPCVOID;
typedef unsigned char      BYTE;
typedef unsigned short     WORD;
typedef unsigned short     WCHAR;
typedef unsigned int       UINT;
typedef unsigned int       DWORD;
typedef unsigned int       ULONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long long ULONG_PTR;
typedef long long          LONG_PTR;
typedef int                BOOL;
typedef int                LONG;
typedef unsigned short     USHORT;
typedef signed char        CHAR;

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* A COLORREF is 0x00BBGGRR; the kernel converts it through GfxColor() so the
 * framebuffer's channel order stays a kernel detail. */
typedef DWORD COLORREF;
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r)) | (((WORD)(g)) << 8) | \
                                 (((DWORD)(BYTE)(b)) << 16)))
#define CLR_INVALID ((COLORREF)0xFFFFFFFF)

typedef struct _RECT { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef const RECT *LPCRECT;
typedef struct _POINT { LONG x, y; } POINT, *LPPOINT;
typedef struct _SIZE { LONG cx, cy; } SIZE, *LPSIZE;
typedef struct _BITMAP {
    LONG bmType, bmWidth, bmHeight, bmWidthBytes;
    WORD bmPlanes, bmBitsPixel;
    void *bmBits;
} BITMAP, *LPBITMAP;

#endif /* SVINDOWS_WIN32_BASE_TYPES */

/* GDI object handles. All GDI handles in Svindows are 32-bit values whose
 * upper 16 bits carry the object generation and whose type nibble is published
 * in the shared handle table (kernel/win32k/gdi/Objects.inc). */
typedef void *HDC;
typedef void *HBITMAP;
typedef void *HBRUSH;
typedef void *HPEN;
typedef void *HFONT;
typedef void *HRGN;
typedef void *HGDIOBJ;

#ifdef SV_GDI32_BUILD
#define GDI32_API __declspec(dllexport)
#else
#define GDI32_API __declspec(dllimport)
#endif

/* TEXTMETRICW: 60 bytes on x64, which is the size NtGdiGetTextMetricsW
 * validates (it rejects anything below 0x3C). */
typedef struct _TEXTMETRICW {
    LONG  tmHeight;
    LONG  tmAscent;
    LONG  tmDescent;
    LONG  tmInternalLeading;
    LONG  tmExternalLeading;
    LONG  tmAveCharWidth;
    LONG  tmMaxCharWidth;
    LONG  tmWeight;
    LONG  tmOverhang;
    LONG  tmDigitizedAspectX;
    LONG  tmDigitizedAspectY;
    WCHAR tmFirstChar;
    WCHAR tmLastChar;
    WCHAR tmDefaultChar;
    WCHAR tmBreakChar;
    BYTE  tmItalic;
    BYTE  tmUnderlined;
    BYTE  tmStruckOut;
    BYTE  tmPitchAndFamily;
    BYTE  tmCharSet;
} TEXTMETRICW, *LPTEXTMETRICW;

typedef struct _LOGFONTW {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision;
    BYTE lfClipPrecision, lfQuality, lfPitchAndFamily;
    WCHAR lfFaceName[32];
} LOGFONTW, *LPLOGFONTW;

/* CreatePen styles and the ROPs and flags the kernel understands. */
#define PS_SOLID   0
#define PS_DASH    1
#define PS_DOT     2
#define PS_DASHDOT 3
#define PS_DASHDOTDOT 4
#define PS_NULL    5

#define BLACKNESS 0x00000042u
#define WHITENESS 0x00FF0062u
#define PATCOPY   0x00F00021u
#define SRCCOPY   0x00CC0020u

#define ETO_OPAQUE 0x0002u
#define ETO_CLIPPED 0x0004u

#define OPAQUE      2
#define TRANSPARENT 1

/* GetStockObject indices. */
#define WHITE_BRUSH        0
#define LTGRAY_BRUSH       1
#define GRAY_BRUSH         2
#define DKGRAY_BRUSH       3
#define BLACK_BRUSH        4
#define NULL_BRUSH         5
#define HOLLOW_BRUSH       NULL_BRUSH
#define WHITE_PEN          6
#define BLACK_PEN          7
#define NULL_PEN           8
#define OEM_FIXED_FONT     10
#define ANSI_FIXED_FONT    11
#define ANSI_VAR_FONT      12
#define SYSTEM_FONT        13
#define DEVICE_DEFAULT_FONT 14
#define DEFAULT_PALETTE    15
#define SYSTEM_FIXED_FONT  16
#define DEFAULT_GUI_FONT   17
#define DC_BRUSH           18
#define DC_PEN             19

/* GetDeviceCaps indices used by this build. */
#define DRIVERVERSION 0
#define TECHNOLOGY    2
#define HORZSIZE      4
#define VERTSIZE      6
#define HORZRES       8
#define VERTRES       10
#define BITSPIXEL     12
#define PLANES        14
#define NUMBRUSHES    16
#define NUMPENS       18
#define NUMFONTS      22
#define NUMCOLORS     24
#define RASTERCAPS    38
#define LOGPIXELSX    88
#define LOGPIXELSY    90
#define SIZEPALETTE   104
#define COLORRES      108

/* DrawTextW flags (the subset this build implements). */
#define DT_TOP         0x0000
#define DT_LEFT        0x0000
#define DT_CENTER      0x0001
#define DT_RIGHT       0x0002
#define DT_VCENTER     0x0004
#define DT_BOTTOM      0x0008
#define DT_WORDBREAK   0x0010
#define DT_SINGLELINE  0x0020
#define DT_EXPANDTABS  0x0040
#define DT_NOCLIP      0x0100
#define DT_CALCRECT    0x0400
#define DT_NOPREFIX    0x0800
#define DT_END_ELLIPSIS 0x8000

/* Device contexts and bitmaps. */
GDI32_API HDC     CreateCompatibleDC(HDC reference);
GDI32_API HBITMAP CreateCompatibleBitmap(HDC reference, int width, int height);
GDI32_API HBITMAP CreateBitmap(int width, int height, UINT planes, UINT bits_per_pixel,
                               const void *bits);
GDI32_API int     GetObjectW(HGDIOBJ object, int bytes, LPVOID out);
GDI32_API int     SetDIBits(HDC dc, HBITMAP bitmap, UINT start_scan,
                            UINT scan_lines, const void *bits,
                            const void *info, UINT usage);
GDI32_API HBITMAP CreateDIBSection(HDC dc, const void *bitmap_info,
                                   UINT usage, void **bits, HANDLE section,
                                   DWORD offset);
GDI32_API HFONT   CreateFontIndirectW(const LOGFONTW *logical_font);
GDI32_API HBRUSH  CreatePatternBrush(HBITMAP bitmap);
GDI32_API HPEN    ExtCreatePen(DWORD style, DWORD width, const void *brush,
                               DWORD style_count, const DWORD *styles);
GDI32_API BOOL    GetTextExtentPointW(HDC dc, const WCHAR *text, int count,
                                      LPSIZE size);
GDI32_API HRGN    CreateRectRgn(int left, int top, int right, int bottom);
GDI32_API HRGN    CreateRectRgnIndirect(const RECT *rect);
GDI32_API HRGN    ExtCreateRegion(const void *transform, DWORD bytes, const void *data);
GDI32_API BOOL    SetRectRgn(HRGN region, int left, int top, int right, int bottom);
GDI32_API int     OffsetRgn(HRGN region, int dx, int dy);
GDI32_API int     GetRgnBox(HRGN region, LPRECT rect);
GDI32_API DWORD   GetRegionData(HRGN region, DWORD bytes, void *data);
GDI32_API int     CombineRgn(HRGN dst, HRGN src1, HRGN src2, int mode);
GDI32_API BOOL    EqualRgn(HRGN first, HRGN second);
GDI32_API DWORD   SetLayout(HDC dc, DWORD layout);
GDI32_API DWORD   GetLayout(HDC dc);
GDI32_API BOOL    DeleteDC(HDC hdc);

/* Objects. */
GDI32_API HBRUSH  CreateSolidBrush(COLORREF colour);
GDI32_API HPEN    CreatePen(int style, int width, COLORREF colour);
GDI32_API HGDIOBJ SelectObject(HDC hdc, HGDIOBJ object);
GDI32_API BOOL    DeleteObject(HGDIOBJ object);
GDI32_API HGDIOBJ GetStockObject(int index);

/* Drawing. */
GDI32_API BOOL    Rectangle(HDC hdc, int left, int top, int right, int bottom);
GDI32_API BOOL    Ellipse(HDC hdc, int left, int top, int right, int bottom);
GDI32_API BOOL    PatBlt(HDC hdc, int x, int y, int width, int height, DWORD rop);
GDI32_API BOOL    BitBlt(HDC dst, int x, int y, int width, int height,
                         HDC src, int src_x, int src_y, DWORD rop);
GDI32_API BOOL    StretchBlt(HDC dst, int x, int y, int width, int height,
                             HDC src, int src_x, int src_y, int src_width,
                             int src_height, DWORD rop);
GDI32_API COLORREF SetPixel(HDC hdc, int x, int y, COLORREF colour);
GDI32_API BOOL    MoveToEx(HDC hdc, int x, int y, LPPOINT previous);
GDI32_API BOOL    LineTo(HDC hdc, int x, int y);
GDI32_API BOOL    Polyline(HDC hdc, const POINT *points, int count);

/* Text. */
GDI32_API BOOL    TextOutW(HDC hdc, int x, int y, const WCHAR *text, int count);
GDI32_API BOOL    ExtTextOutW(HDC hdc, int x, int y, UINT options,
                              const RECT *clip, const WCHAR *text, UINT count,
                              const int *dx);
GDI32_API int     DrawTextW(HDC hdc, const WCHAR *text, int count, LPRECT rect,
                            UINT format);
GDI32_API BOOL    GetTextExtentPoint32W(HDC hdc, const WCHAR *text, int count,
                                        LPSIZE size);
GDI32_API BOOL    GetTextMetricsW(HDC hdc, LPTEXTMETRICW metrics);
GDI32_API COLORREF SetTextColor(HDC hdc, COLORREF colour);
GDI32_API COLORREF GetTextColor(HDC hdc);
GDI32_API COLORREF SetBkColor(HDC hdc, COLORREF colour);
GDI32_API int     SetBkMode(HDC hdc, int mode);
GDI32_API int     GetDeviceCaps(HDC hdc, int index);

#endif /* SVINDOWS_GDI32_H_ */
