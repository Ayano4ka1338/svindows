/*
 * user/w/gdi32/Gdi32.c - GDI objects and device contexts over the NtGdi* services.
 *
 * Everything here is a thin translation of the documented Win32 entry point
 * into the kernel ABI documented by the kernel/win32k/gdi parts. Two pieces of
 * client-side state exist because the kernel has no service for them:
 *
 *   1. The stock object table. win32k keeps a table of pre-made stock brushes,
 *      pens and fonts reachable through NtGdiGetStockObject; Svindows does not
 *      register that service, so GetStockObject materialises equivalent real
 *      GDI objects through NtGdiCreateSolidBrush/NtGdiCreatePen/NtGdiHfontCreate
 *      and caches them. They are genuine handles: SelectObject, Rectangle and
 *      TextOut accept them exactly like a created object.
 *
 *   2. The background mode. NtGdiExtTextOutW decides from its ETO_OPAQUE flag
 *      whether it paints the text background; the kernel never reads a DC
 *      "bk mode" field, so SetBkMode records the mode per DC here and TextOutW
 *      turns it into the flag the kernel honours.
 */

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef long long          INT64;

#include "Gdi32.h"

/* ------------------------------------------------------------------ */
/* Native services (user/w/ntdll/Ntdll.asm -> kernel/win32k/gdi)          */
/* ------------------------------------------------------------------ */

__declspec(dllimport) UINT64 NtGdiGetEntry(ULONG handle, void *cell);
__declspec(dllimport) UINT64 NtGdiCreateSolidBrush(COLORREF colour, ULONG unused);
__declspec(dllimport) UINT64 NtGdiCreatePen(int style, int width, COLORREF colour,
                                            ULONG unused);
__declspec(dllimport) UINT64 NtGdiHfontCreate(void);
__declspec(dllimport) UINT64 NtGdiSelectBrush(HDC hdc, HBRUSH brush);
__declspec(dllimport) UINT64 NtGdiSelectPen(HDC hdc, HPEN pen);
__declspec(dllimport) UINT64 NtGdiSelectFont(HDC hdc, HFONT font);
__declspec(dllimport) UINT64 NtGdiSelectBitmap(HDC hdc, HBITMAP bitmap);
__declspec(dllimport) UINT64 NtGdiDeleteObjectApp(ULONG object);
__declspec(dllimport) UINT64 NtGdiPatBlt(HDC hdc, int x, int y, int width, int height,
                                         ULONG rop);
__declspec(dllimport) UINT64 NtGdiBitBlt(HDC dst, int x, int y, int width, int height,
                                         HDC src, int src_x, int src_y, ULONG rop,
                                         ULONG background, ULONG flags);
__declspec(dllimport) UINT64 NtGdiExtTextOutW(HDC hdc, int x, int y, ULONG options,
                                              const RECT *clip, const WCHAR *text,
                                              int count, const int *dx,
                                              ULONG code_page);
__declspec(dllimport) UINT64 NtGdiGetTextMetricsW(HDC hdc, void *metrics, ULONG size);
__declspec(dllimport) UINT64 NtGdiGetTextExtent(HDC hdc, const WCHAR *text, int count,
                                                SIZE *size, ULONG flags);
__declspec(dllimport) UINT64 NtGdiSetPixel(HDC hdc, int x, int y, COLORREF colour);
__declspec(dllimport) UINT64 NtGdiRectangle(HDC hdc, int left, int top, int right,
                                            int bottom);
__declspec(dllimport) UINT64 NtGdiMoveTo(HDC hdc, int x, int y, POINT *previous);
__declspec(dllimport) UINT64 NtGdiLineTo(HDC hdc, int x, int y);
__declspec(dllimport) UINT64 NtGdiCreateCompatibleDC(HDC reference);
__declspec(dllimport) UINT64 NtGdiCreateCompatibleBitmap(HDC reference, int width,
                                                         int height);
__declspec(dllimport) UINT64 NtGdiCreateBitmap(int width, int height, ULONG planes,
                                               ULONG bits_per_pixel, const void *bits);
__declspec(dllimport) int    NtGdiGetDeviceCaps(HDC hdc, int index);

/* ------------------------------------------------------------------ */
/* The shared GDI handle table                                          */
/* ------------------------------------------------------------------ */

/* GDICELL64 as published by win32k: NtGdiGetEntry(handle, &cell) is how a
 * client learns an object's type, which is what lets one SelectObject entry
 * point dispatch to the per-type NtGdiSelect* service. */
