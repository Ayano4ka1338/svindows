#include <Gdi32.h>

typedef unsigned long long ULONG_PTR;
typedef unsigned long long SIZE_T;
typedef int GpStatus;
typedef struct { ULONG Version; void *DebugEventCallback; BOOL SuppressBackgroundThread;
                 BOOL SuppressExternalCodecs; } GdiplusStartupInput;
typedef struct { int type; HBITMAP bitmap; int width, height, owns_bitmap; } GP_IMAGE;
typedef struct { HDC hdc; } GP_GRAPHICS;
typedef struct { HPEN pen; } GP_PEN;
typedef struct { HBRUSH brush; } GP_BRUSH;
typedef struct { LONG bmType, bmWidth, bmHeight; LONG bmWidthBytes;
                 WORD bmPlanes, bmBitsPixel; void *bmBits; } SV_BITMAP;

__declspec(dllimport) void *GlobalAlloc(unsigned int flags, SIZE_T bytes);
__declspec(dllimport) void *GlobalFree(void *memory);
__declspec(dllimport) int GetObjectW(HGDIOBJ object, int bytes, void *out);

__declspec(dllimport) HANDLE GetModuleHandleA(const char*);
__declspec(dllimport) void *GetProcAddress(HANDLE,const char*);
static void gp_trace(const char *text) {
    static int count;if(count++>=128)return;
    long (*print)(const char*)=(long(*)(const char*))GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtDisplayString");
    if(print)print(text);
}

#define GP_OK 0
#define GP_INVALID_PARAMETER 2
#define GP_OUT_OF_MEMORY 3
#define GP_NOT_IMPLEMENTED 6
#define GP_HEAP_ZERO_MEMORY 8
#define GP_IMAGE_BITMAP 1
#define GP_PS_SOLID 0

static void *gp_alloc(SIZE_T bytes)
{
    return GlobalAlloc(GP_HEAP_ZERO_MEMORY, bytes);
}

static COLORREF gp_color(unsigned int argb)
{
    return ((argb >> 16) & 0xff) | (argb & 0x00ff00) |
           ((argb & 0xff) << 16);
}

static GP_IMAGE *gp_image(HANDLE image)
{
    GP_IMAGE *p = (GP_IMAGE *)image;
    return p && p->type == GP_IMAGE_BITMAP ? p : 0;
}

static GP_GRAPHICS *gp_graphics(HANDLE graphics)
{
    GP_GRAPHICS *p = (GP_GRAPHICS *)graphics;
    return p && p->hdc ? p : 0;
}

__declspec(dllexport) GpStatus GdiplusStartup(ULONG_PTR *token,
        const GdiplusStartupInput *input, void *output)
{
    (void)input; (void)output;
    if (token) *token = 1;
    return GP_OK;
}

__declspec(dllexport) void GdiplusShutdown(ULONG_PTR token) { (void)token; }

__declspec(dllexport) void *GdipAlloc(SIZE_T bytes) { return gp_alloc(bytes); }
__declspec(dllexport) void GdipFree(void *memory) { if (memory) (void)GlobalFree(memory); }

