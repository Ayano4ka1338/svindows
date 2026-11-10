/*
 * user/w/gdi32/GdiDraw.inc - the drawing and text half of gdi32.dll.
 *
 * Included by Gdi32.c so the handle-table and DC_ATTR helpers stay shared.
 *
 * Two entry points have no NtGdi service behind them and are composed from the
 * ones that exist:
 *   Ellipse   - drawn with brush-filled spans (NtGdiPatBlt PATCOPY) and a pen
 *               outline (NtGdiMoveTo/NtGdiLineTo).
 *   Polyline  - a MoveToEx followed by one LineTo per remaining point.
 * Both are honest rasterisations of the shape, not aliases of another call.
 */

/* Per-DC background mode. NtGdiExtTextOutW paints the text background only
 * when ETO_OPAQUE is passed, so the mode lives here and TextOutW translates it
 * into that flag; the kernel has no DC field for it. */
#define SV_BKMODE_SLOTS 16
static HDC g_bkmode_dc[SV_BKMODE_SLOTS];
static int g_bkmode_value[SV_BKMODE_SLOTS];
static UINT g_bkmode_next;

static int dc_bk_mode(HDC hdc)
{
    for (UINT i = 0; i < SV_BKMODE_SLOTS; i++)
        if (g_bkmode_dc[i] == hdc)
            return g_bkmode_value[i];
    return OPAQUE; /* the Win32 default */
}

GDI32_API int SetBkMode(HDC hdc, int mode)
{
    if (mode != OPAQUE && mode != TRANSPARENT)
        return 0;
    for (UINT i = 0; i < SV_BKMODE_SLOTS; i++) {
        if (g_bkmode_dc[i] == hdc) {
            int previous = g_bkmode_value[i];
            g_bkmode_value[i] = mode;
            return previous;
        }
    }
    UINT slot = g_bkmode_next++ % SV_BKMODE_SLOTS;
    g_bkmode_dc[slot] = hdc;
    g_bkmode_value[slot] = mode;
    return OPAQUE;
}

/* SetTextColor/SetBkColor write the DC_ATTR block the kernel reads its drawing
 * colours from. That block is user-writable for exactly this reason, so neither
 * call enters the kernel. */
static COLORREF set_dc_colour(HDC hdc, ULONG offset, COLORREF colour)
{
    ULONG *attr = dc_attr(hdc);
    if (!attr)
        return CLR_INVALID;
    ULONG *field = (ULONG *)((UINT8 *)attr + offset);
    ULONG previous = *field;
    *field = (ULONG)colour & 0x00FFFFFFu;
    /* The companion "palette index" word must be cleared, or win32k would
     * interpret the stored value as a palette entry. */
    *(ULONG *)((UINT8 *)attr + offset + 4) = 0;
    return (COLORREF)(previous & 0x00FFFFFFu);
}

GDI32_API COLORREF SetTextColor(HDC hdc, COLORREF colour)
{
    return set_dc_colour(hdc, DCATTR_CRFOREGND, colour);
}

GDI32_API COLORREF SetBkColor(HDC hdc, COLORREF colour)
{
    return set_dc_colour(hdc, DCATTR_CRBACKGND, colour);
}

/* ------------------------------------------------------------------ */
/* Shapes                                                              */
/* ------------------------------------------------------------------ */

GDI32_API BOOL Rectangle(HDC hdc, int left, int top, int right, int bottom)
{
    /* The kernel fills with the selected brush and outlines with the selected
     * pen, including the hollow (PS_NULL) cases. */
    return NtGdiRectangle(hdc, left, top, right, bottom) != 0;
}

GDI32_API BOOL PatBlt(HDC hdc, int x, int y, int width, int height, DWORD rop)
{
    /* The kernel implements BLACKNESS, WHITENESS and (as the default) a fill
     * with the selected brush, which is PATCOPY. Any other ROP is refused here
     * rather than silently drawn as a brush fill. */
    if (rop != BLACKNESS && rop != WHITENESS && rop != PATCOPY)
        return FALSE;
    return NtGdiPatBlt(hdc, x, y, width, height, rop) != 0;
}

GDI32_API BOOL BitBlt(HDC dst, int x, int y, int width, int height, HDC src,
                      int src_x, int src_y, DWORD rop)
{
    /* Only SRCCOPY exists in the kernel's blitter; it also accepts a source-less
     * BLACKNESS/WHITENESS fill. */
    if (rop != SRCCOPY && rop != BLACKNESS && rop != WHITENESS)
        return FALSE;
    sv_flush_dib(src);
    if(dst!=src)sv_flush_dib(dst);
    BOOL result=NtGdiBitBlt(dst,x,y,width,height,src,src_x,src_y,(ULONG)rop,0,0)!=0;
    sv_read_dib(dst);return result;
}

