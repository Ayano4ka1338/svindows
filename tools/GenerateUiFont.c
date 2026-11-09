/*
 * tools/GenerateUiFont.c - build the kernel's proportional UI font atlas.
 *
 * The window manager needs a Segoe UI-metric-compatible 9pt/12pt face,
 * not the fixed 9x20 console font. This host tool rasterizes Selawik (SIL OFL
 * 1.1, metric-compatible with Segoe UI) with stb_truetype and emits a C table
 * of per-glyph 8-bit coverage bitmaps plus advance/bearing metrics.
 *
 *   tools/GenerateUiFont.c <regular.ttf> <output.c> [--gamma N]
 *
 * The generated file is committed so a normal build never needs a TTF; see
 * the `ui-font` Make target for the regeneration recipe.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

typedef struct {
    int Size;              /* em size in pixels */
    const char *Symbol;    /* C identifier suffix */
} FACE;

static const FACE Faces[] = {
    {12, "Ui"},    /* 9pt at 96 DPI: window frames and application text */
    {16, "Ui16"}, {20, "Ui20"}, {24, "Ui24"}, {32, "Ui32"}, {48, "Ui48"},
};

/* Codepoint set: ASCII, Latin-1, Cyrillic, and the punctuation/symbols the
 * window manager and applications use in captions and text. */
static const unsigned int Ranges[][2] = {
    {0x0020, 0x007E}, {0x00A0, 0x00FF}, {0x0400, 0x045F},
    {0x2010, 0x2015}, {0x2018, 0x201E}, {0x2020, 0x2022},
    {0x2026, 0x2026}, {0x2030, 0x2030}, {0x2039, 0x203A},
    {0x20AC, 0x20AC}, {0x20BD, 0x20BD}, {0x2116, 0x2116},
    {0x2190, 0x2193}, {0x221A, 0x221A}, {0x2264, 0x2265},
    {0x232B, 0x232B},
};

typedef struct {
    unsigned int Codepoint;
    unsigned int Offset;
    unsigned char Width, Height;
    signed char BearingX, BearingY;
    unsigned char Advance;
} GLYPH;

static unsigned char *Pool;
static size_t PoolUsed, PoolCapacity;

