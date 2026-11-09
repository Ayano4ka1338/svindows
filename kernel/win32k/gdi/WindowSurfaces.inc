/* Retained client rasters. GDI writes in client coordinates even while a
 * window moves or is covered. Composition reads only the completed raster;
 * BeginPaint/EndPaint publish an entire paint without exposing intermediate
 * fills, text or partially copied rows. Included by Gdiobj.c. */
#include "../Raster.h"

static void dc_dispose_window(W32K_DC *dc)
{
    void *storage = dc->WindowStorage;
    dc->WindowStorage = NULL;
    dc->WindowPresented = FALSE;
    dc->HasTransparentPixels = FALSE;
    dc->DeviceSurface.Bits = NULL;
    dc->PresentedSurface.Bits = NULL;
    if (storage) ExFreePool(storage);
}

static BOOLEAN dc_prepare_window(W32K_DC *dc)
{
    UINT16 slot = (UINT16)dc->Hwnd;
    if (!W32kWindowExists(slot)) return FALSE;
    INT32 x, y, width, height;
    W32kClientOrigin(slot, &x, &y, &width, &height);
    if (width < 0 || height < 0 || width > 4096 || height > 2160)
        return FALSE;
    if (dc->WindowStorage && dc->DeviceSurface.Width == (UINT32)width &&
        dc->DeviceSurface.Height == (UINT32)height)
        return TRUE;

    SIZE_T pixels = (SIZE_T)width * height;
    UINT32 *storage = pixels ? ExAllocatePoolWithTag(NonPagedPool,
                                  pixels * sizeof(UINT32) * 2, 'fniW') : NULL;
    if (pixels && !storage) return FALSE;
    UINT32 background = dc->HasTransparentPixels ? gdi_inverse_alpha_mask() :
                                                  gdi_pixel(0x00FFFFFF);
    for (SIZE_T i = 0; i < pixels * 2; ++i) storage[i] = background;
    W32K_SURFACE drawing = {storage, (UINT32)width, (UINT32)height,
                            (UINT32)width, FALSE};
    W32K_SURFACE presented = drawing;
    if (storage) presented.Bits = storage + pixels;

    UINT64 flags = KiIrqSave();
    UINT32 copy_w = dc->PresentedSurface.Width < (UINT32)width ?
                   dc->PresentedSurface.Width : (UINT32)width;
    UINT32 copy_h = dc->PresentedSurface.Height < (UINT32)height ?
                   dc->PresentedSurface.Height : (UINT32)height;
    if (dc->WindowStorage)
        for (UINT32 row = 0; row < copy_h; ++row) {
            const UINT32 *old = dc->PresentedSurface.Bits +
                               (UINT64)row * dc->PresentedSurface.Stride;
            W32kCopyPixels(drawing.Bits + (UINT64)row * width, old, copy_w);
            W32kCopyPixels(presented.Bits + (UINT64)row * width, old, copy_w);
        }
    void *old_storage = dc->WindowStorage;
    dc->WindowStorage = storage;
    dc->DeviceSurface = drawing;
    dc->PresentedSurface = presented;
    dc->Surface = &dc->DeviceSurface;
    dc->OrgX = dc->OrgY = dc->ClipX = dc->ClipY = 0;
    dc->ClipW = width;
    dc->ClipH = height;
    dc->Dirty = FALSE;
    KiIrqRestore(flags);
    if (old_storage) ExFreePool(old_storage);
    return TRUE;
}

static void dc_damage(W32K_DC *dc, INT32 x, INT32 y, INT32 w, INT32 h)
{
    if (!dc->Hwnd || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > dc->ClipW) w = dc->ClipW - x;
    if (y + h > dc->ClipH) h = dc->ClipH - y;
    if (w <= 0 || h <= 0) return;
    if (!dc->Dirty) {
        dc->DirtyLeft = x; dc->DirtyTop = y;
        dc->DirtyRight = x + w; dc->DirtyBottom = y + h;
        dc->Dirty = TRUE;
    } else {
        if (x < dc->DirtyLeft) dc->DirtyLeft = x;
        if (y < dc->DirtyTop) dc->DirtyTop = y;
        if (x + w > dc->DirtyRight) dc->DirtyRight = x + w;
        if (y + h > dc->DirtyBottom) dc->DirtyBottom = y + h;
    }
}

