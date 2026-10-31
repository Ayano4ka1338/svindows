/* GDI object lifetime, bitmaps, drawing primitives and text.
 * Included by kernel/win32k/Gdiobj.c. */
/* ------------------------------------------------------------------ */
/* Services: object lifetime                                           */
/* ------------------------------------------------------------------ */

/* NTSTATUS NtGdiGetEntry(ULONG handle, GDICELL64 *out) -- gdi32!GdiGetEntry
 * tail-jumps here for every handle whose upper bits it cannot match against the
 * table itself, which is every real handle. */
UINT64 NtGdiGetEntry(UINT64 *a)
{
    UINT64 handle = a[0];
    GDICELL64 *out = (GDICELL64 *)a[1];
    if (!out || !MmProbeForWrite((UINT64)out, sizeof(*out)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    W32K_OBJECT *object = gdi_object(handle, 0);
    if (!object) {
        memset(out, 0, sizeof(*out));
        return (UINT64)STATUS_INVALID_HANDLE;
    }
    *out = (GDICELL64){(UINT64)object, object->Owner->ProcessId,
                      object->Upper, object->Type, object->UserInfo};
    return (UINT64)STATUS_SUCCESS;
}

/* HBRUSH NtGdiCreateSolidBrush(COLORREF colour, HBRUSH reuse) */
UINT64 NtGdiCreateSolidBrush(UINT64 *a)
{
    W32K_OBJECT *object = NULL;
    UINT64 handle = gdi_allocate(GDI_TYPE_BRUSH, 0, &object);
    if (!handle)
        return 0;
    object->u.Brush.Color = (UINT32)a[0];
    object->u.Brush.Hollow = FALSE;
    return handle;
}

/* HPEN NtGdiCreatePen(int style, int width, COLORREF colour, HBRUSH) */
UINT64 NtGdiCreatePen(UINT64 *a)
{
    W32K_OBJECT *object = NULL;
    UINT64 handle = gdi_allocate(GDI_TYPE_PEN, 0, &object);
    if (!handle)
        return 0;
    object->u.Brush.Color = (UINT32)a[2];
    object->u.Brush.Hollow = ((INT32)a[0] == 5); /* PS_NULL */
    return handle;
}

/* HFONT NtGdiHfontCreate(...): one bitmap font exists, so every logical font
 * maps to it. The handle still has to be a real GDI object, because gdi32
 * validates it before every text call. */
UINT64 NtGdiHfontCreate(UINT64 *a)
{
    return gdi_allocate(GDI_TYPE_FONT, 0, NULL);
}

UINT64 NtGdiCreateRectRgn(UINT64 *a)
{
    return gdi_allocate(GDI_TYPE_REGION, 0, NULL);
}

UINT64 NtGdiDeleteObjectApp(UINT64 *a)
{
    W32K_OBJECT *object = gdi_object(a[0], 0);
    if (!object) return 0;
    for (UINT16 i = 1; i < GDI_MAX_OBJECTS; i++) {
        if (g_gdi_objects[i].Type != GDI_TYPE_DC) continue;
        W32K_DC *dc = &g_gdi_objects[i].u.Dc;
        if (dc->SelectedBitmap == a[0] || dc->SelectedBrush == a[0] ||
            dc->SelectedPen == a[0] || dc->SelectedFont == a[0]) return 0;
    }
    gdi_free(a[0]);
    return 1;
}

UINT64 NtGdiFlush(UINT64 *a)
{
    return 1;
}

/* ------------------------------------------------------------------ */
/* Services: memory DCs and bitmaps                                    */
/* ------------------------------------------------------------------ */

/* HDC NtGdiCreateCompatibleDC(HDC reference): a memory DC starts out bound to
 * nothing drawable, exactly as on Windows -- a 1x1 monochrome default. It
 * becomes useful once a bitmap is selected into it. */
UINT64 NtGdiCreateCompatibleDC(UINT64 *a)
{
    UINT64 attr = gdi_allocate_attr();
    if (!attr)
        return 0;
    W32K_OBJECT *object = NULL;
    UINT64 hdc = gdi_allocate(GDI_TYPE_DC, attr, &object);
    if (!hdc)
        return 0;
    W32K_DC *dc = &object->u.Dc;
    dc->Surface = NULL;
    *(UINT32 *)(attr + DCATTR_CRBACKGND) = 0x00FFFFFF;
    *(UINT32 *)(attr + DCATTR_ULBACKGND) = 0x00FFFFFF;
    *(UINT32 *)(attr + DCATTR_CRBRUSH) = 0x00FFFFFF;
    *(UINT32 *)(attr + DCATTR_ULBRUSH) = 0x00FFFFFF;
    return hdc;
}

static UINT64 gdi_create_bitmap(INT32 width, INT32 height)
{
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
        return 0;
    SIZE_T bytes = (SIZE_T)width * (SIZE_T)height * 4;
    void *bits = ExAllocatePoolWithTag(NonPagedPool, bytes, 'mtiB');
    if (!bits)
        return 0;
    memset(bits, 0, bytes);

    W32K_OBJECT *object = NULL;
    UINT64 handle = gdi_allocate(GDI_TYPE_BITMAP, 0, &object);
    if (!handle) {
        ExFreePool(bits);
        return 0;
    }
    object->u.Bitmap.Storage = bits;
    object->u.Bitmap.Surface.Bits = bits;
    object->u.Bitmap.Surface.Width = (UINT32)width;
    object->u.Bitmap.Surface.Height = (UINT32)height;
    object->u.Bitmap.Surface.Stride = (UINT32)width;
    object->u.Bitmap.Surface.IsScreen = FALSE;
    return handle;
}

/* HBITMAP NtGdiCreateCompatibleBitmap(HDC, int cx, int cy) */
UINT64 NtGdiCreateCompatibleBitmap(UINT64 *a)
{
    return gdi_create_bitmap((INT32)a[1], (INT32)a[2]);
}

/* HBITMAP NtGdiCreateBitmap(int cx, int cy, UINT planes, UINT bpp, void *bits) */
UINT64 NtGdiCreateBitmap(UINT64 *a)
{
    return gdi_create_bitmap((INT32)a[0], (INT32)a[1]);
}

/* HBITMAP NtGdiSelectBitmap(HDC, HBITMAP): binds the memory DC to the bitmap's
 * pixels, which is what makes a memory DC drawable. */
UINT64 NtGdiSelectBitmap(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    W32K_OBJECT *bitmap = gdi_object(a[1], GDI_TYPE_BITMAP);
    if (!dc || !bitmap || dc->Hwnd || dc->Surface == &dc->DeviceSurface)
        return 0;
    UINT64 previous = dc->SelectedBitmap;
    dc->SelectedBitmap = a[1];
    dc->Surface = &bitmap->u.Bitmap.Surface;
    dc->OrgX = dc->OrgY = 0;
    dc->ClipX = dc->ClipY = 0;
    dc->ClipW = (INT32)bitmap->u.Bitmap.Surface.Width;
    dc->ClipH = (INT32)bitmap->u.Bitmap.Surface.Height;
    return previous;
}

UINT64 NtGdiSelectBrush(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !gdi_object(a[1], GDI_TYPE_BRUSH))
        return 0;
    UINT64 previous = dc->SelectedBrush;
    dc->SelectedBrush = a[1];
    return previous;
}

UINT64 NtGdiSelectPen(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !gdi_object(a[1], GDI_TYPE_PEN))
        return 0;
    UINT64 previous = dc->SelectedPen;
    dc->SelectedPen = a[1];
    return previous;
}