GDI32_API BOOL StretchBlt(HDC dst, int x, int y, int width, int height,
                          HDC src, int src_x, int src_y, int src_width,
                          int src_height, DWORD rop)
{
    if (rop != SRCCOPY || width == 0 || height == 0 ||
        src_width == 0 || src_height == 0)
        return FALSE;
    sv_flush_dib(src);
    if (dst != src) sv_flush_dib(dst);
    BOOL result = NtGdiStretchBlt(dst, x, y, width, height, src, src_x,
                                  src_y, src_width, src_height, rop) != 0;
    sv_read_dib(dst);
    return result;
}

GDI32_API COLORREF SetPixel(HDC hdc, int x, int y, COLORREF colour)
{
    /* NtGdiSetPixel returns the colour it stored, or (UINT64)-1 when the point
     * is outside the DC or the DC has no surface. */
    return (COLORREF)(ULONG)NtGdiSetPixel(hdc, x, y, colour);
}

GDI32_API BOOL MoveToEx(HDC hdc, int x, int y, LPPOINT previous)
{
    return NtGdiMoveTo(hdc, x, y, previous) != 0;
}

GDI32_API BOOL LineTo(HDC hdc, int x, int y)
{
    return NtGdiLineTo(hdc, x, y) != 0;
}

GDI32_API BOOL Polyline(HDC hdc, const POINT *points, int count)
{
    /* No NtGdiPolyline is registered, so this is the documented composition:
     * one MoveToEx to the first point and a LineTo for every following vertex.
     * The kernel's rasteriser includes both endpoints; Win32's LineTo excludes
     * the last pixel, so a closed polyline draws its final pixel twice. */
    if (!points || count < 2)
        return FALSE;
    if (!MoveToEx(hdc, points[0].x, points[0].y, NULL))
        return FALSE;
    for (int i = 1; i < count; i++)
        if (!LineTo(hdc, points[i].x, points[i].y))
            return FALSE;
    return TRUE;
}

/* Integer half-width of an ellipse at row offset dy, walking x down as the row
 * moves away from the centre: no floating point is used anywhere in this DLL. */
static void ellipse_half_width(INT64 rx, INT64 ry, INT64 dy, INT64 *half)
{
    INT64 rx2 = rx * rx, ry2 = ry * ry, limit = rx2 * ry2;
    INT64 x = *half;
    while (x > 0 && x * x * ry2 + dy * dy * rx2 > limit)
        x--;
    *half = x;
}

