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
#include <String.h>


/* ------------------------------------------------------------------ */
/* Native services (user/w/ntdll/Ntdll.asm -> kernel/win32k/gdi)          */
/* ------------------------------------------------------------------ */

__declspec(dllimport) UINT64 NtGdiGetEntry(ULONG handle, void *cell);
__declspec(dllimport) UINT64 NtGdiBlitUserBuffer(HDC dc, const UINT32 *pixels,
    UINT32 width, UINT32 height, UINT32 source_x, UINT32 source_y,
    UINT32 copy_width, UINT32 copy_height);
__declspec(dllimport) long NtAllocateVirtualMemory(HANDLE process,
    void **base, ULONG_PTR zero_bits, ULONGLONG *size, ULONG allocation_type,
    ULONG protect);
__declspec(dllimport) long NtFreeVirtualMemory(HANDLE process,
    void **base, ULONGLONG *size, ULONG free_type);
__declspec(dllimport) UINT64 NtGdiCreateSolidBrush(COLORREF colour, ULONG unused);
__declspec(dllimport) UINT64 NtGdiCreatePen(int style, int width, COLORREF colour,
                                            ULONG unused);
__declspec(dllimport) UINT64 NtGdiHfontCreate(const LOGFONTW *logical_font);
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
__declspec(dllimport) UINT64 NtGdiStretchBlt(HDC dst, int x, int y, int width,
    int height, HDC src, int src_x, int src_y, int src_width, int src_height,
    ULONG rop);
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

typedef struct { HBITMAP Handle; int Width, Height; void *Bits;
                 ULONGLONG AllocatedBytes; } BITMAP_DIMENSIONS;
static BITMAP_DIMENSIONS g_bitmap_dimensions[128];
typedef struct { HRGN Handle; RECT Bounds; BOOL Used; } SV_REGION;
static SV_REGION g_regions[128];
typedef struct { HDC Dc; HBITMAP Bitmap; } SV_DC_BITMAP;
static SV_DC_BITMAP g_dc_bitmaps[128];
static SV_REGION *sv_region(HRGN region);
static void sv_track_dc_bitmap(HDC dc, HBITMAP bitmap);
static void sv_flush_dib(HDC dc);
static void sv_read_dib(HDC dc);

static void remember_bitmap_dimensions(HBITMAP bitmap, int width, int height)
{
    if (!bitmap || width <= 0 || height <= 0) return;
    for (UINT i = 0; i < 128; i++) {
        if (!g_bitmap_dimensions[i].Handle ||
            g_bitmap_dimensions[i].Handle == bitmap) {
            g_bitmap_dimensions[i].Handle = bitmap;
            g_bitmap_dimensions[i].Width = width;
            g_bitmap_dimensions[i].Height = height;
            g_bitmap_dimensions[i].Bits = 0;
            return;
        }
    }
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
    HBITMAP bitmap = (HBITMAP)(ULONG_PTR)NtGdiCreateCompatibleBitmap(
        reference, width, height);
    remember_bitmap_dimensions(bitmap, width, height);
    return bitmap;
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
    HBITMAP bitmap = (HBITMAP)(ULONG_PTR)NtGdiCreateBitmap(width, height,
                                      planes, bits_per_pixel, NULL);
    remember_bitmap_dimensions(bitmap, width, height);
    return bitmap;
}

GDI32_API int GetObjectW(HGDIOBJ object, int bytes, LPVOID out)
{
    if (!object || bytes <= 0 || !out || object_type(object) != SV_GDI_TYPE_BITMAP)
        return 0;
    int width = 0, height = 0;
    for (UINT i = 0; i < 128; i++) {
        if (g_bitmap_dimensions[i].Handle == (HBITMAP)object) {
            width = g_bitmap_dimensions[i].Width;
            height = g_bitmap_dimensions[i].Height;
            break;
        }
    }
    if (!width || !height) return 0;
    BITMAP bitmap = {0};
    bitmap.bmWidth = width;
    bitmap.bmHeight = height;
    bitmap.bmWidthBytes = width * 4;
    bitmap.bmPlanes = 1;
    bitmap.bmBitsPixel = 32;
    for (UINT i = 0; i < 128; i++)
        if (g_bitmap_dimensions[i].Handle == (HBITMAP)object) {
            bitmap.bmBits = g_bitmap_dimensions[i].Bits;
            break;
        }
    int copied = bytes < (int)sizeof(bitmap) ? bytes : (int)sizeof(bitmap);
    memcpy(out, &bitmap, (unsigned long long)copied);
    return copied;
}

typedef struct {
    DWORD Size;
    LONG Width, Height;
    WORD Planes, BitsPixel;
    DWORD Compression, SizeImage;
    LONG XPelsPerMeter, YPelsPerMeter;
    DWORD ClrUsed, ClrImportant;
} SV_BITMAPINFOHEADER;
typedef struct { SV_BITMAPINFOHEADER Header; DWORD Colors[1]; } SV_BITMAPINFO;

