/* Composition target and clipped chrome rasterization. The hardware buffer
 * receives only finished damage rectangles, never intermediate backdrop,
 * shadows or frames. Included by UserScreen.inc. */
#include "Raster.h"
static UINT32 *g_w32k_composite;
static UINT32 g_w32k_composite_w, g_w32k_composite_h;
static INT32 g_w32k_clip_x, g_w32k_clip_y, g_w32k_clip_w, g_w32k_clip_h;

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
}