typedef struct _SV_GDICELL {
    UINT64 pKernelInfo;
    UINT32 ProcessId;
    UINT16 nUpper;
    UINT16 nType;
    UINT64 pUserInfo;
} SV_GDICELL;

_Static_assert(sizeof(SV_GDICELL) == 24, "GDICELL64 is 24 bytes on x64");

/* nType values (the GDI handle type nibble). */
#define SV_GDI_TYPE_DC     0x01
#define SV_GDI_TYPE_REGION 0x04
#define SV_GDI_TYPE_BITMAP 0x05
#define SV_GDI_TYPE_FONT   0x0A
#define SV_GDI_TYPE_BRUSH  0x10
#define SV_GDI_TYPE_PEN    0x30

/* DC_ATTR offsets win32k reads the drawing colours from. The block lives in a
 * user-writable arena and its address arrives as the handle cell's pUserInfo,
 * which is exactly the path a client uses: SetTextColor and SetBkColor never
 * enter the kernel. */
#define DCATTR_CRBACKGND 0x20
#define DCATTR_ULBACKGND 0x24
#define DCATTR_CRFOREGND 0x28
#define DCATTR_ULFOREGND 0x2C

static ULONG *dc_attr(HDC hdc)
{
    SV_GDICELL cell;
    if (!hdc)
        return NULL;
    if (NtGdiGetEntry((ULONG)(ULONG_PTR)hdc, &cell) != 0)
        return NULL; /* STATUS_INVALID_HANDLE: not one of our DCs */
    return (ULONG *)(ULONG_PTR)cell.pUserInfo;
}

static UINT16 object_type(HGDIOBJ object)
{
    SV_GDICELL cell;
    if (!object)
        return 0;
    if (NtGdiGetEntry((ULONG)(ULONG_PTR)object, &cell) != 0)
        return 0;
    return cell.nType;
}

/* ------------------------------------------------------------------ */
/* Device contexts and bitmaps                                          */
/* ------------------------------------------------------------------ */

GDI32_API HDC CreateCompatibleDC(HDC reference)
{
    /* The reference DC only selects the format; Svindows has one 32-bpp raster
     * format, so the argument is accepted and unused, as on Windows. */
    (void)reference;
    return (HDC)(ULONG_PTR)NtGdiCreateCompatibleDC(0);
}

GDI32_API HBITMAP CreateCompatibleBitmap(HDC reference, int width, int height)
{
    return (HBITMAP)(ULONG_PTR)NtGdiCreateCompatibleBitmap(reference, width, height);
}

GDI32_API HBITMAP CreateBitmap(int width, int height, UINT planes,
                               UINT bits_per_pixel, const void *bits)
{
    /* The kernel allocates a zeroed 32-bpp surface; the caller's bit pattern is
     * not copied, so a CreateBitmap with initial bits would silently come back
     * blank. Refuse that combination instead of returning a bitmap with the
     * wrong content. */
    if (bits)
        return NULL;
    return (HBITMAP)(ULONG_PTR)NtGdiCreateBitmap(width, height, planes,
                                                 bits_per_pixel, NULL);
}