__declspec(dllexport) GpStatus GdipCreateFromHDC(HDC hdc, HANDLE *out)
{
    if (!hdc || !out) return GP_INVALID_PARAMETER;
    GP_GRAPHICS *g = (GP_GRAPHICS *)gp_alloc(sizeof(*g));
    if (!g) return GP_OUT_OF_MEMORY;
    g->hdc = hdc;
    *out = g;
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipGetImageGraphicsContext(HANDLE image,
                                                           HANDLE *out)
{
    GP_IMAGE *im = gp_image(image);
    if (!im || !out) {gp_trace("[gp] graphics input invalid");return GP_INVALID_PARAMETER;}
    HDC dc = CreateCompatibleDC(0);
    if (!dc) return GP_OUT_OF_MEMORY;
    (void)SelectObject(dc, im->bitmap);
    GP_GRAPHICS *g = (GP_GRAPHICS *)gp_alloc(sizeof(*g));
    if (!g) { (void)DeleteDC(dc); return GP_OUT_OF_MEMORY; }
    g->hdc = dc;
    gp_trace("[gp] graphics ready");
    *out = g;
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipDeleteGraphics(HANDLE graphics)
{
    GP_GRAPHICS *g = gp_graphics(graphics);
    if (!g) return GP_INVALID_PARAMETER;
    (void)DeleteDC(g->hdc);
    (void)GlobalFree(g);
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipSetPageUnit(HANDLE graphics, int unit)
{ return gp_graphics(graphics) ? GP_OK : GP_INVALID_PARAMETER; }
__declspec(dllexport) GpStatus GdipSetSmoothingMode(HANDLE graphics, int mode)
{ return gp_graphics(graphics) ? GP_OK : GP_INVALID_PARAMETER; }
__declspec(dllexport) GpStatus GdipSetInterpolationMode(HANDLE graphics, int mode)
{ return gp_graphics(graphics) ? GP_OK : GP_INVALID_PARAMETER; }

__declspec(dllexport) GpStatus GdipCreateBitmapFromScan0(int width, int height,
        int stride, int pixel_format, const void *scan0, HANDLE *out)
{
    (void)stride; (void)pixel_format;
    char trace[]="[gp] scan width=00000000 height=00000000";
    const char *hex="0123456789abcdef";
    for(int i=0;i<8;i++){trace[16+i]=hex[((unsigned)width>>(28-4*i))&15];trace[32+i]=hex[((unsigned)height>>(28-4*i))&15];}
    gp_trace(trace);
    if (width <= 0 || height <= 0 || !out) return GP_INVALID_PARAMETER;
    HBITMAP bitmap = CreateCompatibleBitmap(0, width, height);
    if (!bitmap) {gp_trace("[gp] scan bitmap allocation failed");return GP_OUT_OF_MEMORY;}
    if (scan0) {
        HDC dc = CreateCompatibleDC(0);
        if (!dc) { (void)DeleteObject(bitmap); return GP_OUT_OF_MEMORY; }
        HGDIOBJ old = SelectObject(dc, bitmap);
        const unsigned int *pixels = (const unsigned int *)scan0;
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                (void)SetPixel(dc, x, y, gp_color(pixels[y * width + x]));
        if (old) (void)SelectObject(dc, old);
        (void)DeleteDC(dc);
    }
    GP_IMAGE *im = (GP_IMAGE *)gp_alloc(sizeof(*im));
    if (!im) {gp_trace("[gp] scan wrapper allocation failed"); (void)DeleteObject(bitmap); return GP_OUT_OF_MEMORY; }
    im->type = GP_IMAGE_BITMAP; im->bitmap = bitmap;
    im->width = width; im->height = height; im->owns_bitmap = 1;
    gp_trace("[gp] scan created");
    *out = im;
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipCreateBitmapFromHBITMAP(HBITMAP bitmap,
        HANDLE palette, HANDLE *out)
{
    (void)palette;
    if (!bitmap || !out) {gp_trace(!bitmap?"[gp] from HBITMAP NULL":"[gp] from HBITMAP output NULL");return GP_INVALID_PARAMETER;}
    SV_BITMAP info;
    if (GetObjectW(bitmap, sizeof(info), &info) != sizeof(info)) {
        gp_trace("[gp] from HBITMAP GetObject failed");return GP_INVALID_PARAMETER;
    }
    gp_trace("[gp] from HBITMAP ready");
    GP_IMAGE *im = (GP_IMAGE *)gp_alloc(sizeof(*im));
    if (!im) return GP_OUT_OF_MEMORY;
    im->type = GP_IMAGE_BITMAP; im->bitmap = bitmap;
    im->width = info.bmWidth; im->height = info.bmHeight;
    im->owns_bitmap = 0;
    *out = im;
    return GP_OK;
}

static GP_IMAGE *gp_copy_image(const GP_IMAGE *source, int x, int y,
                               int width, int height)
{
    if (!source || width <= 0 || height <= 0) return 0;
    HBITMAP bitmap = CreateCompatibleBitmap(0, width, height);
    HDC src = CreateCompatibleDC(0), dst = CreateCompatibleDC(0);
    if (!bitmap || !src || !dst) {
        if (bitmap) (void)DeleteObject(bitmap);
        if (src) (void)DeleteDC(src);
        if (dst) (void)DeleteDC(dst);
        return 0;
    }
    (void)SelectObject(src, source->bitmap);
    (void)SelectObject(dst, bitmap);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    int copy_width = width;
    int copy_height = height;
    if (x + copy_width > source->width) copy_width = source->width - x;
    if (y + copy_height > source->height) copy_height = source->height - y;
    if (copy_width > 0 && copy_height > 0)
        (void)BitBlt(dst, 0, 0, copy_width, copy_height, src, x, y, 0x00CC0020);
    (void)DeleteDC(src); (void)DeleteDC(dst);
    GP_IMAGE *copy = (GP_IMAGE *)gp_alloc(sizeof(*copy));
    if (!copy) { (void)DeleteObject(bitmap); return 0; }
    copy->type = GP_IMAGE_BITMAP; copy->bitmap = bitmap;
    copy->width = width; copy->height = height; copy->owns_bitmap = 1;
    return copy;
}

__declspec(dllexport) GpStatus GdipCloneImage(HANDLE image, HANDLE *out)
{
    GP_IMAGE *im = gp_image(image);
    if (!im || !out) return GP_INVALID_PARAMETER;
    GP_IMAGE *copy = gp_copy_image(im, 0, 0, im->width, im->height);
    if (!copy) return GP_OUT_OF_MEMORY;
    *out = copy;
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipCloneBitmapAreaI(int x, int y,
        int width, int height, int pixel_format, HANDLE image, HANDLE *out)
{
    (void)pixel_format;
    GP_IMAGE *im = gp_image(image);
    if (!im || !out) {gp_trace(!im?"[gp] clone source invalid":"[gp] clone output invalid");return GP_INVALID_PARAMETER;}
    GP_IMAGE *copy = gp_copy_image(im, x, y, width, height);
    if (!copy) {gp_trace("[gp] clone allocation failed");return GP_OUT_OF_MEMORY;}
    gp_trace("[gp] clone ready");
    *out = copy;
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipCreateHBITMAPFromBitmap(HANDLE image,
        HBITMAP *out, unsigned int background)
{
    (void)background;
    GP_IMAGE *im = gp_image(image);
    gp_trace(!im?"[gp] HBITMAP invalid image":!out?"[gp] HBITMAP invalid output":"[gp] HBITMAP valid input");
    if (!im || !out) return GP_INVALID_PARAMETER;
    GP_IMAGE *copy = gp_copy_image(im, 0, 0, im->width, im->height);
    if (!copy) {gp_trace("[gp] HBITMAP copy allocation failed");return GP_OUT_OF_MEMORY;}
    gp_trace("[gp] HBITMAP ready");
    *out = copy->bitmap;
    copy->owns_bitmap = 0;
    (void)GlobalFree(copy);
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipDisposeImage(HANDLE image)
{
    GP_IMAGE *im = gp_image(image);
    if (!im) return GP_INVALID_PARAMETER;
    if (im->owns_bitmap) (void)DeleteObject(im->bitmap);
    (void)GlobalFree(im);
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipCreatePen1(unsigned int color, float width,
        int unit, HANDLE *out)
{
    (void)width; (void)unit;
    if (!out) return GP_INVALID_PARAMETER;
    GP_PEN *pen = (GP_PEN *)gp_alloc(sizeof(*pen));
    if (!pen) return GP_OUT_OF_MEMORY;
    pen->pen = CreatePen(GP_PS_SOLID, 1, gp_color(color));
    if (!pen->pen) { (void)GlobalFree(pen); return GP_OUT_OF_MEMORY; }
    *out = pen;
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipDeletePen(HANDLE pen)
{
    GP_PEN *p = (GP_PEN *)pen;
    if (!p) return GP_INVALID_PARAMETER;
    if (p->pen) (void)DeleteObject(p->pen);
    (void)GlobalFree(p);
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipCreateSolidFill(unsigned int color,
        HANDLE *out)
{
    if (!out) return GP_INVALID_PARAMETER;
    GP_BRUSH *brush = (GP_BRUSH *)gp_alloc(sizeof(*brush));
    if (!brush) return GP_OUT_OF_MEMORY;
    brush->brush = CreateSolidBrush(gp_color(color));
    if (!brush->brush) { (void)GlobalFree(brush); return GP_OUT_OF_MEMORY; }
    *out = brush;
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipDeleteBrush(HANDLE brush)
{
    GP_BRUSH *b = (GP_BRUSH *)brush;
    if (!b) return GP_INVALID_PARAMETER;
    if (b->brush) (void)DeleteObject(b->brush);
    (void)GlobalFree(b);
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipFillRectangleI(HANDLE graphics,
        HANDLE brush, int x, int y, int width, int height)
{
    GP_GRAPHICS *g = gp_graphics(graphics); GP_BRUSH *b = (GP_BRUSH *)brush;
    if (!g || !b || !b->brush) return GP_INVALID_PARAMETER;
    HGDIOBJ old = SelectObject(g->hdc, b->brush);
    (void)Rectangle(g->hdc, x, y, x + width, y + height);
    if (old) (void)SelectObject(g->hdc, old);
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipDrawLineI(HANDLE graphics, HANDLE pen,
        int x1, int y1, int x2, int y2)
{
    GP_GRAPHICS *g = gp_graphics(graphics); GP_PEN *p = (GP_PEN *)pen;
    if (!g || !p || !p->pen) return GP_INVALID_PARAMETER;
    HGDIOBJ old = SelectObject(g->hdc, p->pen);
    (void)MoveToEx(g->hdc, x1, y1, 0); (void)LineTo(g->hdc, x2, y2);
    if (old) (void)SelectObject(g->hdc, old);
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipDrawArcI(HANDLE graphics, HANDLE pen,
        int x, int y, int width, int height, float start, float sweep)
{
    GP_GRAPHICS *g = gp_graphics(graphics); GP_PEN *p = (GP_PEN *)pen;
    (void)start; (void)sweep;
    if (!g || !p || !p->pen) return GP_INVALID_PARAMETER;
    HGDIOBJ old = SelectObject(g->hdc, p->pen);
    (void)Ellipse(g->hdc, x, y, x + width, y + height);
    if (old) (void)SelectObject(g->hdc, old);
    return GP_OK;
}

__declspec(dllexport) GpStatus GdipDrawImageRectI(HANDLE graphics,
        HANDLE image, int x, int y, int width, int height)
{
    GP_GRAPHICS *g = gp_graphics(graphics); GP_IMAGE *im = gp_image(image);
    if (!g || !im || width <= 0 || height <= 0) {gp_trace(!g?"[gp] draw graphics invalid":!im?"[gp] draw image invalid":"[gp] draw size invalid");return GP_INVALID_PARAMETER;}
    HDC src = CreateCompatibleDC(0);
    if (!src) return GP_OUT_OF_MEMORY;
    (void)SelectObject(src, im->bitmap);
    int copy_width = width < im->width ? width : im->width;
    int copy_height = height < im->height ? height : im->height;
    (void)BitBlt(g->hdc, x, y, copy_width, copy_height,
                 src, 0, 0, 0x00CC0020);
    (void)DeleteDC(src);
    return GP_OK;
}
