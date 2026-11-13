/* Composition target and clipped chrome rasterization. The hardware buffer
 * receives only finished damage rectangles, never intermediate backdrop,
 * shadows or frames. Included by UserScreen.inc. */
#include "Raster.h"
static UINT32 *g_w32k_composite;
static UINT32 g_w32k_composite_w, g_w32k_composite_h;
static INT32 g_w32k_clip_x, g_w32k_clip_y, g_w32k_clip_w, g_w32k_clip_h;
static INT32 g_cursor_last_x, g_cursor_last_y, g_cursor_last_w, g_cursor_last_h;
static BOOLEAN g_cursor_last_valid;

/* The retained scene is pointer-free. Blend the pointer only into native
 * scanout so repaint, movement and X11 export cannot bake it into a window. */
static void W32kCursorOverlay(INT32 x, INT32 y, INT32 width, INT32 height)
{
    W32K_CURSOR_IMAGE *image = W32kSelectedCursor();
    if (!g_w32k_pointer_valid || !image) return;
    const W32K_CURSOR_DESCRIPTION *shape = &image->Description;
    INT32 left = g_w32k_pointer_x - (INT32)shape->HotX;
    INT32 top = g_w32k_pointer_y - (INT32)shape->HotY;
    INT32 begin_x = left > x ? left : x, begin_y = top > y ? top : y;
    INT32 end_x = left + (INT32)shape->Width < x + width ?
                      left + (INT32)shape->Width : x + width;
    INT32 end_y = top + (INT32)shape->Height < y + height ?
                      top + (INT32)shape->Height : y + height;
    if (begin_x >= end_x || begin_y >= end_y) return;
    for (INT32 row = begin_y; row < end_y; ++row)
        for (INT32 col = begin_x; col < end_x; ++col) {
            SIZE_T source = (SIZE_T)(row - top) * shape->Width + col - left;
            UINT32 pixel = image->Pixels[source];
            UINT32 under = g_w32k_composite[(SIZE_T)row * g_w32k_composite_w + col];
            UINT32 result;
            if (shape->Flags & W32K_CURSOR_ALPHA) {
                UINT32 alpha = pixel >> 24, inverse = 255 - alpha;
                UINT8 red = (UINT8)((((pixel >> 16) & 255) * alpha +
                          ((under >> GfxFramebuffer.RedShift) & 255) * inverse + 127) / 255);
                UINT8 green = (UINT8)((((pixel >> 8) & 255) * alpha +
                          ((under >> GfxFramebuffer.GreenShift) & 255) * inverse + 127) / 255);
                UINT8 blue = (UINT8)(((pixel & 255) * alpha +
                          ((under >> GfxFramebuffer.BlueShift) & 255) * inverse + 127) / 255);
                result = GfxColor(red, green, blue);
            } else {
                UINT32 rgb = (((under >> GfxFramebuffer.RedShift) & 255) << 16) |
                             (((under >> GfxFramebuffer.GreenShift) & 255) << 8) |
                             ((under >> GfxFramebuffer.BlueShift) & 255);
                rgb = (rgb & (image->And[source] ? 0xffffffu : 0)) ^ (pixel & 0xffffffu);
                result = GfxColor((UINT8)(rgb >> 16), (UINT8)(rgb >> 8), (UINT8)rgb);
            }
            GfxFramebuffer.Base[(SIZE_T)row * (GfxFramebuffer.Pitch / 4) + col] = result;
        }
    g_cursor_last_x = left; g_cursor_last_y = top;
    g_cursor_last_w = shape->Width; g_cursor_last_h = shape->Height;
    g_cursor_last_valid = TRUE;
}

static void W32kCursorRefresh(void)
{
    if (!GfxAvailable() || !g_w32k_composite || W32kXorgConnected()) return;
    W32K_CURSOR_IMAGE *image = g_w32k_pointer_valid ? W32kSelectedCursor() : NULL;
    INT32 x = g_cursor_last_x, y = g_cursor_last_y;
    INT32 right = x + g_cursor_last_w, bottom = y + g_cursor_last_h;
    BOOLEAN old_valid = g_cursor_last_valid;
    if (image) {
        INT32 new_x = g_w32k_pointer_x - (INT32)image->Description.HotX;
        INT32 new_y = g_w32k_pointer_y - (INT32)image->Description.HotY;
        INT32 new_right = new_x + (INT32)image->Description.Width;
        INT32 new_bottom = new_y + (INT32)image->Description.Height;
        if (!old_valid) { x = new_x; y = new_y; right = new_right; bottom = new_bottom; }
        else {
            if (new_x < x) x = new_x;
            if (new_y < y) y = new_y;
            if (new_right > right) right = new_right;
            if (new_bottom > bottom) bottom = new_bottom;
        }
    }
    g_cursor_last_valid = FALSE;
    if (old_valid || image) W32kComposeRect(x, y, right - x, bottom - y);
}

static BOOLEAN W32kCursorPosition(INT32 *x, INT32 *y)
{
    *x = g_w32k_pointer_x; *y = g_w32k_pointer_y;
    return g_w32k_pointer_valid;
}

static BOOLEAN W32kEnsureComposite(void)
{
    UINT32 width = GfxFramebuffer.Width, height = GfxFramebuffer.Height;
    if (g_w32k_composite && g_w32k_composite_w == width &&
        g_w32k_composite_h == height) return TRUE;
    if (!width || !height || width > 4096 || height > 2160) return FALSE;
    UINT32 *pixels = ExAllocatePoolWithTag(NonPagedPool,
                          (SIZE_T)width * height * sizeof(UINT32), 'pmoC');
    if (!pixels) return FALSE;
    if (g_w32k_composite) ExFreePool(g_w32k_composite);
    g_w32k_composite = pixels;
    g_w32k_composite_w = width;
    g_w32k_composite_h = height;
    return TRUE;
}