UINT64 NtGdiSelectFont(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !gdi_object(a[1], GDI_TYPE_FONT))
        return 0;
    UINT64 previous = dc->SelectedFont;
    dc->SelectedFont = a[1];
    return previous;
}

/* HANDLE NtGdiGetDCObject(HDC, int type): GDI_OBJECT_TYPE_BRUSH == 0x10,
 * _PEN == 0x30, _FONT == 0x0A, _BITMAP == 0x05. */
UINT64 NtGdiGetDCObject(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc)
        return 0;
    switch ((UINT32)a[1]) {
    case GDI_TYPE_BRUSH:  return dc->SelectedBrush;
    case GDI_TYPE_PEN:    return dc->SelectedPen;
    case GDI_TYPE_FONT:   return dc->SelectedFont;
    case GDI_TYPE_BITMAP: return dc->SelectedBitmap;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Services: drawing                                                   */
/* ------------------------------------------------------------------ */

/* The colour a fill uses: the selected brush, or the DC brush colour cached in
 * DC_ATTR when nothing has been selected. */
static UINT32 dc_brush_color(UINT64 hdc, W32K_DC *dc, BOOLEAN *hollow)
{
    W32K_OBJECT *brush = gdi_object(dc->SelectedBrush, GDI_TYPE_BRUSH);
    if (hollow)
        *hollow = brush ? brush->u.Brush.Hollow : FALSE;
    if (brush)
        return brush->u.Brush.Color;
    UINT64 attr = dc_attr(hdc);
    return attr ? *(volatile UINT32 *)(attr + DCATTR_CRBRUSH) : 0x00FFFFFF;
}

static UINT32 dc_pen_color(UINT64 hdc, W32K_DC *dc, BOOLEAN *hollow)
{
    W32K_OBJECT *pen = gdi_object(dc->SelectedPen, GDI_TYPE_PEN);
    if (hollow)
        *hollow = pen ? pen->u.Brush.Hollow : FALSE;
    if (pen)
        return pen->u.Brush.Color;
    UINT64 attr = dc_attr(hdc);
    return attr ? *(volatile UINT32 *)(attr + DCATTR_CRPEN) : 0;
}

/* BOOL NtGdiPatBlt(HDC, int x, int y, int w, int h, DWORD rop) */
UINT64 NtGdiPatBlt(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !dc->Surface)
        return 0;
    UINT32 rop = (UINT32)a[5];
    BOOLEAN hollow = FALSE;
    UINT32 color;
    switch (rop) {
    case 0x00000042: color = 0x00000000; break;             /* BLACKNESS */
    case 0x00FF0062: color = 0x00FFFFFF; break;             /* WHITENESS */
    default:         color = dc_brush_color(a[0], dc, &hollow); break;
    }
    if (hollow)
        return 1;
    dc_fill(dc, (INT32)a[1], (INT32)a[2], (INT32)a[3], (INT32)a[4], color);
    dc_present(dc);
    return 1;
}