GDI32_API HBITMAP CreateDIBSection(HDC dc, const void *info, UINT usage,
                                    void **bits, HANDLE section, DWORD offset)
{
    (void)usage; (void)section; (void)offset;
    if (bits) *bits = 0;
    if (!info || !bits) return 0;
    const SV_BITMAPINFOHEADER *header = (const SV_BITMAPINFOHEADER *)info;
    static int trace;
    if(trace++<4){
        extern __declspec(dllimport) long NtDisplayString(const char*);
        char line[]="[dib] bpp=0000 compression=0000";
        const char *hex="0123456789abcdef";
        for(int i=0;i<4;i++){line[10+i]=hex[(header->BitsPixel>>(12-i*4))&15];line[27+i]=hex[(header->Compression>>(12-i*4))&15];}
        NtDisplayString(line);
    }
    if (header->Size < sizeof(*header) || header->Width <= 0 ||
        !header->Height || header->Height == (-2147483647L - 1) ||
        header->Planes != 1 || header->BitsPixel != 32 ||
        header->Compression != 0) return 0;
    int height = header->Height < 0 ? -header->Height : header->Height;
    if (header->Width > 4096 || height > 2160) return 0;
    ULONGLONG bytes = (ULONGLONG)(UINT)header->Width * (UINT)height * 4;
    ULONGLONG allocation = (bytes + 0xfff) & ~0xfffULL;
    void *memory = 0;
    if (NtAllocateVirtualMemory((HANDLE)(ULONGLONG)-1, &memory, 0,
                                &allocation, 0x3000, 4) < 0)
        return 0;
    HBITMAP bitmap = CreateCompatibleBitmap(dc, header->Width, height);
    if (!bitmap) {
        (void)NtFreeVirtualMemory((HANDLE)(ULONGLONG)-1, &memory,
                                 &allocation, 0x8000);
        return 0;
    }
    remember_bitmap_dimensions(bitmap, header->Width, height);
    for (UINT i = 0; i < 128; i++) {
        if (g_bitmap_dimensions[i].Handle == bitmap) {
            g_bitmap_dimensions[i].Bits = memory;
            g_bitmap_dimensions[i].AllocatedBytes = allocation;
            break;
        }
    }
    *bits = memory;
    return bitmap;
}

GDI32_API HFONT CreateFontIndirectW(const LOGFONTW *logical_font)
{
    return (HFONT)(ULONG_PTR)NtGdiHfontCreate(logical_font);
}

GDI32_API HBRUSH CreatePatternBrush(HBITMAP bitmap)
{
    if (!bitmap) return 0;
    /* Pattern sampling is not in the kernel raster ABI. A neutral solid brush
     * keeps callers functional while bitmap-backed pattern rasterization is
     * still unavailable. */
    return CreateSolidBrush(0x00c0c0c0u);
}

typedef struct { UINT Style; COLORREF Color; ULONG_PTR Hatch; } SV_LOGBRUSH;
GDI32_API HPEN ExtCreatePen(DWORD style, DWORD width, const void *brush,
                             DWORD style_count, const DWORD *styles)
{
    (void)style_count; (void)styles;
    if (!brush) return 0;
    const SV_LOGBRUSH *lb = (const SV_LOGBRUSH *)brush;
    return CreatePen((int)(style & 0xf), width ? (int)width : 1, lb->Color);
}

GDI32_API BOOL GetTextExtentPointW(HDC dc, const WCHAR *text, int count,
                                    LPSIZE size)
{ return GetTextExtentPoint32W(dc, text, count, size); }

static SV_REGION *sv_region(HRGN region)
{
    for (UINT i=0;i<128;i++)if(g_regions[i].Used&&g_regions[i].Handle==region)return &g_regions[i];
    return 0;
}
GDI32_API HRGN CreateRectRgn(int left,int top,int right,int bottom)
{
    for(UINT i=0;i<128;i++)if(!g_regions[i].Used){
        g_regions[i].Used=TRUE;g_regions[i].Handle=(HRGN)&g_regions[i];
        g_regions[i].Bounds=(RECT){left,top,right,bottom};return g_regions[i].Handle;}
    return 0;
}
GDI32_API HRGN CreateRectRgnIndirect(const RECT *rect)
{ return rect?CreateRectRgn(rect->left,rect->top,rect->right,rect->bottom):0; }
GDI32_API BOOL SetRectRgn(HRGN region,int left,int top,int right,int bottom)
{ SV_REGION*r=sv_region(region);if(!r)return FALSE;r->Bounds=(RECT){left,top,right,bottom};return TRUE; }
GDI32_API int GetRgnBox(HRGN region,LPRECT rect)
{ SV_REGION*r=sv_region(region);if(!r||!rect)return 0;*rect=r->Bounds;return rect->right<=rect->left||rect->bottom<=rect->top?1:2; }
GDI32_API int CombineRgn(HRGN dst,HRGN src1,HRGN src2,int mode)
{
    SV_REGION*d=sv_region(dst),*a=sv_region(src1),*b=sv_region(src2);if(!d||!a)return 0;
    if(mode==5||!b)d->Bounds=a->Bounds;
    else if(mode==1){RECT*r=&d->Bounds,*x=&a->Bounds,*y=&b->Bounds;
        r->left=x->left>y->left?x->left:y->left;r->top=x->top>y->top?x->top:y->top;
        r->right=x->right<y->right?x->right:y->right;r->bottom=x->bottom<y->bottom?x->bottom:y->bottom;
        if(r->right<r->left)r->right=r->left;if(r->bottom<r->top)r->bottom=r->top;
    }else{RECT*x=&a->Bounds,*y=&b->Bounds;d->Bounds=(RECT){
        x->left<y->left?x->left:y->left,x->top<y->top?x->top:y->top,
        x->right>y->right?x->right:y->right,x->bottom>y->bottom?x->bottom:y->bottom};}
    return d->Bounds.right<=d->Bounds.left||d->Bounds.bottom<=d->Bounds.top?1:2;
}
GDI32_API BOOL EqualRgn(HRGN first,HRGN second)
{ SV_REGION*a=sv_region(first),*b=sv_region(second);return a&&b&&a->Bounds.left==b->Bounds.left&&a->Bounds.top==b->Bounds.top&&a->Bounds.right==b->Bounds.right&&a->Bounds.bottom==b->Bounds.bottom; }

