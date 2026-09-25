/* Window DC caching and device capability services.
 * Included by kernel/win32k/gdiobj.c. */
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
#define W32K_DC_CACHE 64

static UINT64 g_window_dc[W32K_DC_CACHE];

UINT64 W32kAcquireWindowDc(UINT64 hwnd, INT32 x, INT32 y, INT32 width,
                           INT32 height)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (slot >= W32K_DC_CACHE)
        slot = 0;

    UINT64 cached = g_window_dc[slot];
    W32K_DC *dc = cached ? dc_from_handle(cached) : NULL;
    if (dc && dc->Hwnd == hwnd) {
        /* Refresh the geometry: the window may have been moved or resized
         * since the DC was first handed out. */
        dc->OrgX = x;
        dc->OrgY = y;
        dc->ClipX = x;
        dc->ClipY = y;
        dc->ClipW = width < 0 ? 0 : width;
        dc->ClipH = height < 0 ? 0 : height;
        return cached;
    }

    UINT64 hdc = W32kCreateWindowDc(hwnd, x, y, width, height);
    if (hdc)
        g_window_dc[slot] = hdc;
    return hdc;
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
    INT32 width = surface ? (INT32)surface->Width : (INT32)GfxFramebuffer.Width;
    INT32 height = surface ? (INT32)surface->Height
                           : (INT32)GfxFramebuffer.Height;

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