GDI32_API BOOL Ellipse(HDC hdc, int left, int top, int right, int bottom)
{
    /* No NtGdiEllipse is registered: fill with the selected brush through
     * NtGdiPatBlt(PATCOPY) row by row and outline the rim with the selected pen
     * through MoveToEx/LineTo, including the horizontal runs that stitch the rim
     * together where the half-width jumps between rows. */
    if (right < left || bottom < top)
        return FALSE;
    INT64 rx = (INT64)(right - left) / 2;
    INT64 ry = (INT64)(bottom - top) / 2;
    if (rx <= 0 || ry <= 0)
        return Rectangle(hdc, left, top, right, bottom);
    INT64 cx = (INT64)left + rx;
    INT64 cy = (INT64)top + ry;

    INT64 half = rx, previous = rx;
    for (INT64 dy = 0; dy <= ry; dy++) {
        ellipse_half_width(rx, ry, dy, &half);
        INT64 x0 = cx - half, x1 = cx + half;
        INT64 p0 = cx - previous, p1 = cx + previous;
        for (int side = 0; side < 2; side++) {
            INT64 y = side ? cy + dy : cy - dy;
            if (y < top || y >= bottom)
                continue;
            /* Interior span (skipped on the outermost rows, where the rim is the
             * whole span and the brush would paint over the pen). */
            if (dy < ry && x1 - x0 > 1 && !PatBlt(hdc, (int)(x0 + 1), (int)y,
                                                  (int)(x1 - x0 - 1), 1, PATCOPY))
                return FALSE;
            /* Rim: this row's endpoint run, widened to meet the previous row. */
            INT64 lx = p0 < x0 ? p0 : x0, lw = (x0 > p0 ? x0 : p0) - lx + 1;
            INT64 rl = p1 < x1 ? p1 : x1, rw = (x1 > p1 ? x1 : p1) - rl + 1;
            if (!MoveToEx(hdc, (int)lx, (int)y, NULL) || !LineTo(hdc, (int)(lx + lw - 1), (int)y))
                return FALSE;
            if (!MoveToEx(hdc, (int)rl, (int)y, NULL) || !LineTo(hdc, (int)(rl + rw - 1), (int)y))
                return FALSE;
        }
        previous = half;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

static int sv_wcslen(const WCHAR *text)
{
    int count = 0;
    if (!text)
        return 0;
    while (text[count])
        count++;
    return count;
}

GDI32_API BOOL ExtTextOutW(HDC hdc, int x, int y, UINT options, const RECT *clip,
                           const WCHAR *text, UINT count, const int *dx)
{
    if (!text)
        return FALSE;
    if ((int)count < 0)
        count = (UINT)sv_wcslen(text);
    /* ETO_OPAQUE is what makes the kernel fill the background with the DC
     * background colour; ETO_CLIPPED is accepted and ignored because the kernel
     * clips every glyph to the DC rectangle anyway. The dx array is accepted and
     * ignored: the kernel's text is a fixed-cell bitmap font with no per-glyph
     * positioning (kernel/win32k/gdi/Drawing.inc). */
    sv_flush_dib(hdc);
    BOOL result=NtGdiExtTextOutW(hdc,x,y,(ULONG)options,clip,text,(int)count,dx,0)!=0;
    sv_read_dib(hdc);return result;
}

GDI32_API BOOL TextOutW(HDC hdc, int x, int y, const WCHAR *text, int count)
{
    if (!text)
        return FALSE;
    if (count < 0)
        count = sv_wcslen(text);
    /* TextOut is ExtTextOut with the DC's background mode deciding whether the
     * background is painted. */
    UINT options = dc_bk_mode(hdc) == OPAQUE ? ETO_OPAQUE : 0u;
    return ExtTextOutW(hdc,x,y,options,NULL,text,(UINT)count,NULL);
}

GDI32_API BOOL GetTextExtentPoint32W(HDC hdc, const WCHAR *text, int count,
                                     LPSIZE size)
{
    if (!size)
        return FALSE;
    if (!text)
        return FALSE;
    if (count < 0)
        count = sv_wcslen(text);
    return NtGdiGetTextExtent(hdc, text, count, size, 0) != 0;
}

GDI32_API BOOL GetCharWidth32W(HDC hdc, UINT first, UINT last, int *widths)
{
    if (last < first || !widths || last - first > 4095) {
        RtlSetLastWin32Error(87);
        return FALSE;
    }
    for (UINT code = first; code <= last; ++code) {
        WCHAR character = (WCHAR)code;
        SIZE extent;
        if (!GetTextExtentPoint32W(hdc, &character, 1, &extent))
            return FALSE;
        widths[code - first] = extent.cx;
    }
    return TRUE;
}

GDI32_API BOOL GetTextMetricsW(HDC hdc, LPTEXTMETRICW metrics)
{
    if (!metrics)
        return FALSE;
    /* The kernel validates the size argument, so it has to be the real
     * TEXTMETRICW size (60 bytes on x64). */
    return NtGdiGetTextMetricsW(hdc, metrics, (ULONG)sizeof(TEXTMETRICW)) != 0;
}

static int draw_text(HDC hdc, const WCHAR *text, int count, LPRECT rect,
                     UINT format, UINT *processed)
{
    /* Win32's DrawText is a formatting routine, not a kernel primitive: it
     * breaks the string into lines and calls ExtTextOut for each one. This
     * implements the flags a plain editor or label needs, including end
     * ellipsis for single-line labels. Path/word ellipses, tab stops and
     * prefix/accelerator handling remain unsupported. */
    if (!hdc || !text || !rect)
        return 0;
    if (count < 0)
        count = sv_wcslen(text);
    if (format & (0x00004000u /* DT_PATH_ELLIPSIS */ |
                  0x00000080u /* DT_TABSTOP */ | 0x00040000u /* DT_WORD_ELLIPSIS */ |
                  0x00010000u /* DT_MODIFYSTRING */))
        return 0;

    TEXTMETRICW metrics;
    int line_height = 20;
    if (GetTextMetricsW(hdc, &metrics) && metrics.tmHeight > 0)
        line_height = (int)metrics.tmHeight;

    int box_width = (int)(rect->right - rect->left);
    if (box_width < 0)
        box_width = 0;
    int box_height = (int)(rect->bottom - rect->top);

    if (format & DT_SINGLELINE) {
        if (processed) *processed = (UINT)count;
        SIZE extent = {0, 0};
        GetTextExtentPoint32W(hdc, text, count, &extent);
        int draw_count=count;
        BOOL ellipsize=(format&DT_END_ELLIPSIS)&&extent.cx>box_width;
        SIZE prefix_extent=extent,dots_extent={0,0};
        static const WCHAR dots[3]={L'.',L'.',L'.'};
        if(ellipsize){
            GetTextExtentPoint32W(hdc,dots,3,&dots_extent);
            int low=0,high=count;
            while(low<high){int mid=low+(high-low+1)/2;SIZE prefix={0,0};
                GetTextExtentPoint32W(hdc,text,mid,&prefix);
                if(prefix.cx+dots_extent.cx<=box_width)low=mid;else high=mid-1;}
            draw_count=low;GetTextExtentPoint32W(hdc,text,draw_count,&prefix_extent);
            extent.cx=prefix_extent.cx+dots_extent.cx;
        }
        int x = (int)rect->left;
        if (format & DT_CENTER)
            x = (int)rect->left + (box_width - (int)extent.cx) / 2;
        else if (format & DT_RIGHT)
            x = (int)rect->right - (int)extent.cx;
        int y = (int)rect->top;
        if (format & DT_VCENTER)
            y = (int)rect->top + (box_height - line_height) / 2;
        else if (format & DT_BOTTOM)
            y = (int)rect->bottom - line_height;
        if (format & DT_CALCRECT) {
            rect->right = x + extent.cx;
            rect->bottom = y + line_height;
            return line_height;
        }
        /* The caller's rectangle is a clipping box, not an output: only
         * DT_CALCRECT is allowed to write to it. */
        UINT options = format & DT_NOCLIP ? 0 : ETO_CLIPPED;
        ExtTextOutW(hdc, x, y, options, rect, text, (UINT)draw_count, NULL);
        if (ellipsize)
            ExtTextOutW(hdc, x + prefix_extent.cx, y, options, rect, dots, 3, NULL);
        return line_height;
    }

    /* Multi-line: split at newlines and, when DT_WORDBREAK is set, at the last
     * space that fits the box. */
    int y = (int)rect->top;
    int lines = 0;
    int index = 0;
    while (index < count) {
        int line_end = index;
        int last_space = -1;
        SIZE extent = {0, 0};
        while (line_end < count && text[line_end] != '\n') {
            if (text[line_end] == ' ')
                last_space = line_end;
            GetTextExtentPoint32W(hdc, text + index, line_end - index + 1, &extent);
            if ((format & DT_WORDBREAK) && extent.cx > box_width && line_end > index) {
                if (last_space > index)
                    line_end = last_space;
                break;
            }
            line_end++;
        }
        int line_length = line_end - index;
        if (!(format & DT_CALCRECT) && y + line_height > (int)rect->top &&
            (y < (int)rect->bottom || (format & DT_NOCLIP))) {
            SIZE line_extent={0,0};
            GetTextExtentPoint32W(hdc,text+index,line_length,&line_extent);
            int x=(int)rect->left;
            if(format&DT_RIGHT)x=(int)rect->right-line_extent.cx;
            else if(format&DT_CENTER)x+=(box_width-line_extent.cx)/2;
            UINT options = format & DT_NOCLIP ? 0 : ETO_CLIPPED;
            ExtTextOutW(hdc, x, y, options, rect, text + index,
                        (UINT)line_length, NULL);
        }
        y += line_height;
        lines++;
        index = line_end;
        while (index < count && (text[index] == '\n' || text[index] == ' '))
            index++;
        if (y + line_height > (int)rect->bottom && !(format & DT_CALCRECT) &&
            !(format & DT_NOCLIP))
            break;
    }
    if (processed) *processed = (UINT)index;
    if (format & DT_CALCRECT) {
        SIZE widest = {0, 0};
        /* For a calculation pass the widest line decides the box. */
        int start = 0;
        for (int i = 0; i <= count; i++) {
            if (i == count || text[i] == '\n') {
                SIZE extent = {0, 0};
                GetTextExtentPoint32W(hdc, text + start, i - start, &extent);
                if (extent.cx > widest.cx)
                    widest = extent;
                start = i + 1;
            }
        }
        rect->right = rect->left + widest.cx;
        rect->bottom = rect->top + lines * line_height;
    }
    return y - (int)rect->top;
}


GDI32_API int DrawTextW(HDC dc, const WCHAR *text, int count, LPRECT rect,
                        UINT format)
{
    return draw_text(dc, text, count, rect, format, NULL);
}

__declspec(dllexport) int SvGdiDrawTextExW(HDC dc, const WCHAR *text, int count,
    LPRECT rect, UINT format, UINT *processed)
{
    return draw_text(dc, text, count, rect, format, processed);
}