static void sv_track_dc_bitmap(HDC dc,HBITMAP bitmap)
{
    UINT slot=0;
    for(UINT i=0;i<128;i++)if(g_dc_bitmaps[i].Dc==dc){slot=i;goto found;}
    for(UINT i=0;i<128;i++)if(!g_dc_bitmaps[i].Dc){slot=i;goto found;}
    return;
found:g_dc_bitmaps[slot].Dc=bitmap?dc:0;g_dc_bitmaps[slot].Bitmap=bitmap;
}

static void sv_flush_dib(HDC dc)
{
    for(UINT i=0;i<128;i++)if(g_dc_bitmaps[i].Dc==dc&&g_dc_bitmaps[i].Bitmap){
        for(UINT b=0;b<128;b++)if(g_bitmap_dimensions[b].Handle==g_dc_bitmaps[i].Bitmap&&g_bitmap_dimensions[b].Bits){
            (void)NtGdiBlitUserBuffer(dc,(const UINT32*)g_bitmap_dimensions[b].Bits,
                g_bitmap_dimensions[b].Width,g_bitmap_dimensions[b].Height,0,0,
                g_bitmap_dimensions[b].Width,g_bitmap_dimensions[b].Height);return;}}
}

__declspec(dllimport) UINT64 NtGdiReadUserBuffer(HDC,void*,UINT,UINT);
static void sv_read_dib(HDC dc)
{
    for(UINT i=0;i<128;i++)if(g_dc_bitmaps[i].Dc==dc&&g_dc_bitmaps[i].Bitmap)
        for(UINT b=0;b<128;b++)if(g_bitmap_dimensions[b].Handle==g_dc_bitmaps[i].Bitmap&&g_bitmap_dimensions[b].Bits){
            NtGdiReadUserBuffer(dc,g_bitmap_dimensions[b].Bits,g_bitmap_dimensions[b].Width,g_bitmap_dimensions[b].Height);return;}
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
    sv_track_dc_bitmap(hdc, 0);
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
    {
        HGDIOBJ previous=(HGDIOBJ)(ULONG_PTR)NtGdiSelectBitmap(hdc,(HBITMAP)object);
        if(previous||object)sv_track_dc_bitmap(hdc,(HBITMAP)object);
        return previous;
    }
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
    SV_REGION *region=sv_region((HRGN)object);
    if(region){region->Used=FALSE;region->Handle=0;return TRUE;}
    /* Deleting a stock object is a documented no-op that succeeds. */
    if (is_stock(object))
        return TRUE;
    /* The kernel answers 0 when the object is still selected into a DC, which
     * is the documented failure for DeleteObject. */
    if (!NtGdiDeleteObjectApp((ULONG)(ULONG_PTR)object))
        return FALSE;
    for (UINT i = 0; i < 128; i++)
        if (g_bitmap_dimensions[i].Handle == (HBITMAP)object) {
            if(g_bitmap_dimensions[i].Bits){void*base=g_bitmap_dimensions[i].Bits;
                ULONGLONG size=0;(void)NtFreeVirtualMemory((HANDLE)(ULONGLONG)-1,&base,&size,0x8000);}
            g_bitmap_dimensions[i] = (BITMAP_DIMENSIONS){0};
        }
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
            cache[index] = remember_stock((HGDIOBJ)(ULONG_PTR)NtGdiHfontCreate(NULL));
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

__declspec(dllexport) int SvGdiDrawTextW(HDC dc, const WCHAR *text,
                                         int count, LPRECT rect, UINT format)
{ return DrawTextW(dc, text, count, rect, format); }
