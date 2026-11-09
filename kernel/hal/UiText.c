/*
 * hal/UiText.c - proportional UI text for window chrome and GDI.
 *
 * The console keeps the fixed 9x20 face (hal/Font9x20.c) because a character
 * cell grid is what a console is; everything that is a window - the caption,
 * menus, dialog text, and every string a client draws through TextOut - uses
 * the Selawik atlas, whose advances match Segoe UI at 96 DPI. One atlas is
 * stored in kernel/hal/UiFontData.c, so GDI and window frames share the same
 * glyphs.
 */
#include <svindows/Gfx.h>
#include <svindows/UiFont.h>

int GfxUiAscent(void) { return SvUiFontUiFace.Ascent; }
int GfxUiDescent(void) { return SvUiFontUiFace.Descent; }
int GfxUiLineHeight(void) { return SvUiFontUiFace.LineHeight; }

/* Glyph lookup is shared with win32k's GDI, which rasterizes the same atlas
 * into a device context instead of straight onto the framebuffer. */
const GFX_FONT_GLYPH *GfxUiGlyph(UINT32 codepoint)
{
    return GfxUiGlyphForFace(&SvUiFontUiFace, codepoint);
}

const GFX_FONT_FACE *GfxUiFontForHeight(INT32 height)
{
    if (height < 0) height = -height;
    if (height <= 0) height = SvUiFontUiFace.EmPixels;
    const GFX_FONT_FACE *faces[] = {
        &SvUiFontUiFace, &SvUiFontUi16Face, &SvUiFontUi20Face,
        &SvUiFontUi24Face, &SvUiFontUi32Face, &SvUiFontUi48Face,
    };
    const GFX_FONT_FACE *best = faces[0];
    INT32 distance = best->EmPixels - height;
    if (distance < 0) distance = -distance;
    for (UINT32 i = 1; i < sizeof(faces) / sizeof(faces[0]); ++i) {
        INT32 candidate = faces[i]->EmPixels - height;
        if (candidate < 0) candidate = -candidate;
        if (candidate < distance) { best = faces[i]; distance = candidate; }
    }
    return best;
}

const GFX_FONT_GLYPH *GfxUiGlyphForFace(const GFX_FONT_FACE *face,
                                        UINT32 codepoint)
{
    if (!face) face = &SvUiFontUiFace;
    /* Segoe UI Symbol's private-use backspace slot maps to the standard
     * left-arrow glyph when the chosen face has no private-use glyph. The
     * returned glyph remains part of the shared
     * text path used by GDI, measurement and window chrome. */
    if (codepoint == 0xF013)
        codepoint = 0x2190;
    INT32 low = 0, high = face->GlyphCount - 1;
    while (low <= high) {
        INT32 middle = (low + high) / 2;
        UINT32 value = face->Glyphs[middle].Codepoint;
        if (value == codepoint)
            return &face->Glyphs[middle];
        if (value < codepoint)
            low = middle + 1;
        else
            high = middle - 1;
    }
    return NULL;
}

UINT32 GfxUiCharWidthForFace(const GFX_FONT_FACE *face, UINT32 codepoint)
{
    const GFX_FONT_GLYPH *glyph = GfxUiGlyphForFace(face, codepoint);
    return glyph ? glyph->Advance : (UINT32)((face ? face : &SvUiFontUiFace)->EmPixels / 2);
}

UINT32 GfxUiTextWidthCountForFace(const GFX_FONT_FACE *face,
                                  const UINT16 *text, UINT32 count)
{
    UINT32 width = 0;
    if (text)
        for (UINT32 i = 0; i < count && text[i]; ++i)
            width += GfxUiCharWidthForFace(face, text[i]);
    return width;
}

UINT32 GfxUiTextFitCountForFace(const GFX_FONT_FACE *face,
                                const UINT16 *text, UINT32 count,
                                UINT32 max_width, UINT32 *width_out)
{
    UINT32 width = 0, fit = 0;
    if (text)
        for (; fit < count && text[fit]; ++fit) {
            UINT32 advance = GfxUiCharWidthForFace(face, text[fit]);
            if (width + advance > max_width) break;
            width += advance;
        }
    if (width_out) *width_out = width;
    return fit;
}

UINT32 GfxUiCharWidth(UINT32 codepoint)
{
    const GFX_FONT_GLYPH *glyph = GfxUiGlyph(codepoint);
    return glyph ? glyph->Advance : (UINT32)(SvUiFontUiFace.EmPixels / 2);
}

/* Width of a counted run: what every GDI text call has on hand. */
UINT32 GfxUiTextWidthCountW(const UINT16 *text, UINT32 count)
{
    UINT32 width = 0;
    if (!text)
        return 0;
    for (UINT32 i = 0; i < count && text[i]; ++i)
        width += GfxUiCharWidth(text[i]);
    return width;
}

