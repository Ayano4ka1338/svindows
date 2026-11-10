/* Window DC caching and device capability services.
 * Included by kernel/win32k/Gdiobj.c. */
/* ------------------------------------------------------------------ */
/* Window device contexts                                              */
/* ------------------------------------------------------------------ */

/*
 * Windows keeps a small cache of device contexts and hands the same one back
 * for repeated GetDC calls on a window; ReleaseDC returns it to the cache
 * rather than destroying it. The same model here, for a concrete reason: our
 * ReleaseDC path (a NtUserCallOneParam routine we have not identified yet) does
 * not reach the kernel, so creating a fresh DC per GetDC would burn through the
 * handle table in a few hundred repaints.
 */

UINT64 W32kAcquireWindowDc(UINT64 hwnd, INT32 x, INT32 y, INT32 width,
                           INT32 height)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (slot >= W32K_DC_CACHE)
        slot = 0;

    UINT64 cached = g_window_dc[slot];
    if (!hwnd) {
        cached = 0;
        for (UINT16 i = 1; i < GDI_MAX_OBJECTS; i++) {
            W32K_OBJECT *object = &g_gdi_objects[i];
            if (object->Type == GDI_TYPE_DC && object->Owner == gdi_current_process() &&
                !object->u.Dc.Hwnd && !object->u.Dc.StandaloneDisplay &&
                object->u.Dc.DeviceSurface.IsScreen) {
                cached = ((UINT64)object->Upper << 16) | i;
                break;
            }
        }
    }
    W32K_OBJECT *cached_object = cached ? gdi_object(cached, GDI_TYPE_DC) : NULL;
    W32K_DC *dc = cached_object ? &cached_object->u.Dc : NULL;
    if (dc && dc->Hwnd == hwnd) {
        /* Resize failure leaves the existing DC and completed raster intact;
         * do not replace the cache with another DC and orphan its buffers. */
        if (hwnd && !dc_prepare_window(dc)) return 0;
        /* Refresh the geometry: the window may have been moved or resized
         * since the DC was first handed out. */
        if (!hwnd) {
            dc->OrgX = x;
            dc->OrgY = y;
            dc->ClipX = x;
            dc->ClipY = y;
            dc->ClipW = width < 0 ? 0 : width;
            dc->ClipH = height < 0 ? 0 : height;
        }
        return cached;
    }

    UINT64 hdc = W32kCreateWindowDc(hwnd, x, y, width, height);
    if (hdc && hwnd)
        g_window_dc[slot] = hdc;
    return hdc;
}

UINT64 SvNtGdiCreateDisplayDC(UINT64 *args)
{
    (void)args;
    UINT64 hdc = W32kCreateWindowDc(0, 0, 0, GfxFramebuffer.Width,
                                    GfxFramebuffer.Height);
    W32K_OBJECT *object = hdc ? gdi_object(hdc, GDI_TYPE_DC) : NULL;
    if (object) object->u.Dc.StandaloneDisplay = TRUE;
    return hdc;
}

/* A null input origin queries the current mapping. */
UINT64 SvNtGdiViewportOrigin(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc) return 0;
    const INT32 *input = (const INT32 *)a[1];
    INT32 *previous = (INT32 *)a[2];
    if ((input && !MmProbeForRead(a[1], 2 * sizeof(*input))) ||
        (previous && !MmProbeForWrite(a[2], 2 * sizeof(*previous))))
        return 0;
    INT32 x = 0, y = 0;
    if (input) {
        x = input[0];
        y = input[1];
    }
    UINT64 flags = KiIrqSave();
    INT32 old_x = dc->ViewX, old_y = dc->ViewY;
    if (input) {
        dc->ViewX = x;
        dc->ViewY = y;
    }
    KiIrqRestore(flags);
    if (previous) {
        previous[0] = old_x;
        previous[1] = old_y;
    }
    return 1;
}

static BOOLEAN clip_rect_empty(W32K_CLIP_RECT rect)
{
    return rect.Left >= rect.Right || rect.Top >= rect.Bottom;
}

UINT64 SvNtGdiClipRect(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || a[1] > 1) return 0;
    if (a[1] == 0 && !a[2]) {
        dc->HasAppClip = FALSE;
        return 2; /* SIMPLEREGION */
    }
    W32K_CLIP_RECT rect;
    if (!MmProbeForRead(a[2], sizeof(rect))) return 0;
    memcpy(&rect, (const void *)a[2], sizeof(rect));

    if (a[1] == 0) {
        dc->AppClip = rect;
        dc->HasAppClip = TRUE;
        return clip_rect_empty(rect) ? 1 : 2;
    }

    W32K_CLIP_RECT clip = dc->AppClip;
    if (!dc->HasAppClip) {
        INT32 x = dc->ClipX - dc->OrgX, y = dc->ClipY - dc->OrgY;
        clip = (W32K_CLIP_RECT){x, y, x + dc->ClipW, y + dc->ClipH};
    }
    if (clip_rect_empty(clip)) return 1; /* NULLREGION */
    if (clip_rect_empty(rect)) return 2;

    INT64 left = (INT64)rect.Left + dc->ViewX;
    INT64 top = (INT64)rect.Top + dc->ViewY;
    INT64 right = (INT64)rect.Right + dc->ViewX;
    INT64 bottom = (INT64)rect.Bottom + dc->ViewY;
    if (left >= clip.Right || top >= clip.Bottom ||
        right <= clip.Left || bottom <= clip.Top)
        return 2;

    if (left <= clip.Left && right >= clip.Right) {
        if (top <= clip.Top && bottom >= clip.Bottom)
            clip = (W32K_CLIP_RECT){0};
        else if (top <= clip.Top)
            clip.Top = (INT32)bottom;
        else if (bottom >= clip.Bottom)
            clip.Bottom = (INT32)top;
        else
            return 0;
    } else if (top <= clip.Top && bottom >= clip.Bottom) {
        if (left <= clip.Left)
            clip.Left = (INT32)right;
        else if (right >= clip.Right)
            clip.Right = (INT32)left;
        else
            return 0;
    } else {
        /* A hole or L-shaped clip requires a multi-rectangle region. */
        return 0;
    }
    dc->AppClip = clip;
    dc->HasAppClip = TRUE;
    return clip_rect_empty(clip) ? 1 : 2;
}