/* BOOL NtGdiBitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx,
 *                  int sy, DWORD rop, DWORD backColour, FLONG flags) */
UINT64 NtGdiBitBlt(UINT64 *a)
{
    W32K_DC *dst = dc_from_handle(a[0]);
    W32K_DC *src = dc_from_handle(a[5]);
    if (!dst || !dst->Surface)
        return 0;
    INT32 x = (INT32)a[1], y = (INT32)a[2];
    INT32 w = (INT32)a[3], h = (INT32)a[4];
    UINT32 rop = (UINT32)a[8];

    /* A blit with no source is a pattern fill; that is how a lot of client code
     * clears a rectangle. */
    if (!src || !src->Surface) {
        if (rop == 0x00000042 || rop == 0x00FF0062) {
            dc_fill(dst, x, y, w, h,
                    rop == 0x00000042 ? 0x00000000 : 0x00FFFFFF);
            dc_present(dst);
            return 1;
        }
        return 0;
    }

    INT32 sx = (INT32)a[6], sy = (INT32)a[7];
    for (INT32 row = 0; row < h; row++) {
        for (INT32 col = 0; col < w; col++) {
            INT32 source_x = src->OrgX + sx + col;
            INT32 source_y = src->OrgY + sy + row;
            if (source_x < 0 || source_y < 0 ||
                source_x >= (INT32)src->Surface->Width ||
                source_y >= (INT32)src->Surface->Height)
                continue;
            UINT32 pixel = src->Surface->Bits[(UINT32)source_y *
                                              src->Surface->Stride +
                                              (UINT32)source_x];
            INT32 dx = x + col, dy = y + row, dw = 1, dh = 1;
            if (!dc_clip(dst, &dx, &dy, &dw, &dh))
                continue;
            dc_pixel(dst, dx, dy, pixel);
        }
    }
    dc_present(dst);
    return 1;
}

