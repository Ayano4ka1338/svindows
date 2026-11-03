/*
 * hal/Framebuffer.c - linear framebuffer graphics and a text console.
 *
 * Draws directly to the bootloader-provided 32-bpp framebuffer: pixels, rectangles,
 * bitmap-font glyphs, and a scrolling text console the kernel log renders into
 * once graphics are up.
 */
#include <svindows/Gfx.h>
#include <svindows/Mm.h>
#include <svindows/Ke.h>
#include <svindows/Rtl.h>
#include <svindows/Hal.h>
#include <svindows/Multiboot2.h>

extern const UINT8 Font9x20[95][20][5];

GFX_FRAMEBUFFER GfxFramebuffer;

static UINT16 vbe_read(UINT16 index)
{
    __outword(0x1CE, index);
    return __inword(0x1CF);
}

static void vbe_write(UINT16 index, UINT16 value)
{
    __outword(0x1CE, index);
    __outword(0x1CF, value);
}

BOOLEAN GfxRequestBootMode(UINT64 mbi_phys, UINT32 width, UINT32 height)
{
    if (!mbi_phys || width > 0xFFFF || height > 0xFFFF)
        return FALSE;

    MB_INFO_HEADER *header = (MB_INFO_HEADER *)MmPhysToVirt(mbi_phys);
    if (header->total_size < sizeof(*header) + sizeof(MB_TAG) ||
        header->total_size > ~(UINT64)0 - mbi_phys)
        return FALSE;
    UINT64 end = mbi_phys + header->total_size;
    MB_TAG_FRAMEBUFFER *fb = NULL;
    for (UINT64 at = mbi_phys + sizeof(*header); at + sizeof(MB_TAG) <= end;) {
        MB_TAG *tag = (MB_TAG *)MmPhysToVirt(at);
        if (tag->size < sizeof(*tag) || tag->size > end - at) return FALSE;
        if (tag->type == MB_TAG_TYPE_END) break;
        if (tag->type == MB_TAG_TYPE_FRAMEBUFFER &&
            tag->size >= sizeof(MB_TAG_FRAMEBUFFER)) {
            fb = (MB_TAG_FRAMEBUFFER *)tag;
            break;
        }
        at += (tag->size + 7u) & ~7u;
    }
    if (!fb || fb->fb_type != MB_FRAMEBUFFER_TYPE_RGB || vbe_read(0) < 0xB0C4)
        return FALSE;

    /* Bochs/QEMU VBE DISPI. Limine's Multiboot2 mode request is only a hint;
     * set the adapter mode directly and update its framebuffer tag as well. */
    vbe_write(4, 0);
    vbe_write(1, (UINT16)width);
    vbe_write(2, (UINT16)height);
    vbe_write(3, 32);
    vbe_write(4, 0x41); /* enabled, linear framebuffer */
    if (vbe_read(1) != width || vbe_read(2) != height || vbe_read(3) != 32)
        return FALSE;

    fb->width = width;
    fb->height = height;
    fb->pitch = width * 4;
    fb->bpp = 32;
    return TRUE;
}

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
                       PTE_WRITE | PTE_PCD | PTE_PWT)) {
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