GDI32_API BOOL DeleteDC(HDC hdc)
{
    if (!hdc)
        return FALSE;
    /* DeleteDC is NtGdiDeleteObjectApp on a DC handle, which is what win32k
     * itself does. The kernel refuses to free an object still selected into a
     * device context, so this cannot strand a live DC. */
    if (!NtGdiDeleteObjectApp((ULONG)(ULONG_PTR)hdc))
        return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Objects                                                             */
/* ------------------------------------------------------------------ */

GDI32_API HBRUSH CreateSolidBrush(COLORREF colour)
{
    return (HBRUSH)(ULONG_PTR)NtGdiCreateSolidBrush(colour, 0);
}

GDI32_API HPEN CreatePen(int style, int width, COLORREF colour)
{
    /* NtGdiCreatePen(style, width, colour, hbrush) records the colour and
     * whether the pen is hollow (PS_NULL); width and style beyond that are not
     * modelled by the kernel's one-pixel rasteriser. */
    return (HPEN)(ULONG_PTR)NtGdiCreatePen(style, width, colour, 0);
}

GDI32_API HGDIOBJ SelectObject(HDC hdc, HGDIOBJ object)
{
    /* One Win32 entry point, four typed kernel services: the object type comes
     * from the shared handle table. A stock-object handle created by this DLL
     * is a real brush/pen/font, so it takes the same path. */
    switch (object_type(object)) {
    case SV_GDI_TYPE_BRUSH:
        return (HGDIOBJ)(ULONG_PTR)NtGdiSelectBrush(hdc, (HBRUSH)object);
    case SV_GDI_TYPE_PEN:
        return (HGDIOBJ)(ULONG_PTR)NtGdiSelectPen(hdc, (HPEN)object);
    case SV_GDI_TYPE_FONT:
        return (HGDIOBJ)(ULONG_PTR)NtGdiSelectFont(hdc, (HFONT)object);
    case SV_GDI_TYPE_BITMAP:
        return (HGDIOBJ)(ULONG_PTR)NtGdiSelectBitmap(hdc, (HBITMAP)object);
    default:
        /* Also the "handle belongs to another process or is not a GDI object"
         * case: deleting or selecting it would corrupt the drawing state. */
        return NULL;
    }
}

/* Stock objects this DLL materialised, so DeleteObject can leave them alone
 * exactly like the real stock table does. */
#define SV_STOCK_SLOTS 16
static HGDIOBJ g_stock[SV_STOCK_SLOTS];
static UINT    g_stock_count;

static BOOL is_stock(HGDIOBJ object)
{
    for (UINT i = 0; i < g_stock_count; i++)
        if (g_stock[i] == object)
            return TRUE;
    return FALSE;
}

static HGDIOBJ remember_stock(HGDIOBJ object)
{
    if (object && g_stock_count < SV_STOCK_SLOTS)
        g_stock[g_stock_count++] = object;
    return object;
}

GDI32_API BOOL DeleteObject(HGDIOBJ object)
{
    if (!object)
        return FALSE;
    /* Deleting a stock object is a documented no-op that succeeds. */
    if (is_stock(object))
        return TRUE;
    /* The kernel answers 0 when the object is still selected into a DC, which
     * is the documented failure for DeleteObject. */
    if (!NtGdiDeleteObjectApp((ULONG)(ULONG_PTR)object))
        return FALSE;
    return TRUE;
}

GDI32_API HGDIOBJ GetStockObject(int index)
{
    /* Lazily created once per index; see the file comment. NULL is the honest
     * answer for a stock object this build cannot represent. */
    static HGDIOBJ cache[DC_PEN + 1];
    switch (index) {
    case WHITE_BRUSH:
    case LTGRAY_BRUSH:
    case GRAY_BRUSH:
    case DKGRAY_BRUSH:
    case BLACK_BRUSH:
        if (!cache[index]) {
            static const COLORREF colours[] = {0x00FFFFFF, 0x00C0C0C0, 0x00808080,
                                               0x00404040, 0x00000000};
            cache[index] = remember_stock(CreateSolidBrush(colours[index]));
        }
        return cache[index];
    case WHITE_PEN:
    case BLACK_PEN:
    case NULL_PEN:
        if (!cache[index]) {
            COLORREF colour = index == WHITE_PEN ? 0x00FFFFFF : 0x00000000;
            int style = index == NULL_PEN ? PS_NULL : PS_SOLID;
            cache[index] = remember_stock(CreatePen(style, 1, colour));
        }
        return cache[index];
    case DEFAULT_GUI_FONT:
    case SYSTEM_FONT:
    case ANSI_VAR_FONT:
    case ANSI_FIXED_FONT:
    case SYSTEM_FIXED_FONT:
    case OEM_FIXED_FONT:
    case DEVICE_DEFAULT_FONT:
        /* One bitmap font exists in the kernel, so every logical font handle
         * selects it. Always a real object: SelectObject must accept it. */
        if (!cache[index])
            cache[index] = remember_stock((HGDIOBJ)(ULONG_PTR)NtGdiHfontCreate());
        return cache[index];
    default:
        /* NULL_BRUSH/HOLLOW_BRUSH: the kernel's brush object has no hollow flag
         * (only a PS_NULL pen is hollow), so a hollow brush cannot be expressed
         * and NULL is returned. DEFAULT_PALETTE, DC_BRUSH and DC_PEN have no
         * object type in this build either. */
        return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Device capabilities                                                 */
/* ------------------------------------------------------------------ */

GDI32_API int GetDeviceCaps(HDC hdc, int index)
{
    /* Answered by the kernel from the framebuffer geometry and the one display
     * format this build supports; a DC that does not belong to this process
     * yields 0 rather than a fabricated capability. */
    return NtGdiGetDeviceCaps(hdc, index);
}

#include "GdiDraw.inc"