static void dc_present(W32K_DC *dc)
{
    if (!dc->Hwnd || dc->Painting || !dc->Dirty) return;
    UINT64 flags = KiIrqSave();
    INT32 left = dc->DirtyLeft, top = dc->DirtyTop;
    INT32 width = dc->DirtyRight - left, height = dc->DirtyBottom - top;
    for (INT32 row = top; row < dc->DirtyBottom; ++row) {
        UINT64 offset = (UINT64)row * dc->DeviceSurface.Stride + left;
        W32kCopyPixels(dc->PresentedSurface.Bits + offset,
                       dc->DeviceSurface.Bits + offset, (SIZE_T)width);
    }
    dc->Dirty = FALSE;
    dc->WindowPresented = TRUE;
    INT32 x, y, cw, ch;
    W32kClientOrigin((UINT16)dc->Hwnd, &x, &y, &cw, &ch);
    W32kScreenDamage(x + left, y + top, width, height);
    KiIrqRestore(flags);
}

BOOLEAN W32kBeginWindowPaint(UINT64 hdc, UINT64 hwnd)
{
    W32K_DC *dc = dc_from_handle(hdc);
    if (!dc || dc->Hwnd != hwnd || dc->Painting) return FALSE;
    dc->Painting = TRUE;
    return TRUE;
}

BOOLEAN W32kEndWindowPaint(UINT64 hdc, UINT64 hwnd)
{
    W32K_DC *dc = dc_from_handle(hdc);
    if (!dc || dc->Hwnd != hwnd || !dc->Painting) return FALSE;
    dc->Painting = FALSE;
    dc_present(dc);
    return TRUE;
}

UINT64 NtGdiClearWindowBackground(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    UINT32 mask = gdi_inverse_alpha_mask();
    if (!dc || !dc->Hwnd || !dc->Painting || !dc->Surface->Bits ||
        !mask || (mask >> __builtin_ctz(mask)) != 255)
        return 0;
    dc->HasTransparentPixels = TRUE;
    surface_fill(dc->Surface, 0, 0, dc->ClipW, dc->ClipH, mask);
    dc_damage(dc, 0, 0, dc->ClipW, dc->ClipH);
    return 1;
}

/* Composition runs on the input worker as well as the owner process. Access
 * kernel rasters directly; never dereference a foreign process's DC_ATTR. */
void W32kCopyWindowClient(UINT16 slot, UINT32 *target, UINT32 stride,
                          INT32 x, INT32 y, INT32 width, INT32 height)
{
    if (!slot || slot >= W32K_DC_CACHE) return;
    UINT64 handle = g_window_dc[slot];
    UINT16 index = (UINT16)handle;
    if (!index || index >= GDI_MAX_OBJECTS) return;
    W32K_OBJECT *object = &g_gdi_objects[index];
    if (object->Type != GDI_TYPE_DC || object->Upper != (UINT16)(handle >> 16) ||
        (UINT16)object->u.Dc.Hwnd != slot) return;
    const W32K_SURFACE *surface = &object->u.Dc.PresentedSurface;
    /* Allocating a DC does not paint the window. An untouched retained
     * buffer must not become an opaque white sibling overlay. */
    if (!surface->Bits || !object->u.Dc.WindowPresented) return;
    INT32 cx, cy, cw, ch;
    W32kClientOrigin(slot, &cx, &cy, &cw, &ch);
    if (cw > (INT32)surface->Width) cw = (INT32)surface->Width;
    if (ch > (INT32)surface->Height) ch = (INT32)surface->Height;
    INT32 left = x > cx ? x : cx, top = y > cy ? y : cy;
    INT32 right = x + width < cx + cw ? x + width : cx + cw;
    INT32 bottom = y + height < cy + ch ? y + height : cy + ch;
    if (right <= left || bottom <= top) return;
    BOOLEAN transparent = object->u.Dc.HasTransparentPixels;
    UINT32 mask = gdi_inverse_alpha_mask();
    UINT32 shift = transparent ? __builtin_ctz(mask) : 0;
    for (INT32 row = top; row < bottom; ++row) {
        UINT32 *dst = target + (UINT64)row * stride + left;
        const UINT32 *src = surface->Bits +
            (UINT64)(row - cy) * surface->Stride + left - cx;
        if (!transparent) {
            W32kCopyPixels(dst, src, (SIZE_T)(right - left));
            continue;
        }
        for (INT32 col = 0; col < right - left; ++col) {
            UINT32 inverse = (src[col] & mask) >> shift;
            if (!inverse) dst[col] = src[col];
            else if (inverse < 255)
                dst[col] = blend_pixel(src[col], dst[col], (UINT8)(255 - inverse));
        }
    }
}