static unsigned char *Emit(const unsigned char *data, size_t length)
{
    if (PoolUsed + length > PoolCapacity) {
        PoolCapacity = (PoolUsed + length) * 2 + 4096;
        Pool = realloc(Pool, PoolCapacity);
        if (!Pool) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    size_t offset = PoolUsed;
    memcpy(Pool + offset, data, length);
    PoolUsed += length;
    return Pool + offset;
}

static int Compare(const void *left, const void *right)
{
    unsigned int a = ((const GLYPH *)left)->Codepoint;
    unsigned int b = ((const GLYPH *)right)->Codepoint;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void WriteFace(FILE *out, const stbtt_fontinfo *font,
                      const stbtt_fontinfo *fallback_font, const FACE *face,
                      double gamma)
{
    float scale = stbtt_ScaleForMappingEmToPixels(font, (float)face->Size);
    float fallback_scale = fallback_font ?
        stbtt_ScaleForMappingEmToPixels(fallback_font, (float)face->Size) : 0;
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(font, &ascent, &descent, &lineGap);
    int ascent_px = (int)floor(ascent * scale + 0.5);
    int descent_px = (int)floor(-descent * scale + 0.5);
    int line_px = ascent_px + descent_px;
    if (lineGap > 0)
        line_px = (int)floor((ascent - descent + lineGap) * scale + 0.5);

    GLYPH *glyphs = NULL;
    size_t count = 0, capacity = 0;
    PoolUsed = 0;

    for (size_t range = 0; range < sizeof(Ranges) / sizeof(Ranges[0]); ++range) {
        int present = 0, wanted = 0;
        for (unsigned int cp = Ranges[range][0]; cp <= Ranges[range][1]; ++cp) {
            ++wanted;
            const stbtt_fontinfo *glyph_font = font;
            float glyph_scale = scale;
            int index = stbtt_FindGlyphIndex(font, (int)cp);
            if (!index && fallback_font) {
                index = stbtt_FindGlyphIndex(fallback_font, (int)cp);
                if (index) { glyph_font = fallback_font; glyph_scale = fallback_scale; }
            }
            if (!index) continue;
            ++present;
            int advance, bearing;
            stbtt_GetGlyphHMetrics(glyph_font, index, &advance, &bearing);
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBox(glyph_font, index, glyph_scale, glyph_scale,
                                    &x0, &y0, &x1, &y1);
            int width = x1 - x0, height = y1 - y0;
            unsigned int offset = (unsigned int)PoolUsed;
            if (width > 0 && height > 0) {
                if (width > 255 || height > 255) continue;
                unsigned char *bitmap = malloc((size_t)width * height);
                if (!bitmap) { fprintf(stderr, "out of memory\n"); exit(1); }
                stbtt_MakeGlyphBitmap(glyph_font, bitmap, width, height, width,
                                      glyph_scale, glyph_scale, index);
                if (gamma != 1.0) {
                    for (int i = 0; i < width * height; ++i) {
                        double value = bitmap[i] / 255.0;
                        double shaped = pow(value, gamma) * 255.0 + 0.5;
                        bitmap[i] = (unsigned char)(shaped > 255 ? 255 : shaped);
                    }
                }
                Emit(bitmap, (size_t)width * height);
                free(bitmap);
            }
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 512;
                glyphs = realloc(glyphs, capacity * sizeof(*glyphs));
                if (!glyphs) { fprintf(stderr, "out of memory\n"); exit(1); }
            }
            GLYPH *glyph = &glyphs[count++];
            glyph->Codepoint = cp;
            glyph->Offset = offset;
            glyph->Width = (unsigned char)width;
            glyph->Height = (unsigned char)height;
            glyph->BearingX = (signed char)x0;
            glyph->BearingY = (signed char)-y0;
            int step = (int)floor(advance * glyph_scale + 0.5);
            glyph->Advance = (unsigned char)(step > 255 ? 255 : step);
        }
        if (present != wanted)
            fprintf(stderr, "    U+%04X..U+%04X: %d/%d present\n",
                    Ranges[range][0], Ranges[range][1], present, wanted);
    }
    qsort(glyphs, count, sizeof(*glyphs), Compare);

    fprintf(out, "\nstatic const unsigned char SvUiFont%sPixels[] = {\n",
            face->Symbol);
    for (size_t i = 0; i < PoolUsed; ++i) {
        if (i % 16 == 0) fprintf(out, "    ");
        fprintf(out, "%u,", Pool[i]);
        if (i % 16 == 15 || i + 1 == PoolUsed) fprintf(out, "\n");
        else fprintf(out, " ");
    }
    fprintf(out, "};\n\nstatic const GFX_FONT_GLYPH SvUiFont%sGlyphs[] = {\n",
            face->Symbol);
    for (size_t i = 0; i < count; ++i) {
        const GLYPH *glyph = &glyphs[i];
        fprintf(out, "    {0x%04X,%u,%u,%u,%d,%d,%u},\n", glyph->Codepoint,
                glyph->Offset, glyph->Width, glyph->Height,
                glyph->BearingX, glyph->BearingY, glyph->Advance);
    }
    fprintf(out, "};\n\nconst GFX_FONT_FACE SvUiFont%sFace = {\n"
            "    \"Selawik %dpx\", %d, %d, %d, %d,\n"
            "    SvUiFont%sPixels, SvUiFont%sGlyphs,\n"
            "    (int)(sizeof(SvUiFont%sGlyphs) / sizeof(SvUiFont%sGlyphs[0])),\n"
            "};\n", face->Symbol, face->Size, face->Size, ascent_px, descent_px,
            line_px, face->Symbol, face->Symbol, face->Symbol, face->Symbol);
    fprintf(stderr, "  %s: %zu glyphs, %zu bitmap bytes, ascent %d descent %d line %d\n",
            face->Symbol, count, PoolUsed, ascent_px, descent_px, line_px);
    free(glyphs);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <regular.ttf> <output.c> [fallback.ttf] [--gamma N]\n", argv[0]);
        return 2;
    }
    double gamma = 1.0;
    const char *fallback_path = argc > 3 && strcmp(argv[3], "--gamma") ? argv[3] : NULL;
    int option_start = fallback_path ? 4 : 3;
    for (int i = option_start; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--gamma")) gamma = atof(argv[i + 1]);
    if (gamma < 0.35 || gamma > 3.0) {
        fprintf(stderr, "gamma out of range\n");
        return 2;
    }

    FILE *ttf = fopen(argv[1], "rb");
    if (!ttf) { perror(argv[1]); return 1; }
    fseek(ttf, 0, SEEK_END);
    long length = ftell(ttf);
    fseek(ttf, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, ttf) != (size_t)length) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    fclose(ttf);

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        fprintf(stderr, "%s: not a usable TrueType font\n", argv[1]);
        return 1;
    }

    unsigned char *fallback_data = NULL;
    stbtt_fontinfo fallback_font, *fallback = NULL;
    if (fallback_path) {
        FILE *file = fopen(fallback_path, "rb");
        if (!file) { perror(fallback_path); return 1; }
        fseek(file, 0, SEEK_END);
        long fallback_length = ftell(file);
        fseek(file, 0, SEEK_SET);
        fallback_data = malloc((size_t)fallback_length);
        if (!fallback_data || fread(fallback_data, 1, (size_t)fallback_length, file) != (size_t)fallback_length) {
            fprintf(stderr, "cannot read %s\n", fallback_path);
            return 1;
        }
        fclose(file);
        if (!stbtt_InitFont(&fallback_font, fallback_data,
                            stbtt_GetFontOffsetForIndex(fallback_data, 0))) {
            fprintf(stderr, "%s: not a usable TrueType fallback font\n", fallback_path);
            return 1;
        }
        fallback = &fallback_font;
    }

    FILE *out = fopen(argv[2], "w");
    if (!out) { perror(argv[2]); return 1; }
    fprintf(out,
            "/* Generated by tools/GenerateUiFont.c from Selawik Regular.\n"
            " * Selawik is distributed under the SIL Open Font License 1.1 and is\n"
            " * metric-compatible with Segoe UI; missing glyphs use DejaVu Sans.\n"
            " * Both font licenses are in tools/third_party/.\n"
            " * Do not edit: run `make ui-font` instead.\n"
            " *\n"
            " * win32k uses this atlas for GDI text and window frames. */\n"
            "#include <svindows/UiFont.h>\n");
    for (size_t i = 0; i < sizeof(Faces) / sizeof(Faces[0]); ++i)
        WriteFace(out, &font, fallback, &Faces[i], gamma);
    fclose(out);
    free(data);
    free(fallback_data);
    free(Pool);
    return 0;
}