static void W32kRasterPixel(INT32 x, INT32 y, UINT32 pixel)
{
    if (x < g_w32k_clip_x || y < g_w32k_clip_y ||
        x >= g_w32k_clip_x + g_w32k_clip_w ||
        y >= g_w32k_clip_y + g_w32k_clip_h) return;
    g_w32k_composite[(UINT64)y * g_w32k_composite_w + x] = pixel;
}

static void W32kRasterFill(INT32 x, INT32 y, INT32 width, INT32 height,
                           UINT32 pixel)
{
    if (x < g_w32k_clip_x) { width -= g_w32k_clip_x - x; x = g_w32k_clip_x; }
    if (y < g_w32k_clip_y) { height -= g_w32k_clip_y - y; y = g_w32k_clip_y; }
    if (x + width > g_w32k_clip_x + g_w32k_clip_w)
        width = g_w32k_clip_x + g_w32k_clip_w - x;
    if (y + height > g_w32k_clip_y + g_w32k_clip_h)
        height = g_w32k_clip_y + g_w32k_clip_h - y;
    if (width <= 0 || height <= 0) return;
    for (INT32 row = y; row < y + height; ++row) {
        UINT32 *target = g_w32k_composite + (UINT64)row * g_w32k_composite_w + x;
        for (INT32 col = 0; col < width; ++col) target[col] = pixel;
    }
}

/* Darken the already composed backdrop; shadows never replace it with a
 * solid black frame. Alpha is fixed point [0,256], with bounded damage clips. */
static void W32kRasterShadow(INT32 x, INT32 y, INT32 width, INT32 height, UINT32 alpha)
{
    if (x < g_w32k_clip_x) { width -= g_w32k_clip_x-x; x = g_w32k_clip_x; }
    if (y < g_w32k_clip_y) { height -= g_w32k_clip_y-y; y = g_w32k_clip_y; }
    if (x+width > g_w32k_clip_x+g_w32k_clip_w) width = g_w32k_clip_x+g_w32k_clip_w-x;
    if (y+height > g_w32k_clip_y+g_w32k_clip_h) height = g_w32k_clip_y+g_w32k_clip_h-y;
    if (width <= 0 || height <= 0) return;
    UINT32 inverse = 256-alpha;
    for (INT32 row = y; row < y+height; ++row) {
        UINT32 *target = g_w32k_composite+(UINT64)row*g_w32k_composite_w+x;
        for (INT32 col = 0; col < width; ++col) {
            UINT32 under = target[col];
            target[col] = GfxColor(
                (UINT8)((((under >> GfxFramebuffer.RedShift)&255)*inverse) >> 8),
                (UINT8)((((under >> GfxFramebuffer.GreenShift)&255)*inverse) >> 8),
                (UINT8)((((under >> GfxFramebuffer.BlueShift)&255)*inverse) >> 8));
        }
    }
}

static void W32kRasterText(INT32 x, INT32 baseline, const WCHAR *text,
                           UINT32 color, INT32 max_width, INT32 top,
                           INT32 height)
{
    INT32 text_left = x, text_right = x + max_width;
    if (max_width <= 0 || height <= 0) return;
    for (UINT32 i = 0; text[i]; ++i) {
        const GFX_FONT_GLYPH *glyph = GfxUiGlyph(text[i]);
        if (glyph) {
            const UINT8 *bits = SvUiFontUiFace.Pixels + glyph->Offset;
            INT32 left = x + glyph->BearingX, glyph_top = baseline - glyph->BearingY;
            for (UINT32 row = 0; row < glyph->Height; ++row) {
                INT32 py = glyph_top + (INT32)row;
                if (py < top || py >= top + height ||
                    py < g_w32k_clip_y || py >= g_w32k_clip_y + g_w32k_clip_h)
                    continue;
                for (UINT32 col = 0; col < glyph->Width; ++col) {
                    INT32 px = left + (INT32)col;
                    UINT32 alpha = bits[(UINT64)row * glyph->Width + col];
                    if (!alpha || px < text_left || px >= text_right ||
                        px < g_w32k_clip_x ||
                        px >= g_w32k_clip_x + g_w32k_clip_w) continue;
                    UINT32 *target = g_w32k_composite +
                                     (UINT64)py * g_w32k_composite_w + px;
                    UINT32 under = *target, inverse = 255 - alpha;
                    UINT32 red = (((color >> 16) & 255) * alpha +
                        ((under >> GfxFramebuffer.RedShift) & 255) * inverse + 127) / 255;
                    UINT32 green = (((color >> 8) & 255) * alpha +
                        ((under >> GfxFramebuffer.GreenShift) & 255) * inverse + 127) / 255;
                    UINT32 blue = ((color & 255) * alpha +
                        ((under >> GfxFramebuffer.BlueShift) & 255) * inverse + 127) / 255;
                    *target = GfxColor((UINT8)red, (UINT8)green, (UINT8)blue);
                }
            }
        }
        x += (INT32)GfxUiCharWidth(text[i]);
    }
}

static void W32kPresentDamage(INT32 x, INT32 y, INT32 width, INT32 height)
{
    /* Once Xorg is connected it owns the physical framebuffer. Win32 windows
     * are presented only through their managed X11 clients. */
    if (W32kXorgConnected())
        return;
    for (INT32 row = y; row < y + height; ++row)
        W32kCopyPixels(GfxFramebuffer.Base + (UINT64)row * (GfxFramebuffer.Pitch / 4) + x,
               g_w32k_composite + (UINT64)row * g_w32k_composite_w + x,
               (SIZE_T)width);
    W32kCursorOverlay(x, y, width, height);
}