/* Copy an RGB software surface into a DC's client rectangle. The service is
 * fast path for copying a user RGB surface into a window DC. */
UINT64 NtGdiBlitUserBuffer(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    const UINT32 *source = (const UINT32 *)a[1];
    UINT32 width = (UINT32)a[2], height = (UINT32)a[3];
    UINT32 source_x = (UINT32)a[4], source_y = (UINT32)a[5];
    UINT32 copy_width = (UINT32)a[6], copy_height = (UINT32)a[7];
    if (!dc || !dc->Surface || !source || !width || !height ||
        width > 4096 || height > 2160 || !copy_width || !copy_height ||
        source_x >= width || source_y >= height ||
        copy_width > width - source_x || copy_height > height - source_y ||
        !MmProbeForRead((UINT64)source, (UINT64)width * height * sizeof(UINT32)))
        return 0;

    INT32 x = 0, y = 0, w = (INT32)copy_width, h = (INT32)copy_height;
    if (!dc_clip(dc, &x, &y, &w, &h))
        return 1;
    dc_damage(dc, x, y, w, h);
    BOOLEAN unobscured = !dc->Surface->IsScreen || !dc->Hwnd ||
        W32kScreenRectUnobscured(dc->Hwnd, x, y, w, h);
    for (INT32 row = 0; row < h; ++row) {
        const UINT32 *src = source +
            (UINT64)(source_y + y + row - dc->OrgY) * width +
            (source_x + x - dc->OrgX);
        UINT32 *dst = dc->Surface->Bits + (UINT64)(y + row) *
                      dc->Surface->Stride + x;
        for (INT32 col = 0; col < w; ++col) {
            if (!unobscured &&
                !W32kScreenPixelVisible(dc->Hwnd, x + col, y + row))
                continue;
            UINT32 rgb = src[col];
            dst[col] = ((rgb >> 16) & 0xffu) << GfxFramebuffer.RedShift |
                       ((rgb >> 8) & 0xffu) << GfxFramebuffer.GreenShift |
                       (rgb & 0xffu) << GfxFramebuffer.BlueShift;
        }
    }
    dc_present(dc);
    return 1;
}

UINT64 NtGdiSetPixel(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !dc->Surface)
        return (UINT64)-1;
    INT32 x = (INT32)a[1], y = (INT32)a[2], w = 1, h = 1;
    if (dc_clip(dc, &x, &y, &w, &h))
        dc_pixel(dc, x, y, gdi_pixel((UINT32)a[3]));
    dc_present(dc);
    return a[3];
}

UINT64 NtGdiMoveTo(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc)
        return 0;
    INT32 *old = (INT32 *)a[3];
    if (old && MmProbeForWrite((UINT64)old, 8)) {
        old[0] = dc->CurX;
        old[1] = dc->CurY;
    }
    dc->CurX = (INT32)a[1];
    dc->CurY = (INT32)a[2];
    return 1;
}

/* Straight Bresenham in DC coordinates; each pixel is clipped individually. */
UINT64 NtGdiLineTo(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !dc->Surface)
        return 0;
    BOOLEAN hollow = FALSE;
    UINT32 pixel = gdi_pixel(dc_pen_color(a[0], dc, &hollow));
    INT32 x0 = dc->CurX, y0 = dc->CurY;
    INT32 x1 = (INT32)a[1], y1 = (INT32)a[2];
    dc->CurX = x1;
    dc->CurY = y1;
    if (hollow)
        return 1;

    INT32 dx = x1 > x0 ? x1 - x0 : x0 - x1;
    INT32 dy = y1 > y0 ? y1 - y0 : y0 - y1;
    INT32 sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    INT32 err = dx - dy;
    for (;;) {
        INT32 px = x0, py = y0, pw = 1, ph = 1;
        if (dc_clip(dc, &px, &py, &pw, &ph))
            dc_pixel(dc, px, py, pixel);
        if (x0 == x1 && y0 == y1)
            break;
        INT32 e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
    dc_present(dc);
    return 1;
}