/* Longest prefix that fits, with its width: GetTextExtentExW's contract. */
UINT32 GfxUiTextFitCountW(const UINT16 *text, UINT32 count, UINT32 max_width,
                          UINT32 *width_out)
{
    UINT32 width = 0, fit = 0;
    if (text)
        for (; fit < count && text[fit]; ++fit) {
            UINT32 advance = GfxUiCharWidth(text[fit]);
            if (width + advance > max_width)
                break;
            width += advance;
        }
    if (width_out)
        *width_out = width;
    return fit;
}

UINT32 GfxUiTextWidthW(const UINT16 *text)
{
    UINT32 width = 0;
    if (!text)
        return 0;
    for (UINT32 i = 0; text[i]; ++i)
        width += GfxUiCharWidth(text[i]);
    return width;
}

UINT32 GfxUiTextWidth(const char *text)
{
    UINT32 width = 0;
    if (!text)
        return 0;
    for (UINT32 i = 0; text[i]; ++i) {
        /* The chart above ASCII is rarely used in kernel strings; a byte that
         * is not a codepoint on its own is measured as the replacement width. */
        UINT32 codepoint = (UINT8)text[i];
        width += GfxUiCharWidth(codepoint);
    }
    return width;
}

UINT32 GfxUiTextFitW(const UINT16 *text, UINT32 max_width)
{
    UINT32 width = 0, count = 0;
    if (!text)
        return 0;
    for (; text[count]; ++count) {
        UINT32 advance = GfxUiCharWidth(text[count]);
        if (width + advance > max_width)
            break;
        width += advance;
    }
    return width;
}

static void ui_draw_glyph(const GFX_FONT_GLYPH *glyph, UINT32 x,
                          UINT32 baseline, UINT32 color)
{
    if (!glyph || !glyph->Width || !glyph->Height)
        return;
    INT32 top = (INT32)baseline - glyph->BearingY;
    INT32 left = (INT32)x + glyph->BearingX;
    const UINT8 *bits = SvUiFontUiFace.Pixels + glyph->Offset;
    UINT32 fr = (color >> 16) & 0xFF, fg = (color >> 8) & 0xFF, fb = color & 0xFF;
    for (UINT32 row = 0; row < glyph->Height; ++row) {
        INT32 py = top + (INT32)row;
        if (py < 0 || (UINT32)py >= GfxFramebuffer.Height)
            continue;
        UINT32 *target = GfxFramebuffer.Base +
                         (UINT64)py * (GfxFramebuffer.Pitch / 4);
        const UINT8 *source = bits + (UINT64)row * glyph->Width;
        for (UINT32 col = 0; col < glyph->Width; ++col) {
            INT32 px = left + (INT32)col;
            UINT32 coverage = source[col];
            if (!coverage || px < 0 || (UINT32)px >= GfxFramebuffer.Width)
                continue;
            UINT32 dst = target[px];
            UINT32 dr = (dst >> GfxFramebuffer.RedShift) & 0xFF;
            UINT32 dg = (dst >> GfxFramebuffer.GreenShift) & 0xFF;
            UINT32 db = (dst >> GfxFramebuffer.BlueShift) & 0xFF;
            UINT32 inverse = 255 - coverage;
            UINT32 r = (fr * coverage + dr * inverse + 127) / 255;
            UINT32 g = (fg * coverage + dg * inverse + 127) / 255;
            UINT32 b = (fb * coverage + db * inverse + 127) / 255;
            target[px] = GfxColor((UINT8)r, (UINT8)g, (UINT8)b);
        }
    }
}

void GfxDrawUiTextW(UINT32 x, UINT32 baseline, const UINT16 *text, UINT32 color)
{
    if (!text || !GfxAvailable())
        return;
    for (UINT32 i = 0; text[i]; ++i) {
        const GFX_FONT_GLYPH *glyph = GfxUiGlyph(text[i]);
        ui_draw_glyph(glyph, x, baseline, color);
        x += glyph ? glyph->Advance : (UINT32)(SvUiFontUiFace.EmPixels / 2);
    }
}

void GfxDrawUiText(UINT32 x, UINT32 baseline, const char *text, UINT32 color)
{
    if (!text || !GfxAvailable())
        return;
    for (UINT32 i = 0; text[i]; ++i) {
        UINT32 codepoint = (UINT8)text[i];
        const GFX_FONT_GLYPH *glyph = GfxUiGlyph(codepoint);
        ui_draw_glyph(glyph, x, baseline, color);
        x += glyph ? glyph->Advance : (UINT32)(SvUiFontUiFace.EmPixels / 2);
    }
}
