/*
 * svindows/UiFont.h - the proportional UI face inside the kernel.
 *
 * The generated atlas in kernel/hal/UiFontData.c is compiled for win32k.
 */
#ifndef _SVINDOWS_UIFONT_H_
#define _SVINDOWS_UIFONT_H_

#include <nt/Ntdef.h>

typedef struct _GFX_FONT_GLYPH {
    UINT32 Codepoint;
    UINT32 Offset;              /* byte offset into the face's bitmap pool */
    UINT8  Width, Height;
    INT8   BearingX;            /* left edge relative to the pen origin     */
    INT8   BearingY;            /* bitmap top above the baseline            */
    UINT8  Advance;             /* pen step in pixels                       */
} GFX_FONT_GLYPH;

typedef struct _GFX_FONT_FACE {
    const char *Name;
    INT32  EmPixels;
    INT32  Ascent;              /* pixels above the baseline   */
    INT32  Descent;             /* pixels below the baseline   */
    INT32  LineHeight;
    const UINT8 *Pixels;
    const GFX_FONT_GLYPH *Glyphs;
    INT32  GlyphCount;
} GFX_FONT_FACE;

/* 12px em (9pt): window captions, menus, lists, taskbar. */
extern const GFX_FONT_FACE SvUiFontUiFace;
extern const GFX_FONT_FACE SvUiFontUi16Face;
extern const GFX_FONT_FACE SvUiFontUi20Face;
extern const GFX_FONT_FACE SvUiFontUi24Face;
extern const GFX_FONT_FACE SvUiFontUi32Face;
extern const GFX_FONT_FACE SvUiFontUi48Face;
const GFX_FONT_GLYPH *GfxUiGlyph(UINT32 codepoint);
const GFX_FONT_FACE *GfxUiFontForHeight(INT32 height);
const GFX_FONT_GLYPH *GfxUiGlyphForFace(const GFX_FONT_FACE *face,
                                         UINT32 codepoint);
UINT32 GfxUiCharWidthForFace(const GFX_FONT_FACE *face, UINT32 codepoint);
UINT32 GfxUiTextWidthCountForFace(const GFX_FONT_FACE *face,
                                   const UINT16 *text, UINT32 count);
UINT32 GfxUiTextFitCountForFace(const GFX_FONT_FACE *face,
                                 const UINT16 *text, UINT32 count,
                                 UINT32 max_width, UINT32 *width_out);


#endif /* _SVINDOWS_UIFONT_H_ */