/* BOOL NtGdiRectangle(HDC, int left, int top, int right, int bottom):
 * brush-filled interior with a one-pixel pen border. */
UINT64 NtGdiRectangle(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !dc->Surface)
        return 0;
    INT32 left = (INT32)a[1], top = (INT32)a[2];
    INT32 right = (INT32)a[3], bottom = (INT32)a[4];
    if (right < left || bottom < top)
        return 0;

    BOOLEAN brush_hollow = FALSE, pen_hollow = FALSE;
    UINT32 fill = dc_brush_color(a[0], dc, &brush_hollow);
    UINT32 edge = dc_pen_color(a[0], dc, &pen_hollow);
    if (!brush_hollow)
        dc_fill(dc, left, top, right - left, bottom - top, fill);
    if (!pen_hollow) {
        dc_fill(dc, left, top, right - left, 1, edge);
        dc_fill(dc, left, bottom - 1, right - left, 1, edge);
        dc_fill(dc, left, top, 1, bottom - top, edge);
        dc_fill(dc, right - 1, top, 1, bottom - top, edge);
    }
    dc_present(dc);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Services: text                                                      */
/* ------------------------------------------------------------------ */

#define GDI_FONT_HEIGHT GFX_FONT_H

/* BOOL NtGdiExtTextOutW(HDC, int x, int y, UINT flags, const RECT *clip,
 *                       const WCHAR *text, int count, const INT *dx,
 *                       DWORD codePage)
 *
 * ETO_OPAQUE == 0x0002 fills the background rectangle first; otherwise the
 * background mode in DC_ATTR decides, and TRANSPARENT is the common case.
 * Advances come from the proportional UI face, so a string laid out by the
 * client and the same string measured by GetTextExtent line up exactly. */
UINT64 NtGdiExtTextOutW(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !dc->Surface)
        return 0;

    INT32 x = (INT32)a[1], y = (INT32)a[2];
    UINT32 flags = (UINT32)a[3];
    const UINT16 *text = (const UINT16 *)a[5];
    INT32 count = (INT32)a[6];
    if (count < 0 || count > 4096)
        return 0;
    if (count && (!text || !MmProbeForRead((UINT64)text,
                                           (SIZE_T)count * sizeof(UINT16))))
        return 0;

    UINT32 fg = dc_text_color(a[0]);
    UINT32 bg = dc_bk_color(a[0]);
    BOOLEAN opaque = (flags & 0x0002) != 0;
    const INT32 *dx = (const INT32 *)a[7];

    if (opaque)
        dc_fill(dc, x, y, (INT32)GfxUiTextWidthCountW(text, (UINT32)count),
                GDI_FONT_HEIGHT, bg);

    INT32 pen = x;
    for (INT32 i = 0; i < count; i++) {
        /* A caller-supplied dx array overrides the font's own advances, which
         * is how DrawText aligns and letter-spaces runs. */
        INT32 step = (dx && MmProbeForRead((UINT64)(dx + i), sizeof(INT32)))
                         ? dx[i]
                         : (INT32)GfxUiCharWidth(text[i]);
        dc_glyph(dc, pen, y, text[i], fg, bg, opaque);
        pen += step;
    }
    dc_present(dc);
    return 1;
}

/* BOOL NtGdiGetTextMetricsW(HDC, TMW_INTERNAL *out, ULONG size)
 *
 * The UI face is a 12 pixel em (9pt) face: GDI reported the same numbers for
 * Segoe UI at this size, so layout that asks for the metrics gets the truth
 * instead of a placeholder. */