void W32kDestroyWindowDc(UINT64 hwnd)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (slot >= W32K_DC_CACHE)
        return;
    if (g_window_dc[slot]) {
        W32kReleaseDc(g_window_dc[slot]);
        g_window_dc[slot] = 0;
    }
}

void W32kCleanupWindowDc(PKPROCESS process, UINT64 hwnd)
{
    UINT16 slot = (UINT16)hwnd;
    if (!slot || slot >= W32K_DC_CACHE) return;
    UINT64 handle = g_window_dc[slot];
    UINT16 index = (UINT16)handle;
    if (!index || index >= GDI_MAX_OBJECTS) return;
    W32K_OBJECT *object = &g_gdi_objects[index];
    if (object->Type != GDI_TYPE_DC || object->Owner != process ||
        object->u.Dc.Hwnd != hwnd || object->Upper != (UINT16)(handle >> 16)) return;
    (void)W32kClearSharedHandle(process, TRUE, index);
    dc_dispose_window(&object->u.Dc);
    object->Type = 0;
    object->Owner = NULL;
    object->UserInfo = 0;
    g_window_dc[slot] = 0;
}

/* ------------------------------------------------------------------ */
/* Services: device capabilities                                       */
/* ------------------------------------------------------------------ */

/* GetDeviceCaps indices. */
#define DEVCAP_DRIVERVERSION 0
#define DEVCAP_TECHNOLOGY    2
#define DEVCAP_HORZSIZE      4
#define DEVCAP_VERTSIZE      6
#define DEVCAP_HORZRES       8
#define DEVCAP_VERTRES       10
#define DEVCAP_BITSPIXEL     12
#define DEVCAP_PLANES        14
#define DEVCAP_NUMBRUSHES    16
#define DEVCAP_NUMPENS       18
#define DEVCAP_NUMFONTS      22
#define DEVCAP_NUMCOLORS     24
#define DEVCAP_RASTERCAPS    38
#define DEVCAP_LOGPIXELSX    88
#define DEVCAP_LOGPIXELSY    90
#define DEVCAP_SIZEPALETTE   104
#define DEVCAP_COLORRES      108

/*
 * int NtGdiGetDeviceCaps(HDC, int index)
 *
 * gdi32!GetDeviceCaps only reaches this syscall when the DC's
 * DC_ATTR.ulDirty_ bit 0x10000 is clear; with it set it reads a cached
 * pGdiDevCaps blob instead. A freshly zeroed DC_ATTR takes the syscall path,
 * which is the one we can answer honestly from the framebuffer.
 *
 * LOGPIXELSY matters more than it looks: a program that asks for it to size a
 * font and gets 0 computes a zero-height font, fails to create it, and gives up
 * before showing a window.
 */
UINT64 NtGdiGetDeviceCaps(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc)
        return 0;
    W32K_SURFACE *surface = dc->Surface;
    INT32 width = surface && !dc->Hwnd ? (INT32)surface->Width :
                                        (INT32)GfxFramebuffer.Width;
    INT32 height = surface && !dc->Hwnd ? (INT32)surface->Height :
                                         (INT32)GfxFramebuffer.Height;

    switch ((UINT32)a[1]) {
    case DEVCAP_DRIVERVERSION: return 0x4000;
    case DEVCAP_TECHNOLOGY:    return 1;      /* DT_RASDISPLAY */
    /* Physical size in millimetres at 96 DPI: 25.4 mm per inch. */
    case DEVCAP_HORZSIZE:      return (UINT64)(width * 254 / 960);
    case DEVCAP_VERTSIZE:      return (UINT64)(height * 254 / 960);
    case DEVCAP_HORZRES:       return (UINT64)width;
    case DEVCAP_VERTRES:       return (UINT64)height;
    case DEVCAP_BITSPIXEL:     return 32;
    case DEVCAP_PLANES:        return 1;
    case DEVCAP_NUMBRUSHES:    return (UINT64)-1;
    case DEVCAP_NUMPENS:       return (UINT64)-1;
    case DEVCAP_NUMFONTS:      return 1;
    case DEVCAP_NUMCOLORS:     return (UINT64)-1; /* more than 8 bpp */
    /* RC_BITBLT | RC_BITMAP64 | RC_GDI20_OUTPUT | RC_DI_BITMAP | RC_PALETTE
     * is what a plain 32-bpp raster display reports; leave out palette and
     * stretching we do not implement. */
    case DEVCAP_RASTERCAPS:    return 0x0001 | 0x0002 | 0x0010 | 0x0080;
    case DEVCAP_LOGPIXELSX:    return 96;
    case DEVCAP_LOGPIXELSY:    return 96;
    case DEVCAP_SIZEPALETTE:   return 0;
    case DEVCAP_COLORRES:      return 24;
    }
    return 0;
}

/* int NtGdiGetCurrentDpiInfo(HDC, DPI_INFO *out): the shape of DPI_INFO is not
 * public. Report failure rather than filling a structure we would be guessing
 * at; callers fall back to GetDeviceCaps(LOGPIXELS*). */
UINT64 NtGdiGetCurrentDpiInfo(UINT64 *a)
{
    return 0;
}
