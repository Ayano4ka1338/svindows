/*
 * svindows/Gfx.h - linear framebuffer graphics.
 *
 * When GRUB provides a framebuffer (requested in the Multiboot2 header), the
 * kernel draws to it directly: pixels, rectangles, and an 8x16 bitmap-font text
 * console. This is the foundation the desktop is built on.
 */
#ifndef _SVINDOWS_GFX_H_
#define _SVINDOWS_GFX_H_

#include <nt/Ntdef.h>
#include <svindows/UiFont.h>

typedef struct _GFX_FRAMEBUFFER {
    UINT64  PhysAddr;
    UINT32 *Base;      /* mapped virtual address (32-bpp assumed)      */
    UINT32  Pitch;     /* bytes per scanline                           */
    UINT32  Width;
    UINT32  Height;
    UINT8   Bpp;
    UINT8   RedShift;
    UINT8   GreenShift;
    UINT8   BlueShift;
    BOOLEAN Present;
} GFX_FRAMEBUFFER;

extern GFX_FRAMEBUFFER GfxFramebuffer;

/* Pack an (r,g,b) triple into a pixel for the current framebuffer format. */
UINT32 GfxColor(UINT8 r, UINT8 g, UINT8 b);

BOOLEAN GfxAvailable(void);
/* Map the bootloader framebuffer (needs Mm). FALSE means there is no usable
 * framebuffer, either because it was not supplied or one of its mappings
 * could not be installed. */
BOOLEAN GfxInitialize(void);
/* On Bochs/QEMU VBE adapters, select and publish an exact boot display mode. */
BOOLEAN GfxRequestBootMode(UINT64 mbi_phys, UINT32 width, UINT32 height);

void GfxPutPixel(UINT32 x, UINT32 y, UINT32 color);
void GfxFillRect(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 color);
void GfxClear(UINT32 color);
void GfxDrawChar(UINT32 x, UINT32 y, char c, UINT32 fg, UINT32 bg);
/* Draw only the glyph's nonzero coverage, preserving the rest of the cell. */
void GfxDrawCharOverlay(UINT32 x, UINT32 y, char c, UINT32 fg, UINT32 bg);
void GfxDrawString(UINT32 x, UINT32 y, const char *s, UINT32 fg, UINT32 bg);

#define GFX_FONT_W 9
#define GFX_FONT_H 20

/*
 * Proportional UI text (Selawik, the Segoe UI metric-compatible face).
 *
 * The fixed 9x20 console font above belongs to the character console; window
 * chrome and GDI text use the proportional face, whose metrics mirror Segoe UI
 * at 96 DPI so layout numbers can be written the way Windows does.
 */
int    GfxUiAscent(void);
int    GfxUiDescent(void);
int    GfxUiLineHeight(void);
/* Glyph lookup, shared with win32k's GDI: it rasterizes the same atlas into a
 * device context instead of straight onto the framebuffer. */
const GFX_FONT_GLYPH *GfxUiGlyph(UINT32 codepoint);
UINT32 GfxUiCharWidth(UINT32 codepoint);
UINT32 GfxUiTextWidth(const char *text);
UINT32 GfxUiTextWidthW(const UINT16 *text);
UINT32 GfxUiTextWidthCountW(const UINT16 *text, UINT32 count);
UINT32 GfxUiTextFitCountW(const UINT16 *text, UINT32 count, UINT32 max_width,
                          UINT32 *width_out);
/* Draw with the pen origin at x on the baseline; coverage is blended over the
 * existing pixels, so callers paint the background first. */
void   GfxDrawUiText(UINT32 x, UINT32 baseline, const char *text, UINT32 color);
void   GfxDrawUiTextW(UINT32 x, UINT32 baseline, const UINT16 *text,
                      UINT32 color);
/* Largest prefix of text that fits in max_width pixels. */
UINT32 GfxUiTextFitW(const UINT16 *text, UINT32 max_width);

#endif /* _SVINDOWS_GFX_H_ */