UINT64 NtGdiGetTextMetricsW(UINT64 *a)
{
    if (!dc_from_handle(a[0]))
        return 0;
    UINT8 *out = (UINT8 *)a[1];
    UINT32 size = (UINT32)a[2];
    if (size > 0x100)
        size = 0x100;
    if (!out || size < 0x3C || !MmProbeForWrite((UINT64)out, size))
        return 0;
    memset(out, 0, size);

    INT32 ascent = GfxUiAscent(), descent = GfxUiDescent();
    INT32 *metrics = (INT32 *)out;
    metrics[0] = ascent + descent;     /* tmHeight            */
    metrics[1] = ascent;               /* tmAscent            */
    metrics[2] = descent;              /* tmDescent           */
    metrics[3] = 0;                    /* tmInternalLeading   */
    metrics[4] = 0;                    /* tmExternalLeading   */
    metrics[5] = GfxUiCharWidth('x');  /* tmAveCharWidth      */
    metrics[6] = GfxUiCharWidth('W');  /* tmMaxCharWidth      */
    metrics[7] = 400;                  /* tmWeight (normal)   */
    metrics[8] = 0;                    /* tmOverhang          */
    metrics[9] = 96;                   /* tmDigitizedAspectX  */
    metrics[10] = 96;                  /* tmDigitizedAspectY  */
    *(UINT16 *)(out + 0x2C) = 32;      /* tmFirstChar         */
    *(UINT16 *)(out + 0x2E) = 126;     /* tmLastChar          */
    *(UINT16 *)(out + 0x30) = '?';     /* tmDefaultChar       */
    *(UINT16 *)(out + 0x32) = ' ';     /* tmBreakChar         */
    /* Variable pitch (TMPF_FIXED_PITCH in Windows means "not fixed"), Swiss
     * family: what the atlas actually is. */
    out[0x37] = 0x21;                  /* tmPitchAndFamily    */
    out[0x38] = 0x01;                  /* tmCharSet: DEFAULT  */
    return 1;
}

static void gdi_text_size(const UINT16 *text, INT32 count, INT32 *width,
                          INT32 *height)
{
    *width = (INT32)GfxUiTextWidthCountW(text, (UINT32)(count < 0 ? 0 : count));
    *height = GfxUiAscent() + GfxUiDescent();
}

/* BOOL NtGdiGetTextExtent(HDC, const WCHAR *, int count, SIZE *out, UINT) */
UINT64 NtGdiGetTextExtent(UINT64 *a)
{
    if (!dc_from_handle(a[0]))
        return 0;
    const UINT16 *text = (const UINT16 *)a[1];
    INT32 count = (INT32)a[2];
    INT32 *out = (INT32 *)a[3];
    if (!out || !MmProbeForWrite((UINT64)out, 8))
        return 0;
    if (count < 0 || count > 4096)
        return 0;
    if (count && (!text || !MmProbeForRead((UINT64)text,
                                           (SIZE_T)count * sizeof(UINT16))))
        return 0;
    gdi_text_size(text, count, &out[0], &out[1]);
    return 1;
}

/* BOOL NtGdiGetTextExtentExW(HDC, const WCHAR *, ULONG count, ULONG maxExtent,
 *                           ULONG *fit, ULONG *dx, SIZE *size, FLONG) */
UINT64 NtGdiGetTextExtentExW(UINT64 *a)
{
    if (!dc_from_handle(a[0]))
        return 0;
    const UINT16 *text = (const UINT16 *)a[1];
    UINT32 count = (UINT32)a[2];
    if (count > 4096)
        return 0;
    if (count && (!text || !MmProbeForRead((UINT64)text,
                                           (SIZE_T)count * sizeof(UINT16))))
        return 0;
    UINT32 max_extent = (UINT32)a[3];
    UINT32 *fit = (UINT32 *)a[4];
    UINT32 *dx = (UINT32 *)a[5];
    INT32 *size = (INT32 *)a[6];

    UINT32 partial = 0;
    UINT32 fitting = GfxUiTextFitCountW(text, count, max_extent, &partial);
    if (dx && MmProbeForWrite((UINT64)dx, (SIZE_T)count * sizeof(UINT32))) {
        UINT32 pen = 0;
        for (UINT32 i = 0; i < count; i++) {
            pen += GfxUiCharWidth(text[i]);
            dx[i] = pen;
        }
    }
    if (fit && MmProbeForWrite((UINT64)fit, sizeof(UINT32)))
        *fit = fitting;
    if (size && MmProbeForWrite((UINT64)size, 8))
        gdi_text_size(text, (INT32)count, &size[0], &size[1]);
    return 1;
}
