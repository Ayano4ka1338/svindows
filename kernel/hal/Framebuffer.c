/*
 * hal/Framebuffer.c - linear framebuffer graphics and a text console.
 *
 * Draws directly to the GRUB-provided 32-bpp framebuffer: pixels, rectangles,
 * bitmap-font glyphs, and a scrolling text console the kernel log renders into
 * once graphics are up.
 */
#include <svindows/Gfx.h>
#include <svindows/Mm.h>
#include <svindows/Ke.h>
#include <svindows/Rtl.h>

extern const UINT8 Font9x20[95][20][5];

GFX_FRAMEBUFFER GfxFramebuffer;

BOOLEAN GfxAvailable(void)
{
    return GfxFramebuffer.Present && GfxFramebuffer.Base != NULL;
}

UINT32 GfxColor(UINT8 r, UINT8 g, UINT8 b)
{
    return ((UINT32)r << GfxFramebuffer.RedShift) |
           ((UINT32)g << GfxFramebuffer.GreenShift) |
           ((UINT32)b << GfxFramebuffer.BlueShift);
}

BOOLEAN GfxInitialize(void)
{
    if (!GfxFramebuffer.Present || GfxFramebuffer.Bpp != 32)
        return FALSE;

    /* Map the framebuffer into the direct-map window (it lives above RAM). */
    UINT64 phys = GfxFramebuffer.PhysAddr;
    UINT64 size = (UINT64)GfxFramebuffer.Pitch * GfxFramebuffer.Height;
    for (UINT64 off = 0; off < size; off += PAGE_SIZE) {
        if (!MmMapPage((UINT64)(ULONG_PTR)MmPhysToVirt(phys + off), phys + off,
                       PTE_WRITE)) {
            GfxFramebuffer.Base = NULL;
            return FALSE;
        }
    }

    GfxFramebuffer.Base = (UINT32 *)MmPhysToVirt(phys);

    KeLog("[gfx]  framebuffer %ux%u x%u bpp @ 0x%lx -> %p\n",
          GfxFramebuffer.Width, GfxFramebuffer.Height, GfxFramebuffer.Bpp,
          (unsigned long)phys, (void *)GfxFramebuffer.Base);
    return TRUE;
}

static ALWAYS_INLINE UINT32 stride_px(void)
{
    return GfxFramebuffer.Pitch / 4;
}

void GfxPutPixel(UINT32 x, UINT32 y, UINT32 color)
{
    if (GfxAvailable() && x < GfxFramebuffer.Width && y < GfxFramebuffer.Height)
        GfxFramebuffer.Base[y * stride_px() + x] = color;
}

void GfxFillRect(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 color)
{
    if (!GfxAvailable())
        return;
    if (x >= GfxFramebuffer.Width || y >= GfxFramebuffer.Height)
        return;
    UINT32 x1 = x + (w > GfxFramebuffer.Width - x ?
                     GfxFramebuffer.Width - x : w);
    UINT32 y1 = y + (h > GfxFramebuffer.Height - y ?
                     GfxFramebuffer.Height - y : h);

    UINT32 sp = stride_px();
    for (UINT32 yy = y; yy < y1; yy++) {
        UINT32 *row = GfxFramebuffer.Base + yy * sp;
        for (UINT32 xx = x; xx < x1; xx++)
            row[xx] = color;
    }
}

void GfxClear(UINT32 color)
{
    GfxFillRect(0, 0, GfxFramebuffer.Width, GfxFramebuffer.Height, color);
}

static void gfx_draw_char(UINT32 x, UINT32 y, char c, UINT32 fg, UINT32 bg,
                           BOOLEAN overlay)
{
    if (!GfxAvailable() || x >= GfxFramebuffer.Width ||
        y >= GfxFramebuffer.Height)
        return;
    UINT8 uc = (UINT8)c;
    const UINT8 (*glyph)[5] =
        (uc >= 32 && uc < 127) ? Font9x20[uc - 32] : Font9x20[0];

    /* Expanding the sixteen possible coverage levels once per glyph keeps
     * the inner pixel loop small while still giving curved strokes smooth
     * edges on the 32-bpp framebuffer. */
    UINT32 shades[16];
    UINT32 fr = (fg >> GfxFramebuffer.RedShift) & 0xff;
    UINT32 fgreen = (fg >> GfxFramebuffer.GreenShift) & 0xff;
    UINT32 fb = (fg >> GfxFramebuffer.BlueShift) & 0xff;
    UINT32 br = (bg >> GfxFramebuffer.RedShift) & 0xff;
    UINT32 bgreen = (bg >> GfxFramebuffer.GreenShift) & 0xff;
    UINT32 bb = (bg >> GfxFramebuffer.BlueShift) & 0xff;
    for (UINT32 alpha = 0; alpha < 16; alpha++) {
        UINT32 r = (fr * alpha + br * (15 - alpha) + 7) / 15;
        UINT32 green =
            (fgreen * alpha + bgreen * (15 - alpha) + 7) / 15;
        UINT32 b = (fb * alpha + bb * (15 - alpha) + 7) / 15;
        shades[alpha] = GfxColor((UINT8)r, (UINT8)green, (UINT8)b);
    }

    UINT32 sp = stride_px();
    for (UINT32 row = 0; row < GFX_FONT_H &&
         row < GfxFramebuffer.Height - y; row++) {
        UINT32 *dst = GfxFramebuffer.Base + (y + row) * sp + x;
        for (UINT32 col = 0; col < GFX_FONT_W &&
             col < GfxFramebuffer.Width - x; col++) {
            UINT8 packed = glyph[row][col / 2];
            UINT8 alpha = (col & 1) ? packed >> 4 : packed & 0x0f;
            if (!overlay || alpha)
                dst[col] = shades[alpha];
        }
    }
}

void GfxDrawChar(UINT32 x, UINT32 y, char c, UINT32 fg, UINT32 bg)
{
    gfx_draw_char(x, y, c, fg, bg, FALSE);
}

void GfxDrawCharOverlay(UINT32 x, UINT32 y, char c, UINT32 fg, UINT32 bg)
{
    gfx_draw_char(x, y, c, fg, bg, TRUE);
}

void GfxDrawString(UINT32 x, UINT32 y, const char *s, UINT32 fg, UINT32 bg)
{
    while (*s) {
        GfxDrawChar(x, y, *s++, fg, bg);
        x += GFX_FONT_W;
    }
}
