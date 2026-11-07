/* Virtual VGA/CGA, PC timer/keyboard I/O and DOS video presentation. */
#include <svindows/Gfx.h>
#include <svindows/Console.h>
#include <svindows/Hal.h>
#include <svindows/Mm.h>
#include "Internal.h"

#define DOS_VIDEO_SCALE_MAX 8192u
static U16 dos_video_xmap[DOS_VIDEO_SCALE_MAX];
static U16 dos_video_ymap[DOS_VIDEO_SCALE_MAX];
static U32 dos_video_map_width, dos_video_map_height;
static U32 dos_video_map_source_width, dos_video_map_source_height;

static int dos_video_prepare_scale_map(U32 source_width, U32 source_height,
                                       U32 width, U32 height,
                                       const U16 **xmap, const U16 **ymap)
{
    if (!width || !height || width > DOS_VIDEO_SCALE_MAX ||
        height > DOS_VIDEO_SCALE_MAX)
        return 0;
    if (dos_video_map_width != width || dos_video_map_height != height ||
        dos_video_map_source_width != source_width ||
        dos_video_map_source_height != source_height) {
        for (U32 x = 0; x < width; ++x)
            dos_video_xmap[x] = (U16)((U64)x * source_width / width);
        for (U32 y = 0; y < height; ++y)
            dos_video_ymap[y] = (U16)((U64)y * source_height / height);
        dos_video_map_width = width;
        dos_video_map_height = height;
        dos_video_map_source_width = source_width;
        dos_video_map_source_height = source_height;
    }
    *xmap = dos_video_xmap;
    *ymap = dos_video_ymap;
    return 1;
}

/* BIOS OEM font tables use the same licensed glyph atlas as the console. */
void DosVideoInitializeFonts(DOS_VM *vm) {
    if (vm->BiosFontsReady) return;
    static const U16 extended[128] = {
      0x00c7,0x00fc,0x00e9,0x00e2,0x00e4,0x00e0,0x00e5,0x00e7,0x00ea,0x00eb,0x00e8,0x00ef,0x00ee,0x00ec,0x00c4,0x00c5,
      0x00c9,0x00e6,0x00c6,0x00f4,0x00f6,0x00f2,0x00fb,0x00f9,0x00ff,0x00d6,0x00dc,0x00a2,0x00a3,0x00a5,0x20a7,0x0192,
      0x00e1,0x00ed,0x00f3,0x00fa,0x00f1,0x00d1,0x00aa,0x00ba,0x00bf,0x2310,0x00ac,0x00bd,0x00bc,0x00a1,0x00ab,0x00bb,
      0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255d,0x255c,0x255b,0x2510,
      0x2514,0x2534,0x252c,0x251c,0x2500,0x253c,0x255e,0x255f,0x255a,0x2554,0x2569,0x2566,0x2560,0x2550,0x256c,0x2567,
      0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256b,0x256a,0x2518,0x250c,0x2588,0x2584,0x258c,0x2590,0x2580,
      0x03b1,0x00df,0x0393,0x03c0,0x03a3,0x03c3,0x00b5,0x03c4,0x03a6,0x0398,0x03a9,0x03b4,0x221e,0x03c6,0x03b5,0x2229,
      0x2261,0x00b1,0x2265,0x2264,0x2320,0x2321,0x00f7,0x2248,0x00b0,0x2219,0x00b7,0x221a,0x207f,0x00b2,0x25a0,0x00a0
    };
    static const unsigned heights[] = {16,14,8};
    for (unsigned table = 0; table < 3; table++) {
      unsigned height = heights[table];
      for (unsigned ch = 0; ch < 256; ch++) {
        U32 cp = ch < 128 ? ch : extended[ch - 128];
        const GFX_FONT_GLYPH *g = GfxUiGlyph(cp);
        if (!g) g = GfxUiGlyph('?');
        for (unsigned row = 0; row < height; row++) {
          U8 bits = 0;
          if (g && ch >= 32) {
            int y = (int)(row * SvUiFontUiFace.LineHeight / height) -
                    (SvUiFontUiFace.Ascent - g->BearingY);
            for (unsigned x = 0; x < 8; x++) {
              int sx = (int)(x * g->Advance / 8u) - g->BearingX;
              if (sx >= 0 && sx < g->Width && y >= 0 && y < g->Height &&
                  SvUiFontUiFace.Pixels[g->Offset + y * g->Width + sx] >= 128)
                bits |= 0x80u >> x;
            }
          }
          vm->Memory[0xc0000u + table * 0x1000u + ch * height + row] = bits;
        }
      }
    }
    vm->Memory[0x1f * 4] = 0; vm->Memory[0x1f * 4 + 1] = 0x24;
    vm->Memory[0x1f * 4 + 2] = 0; vm->Memory[0x1f * 4 + 3] = 0xc0;
    vm->Memory[0x43 * 4] = vm->Memory[0x43 * 4 + 1] = 0;
    vm->Memory[0x43 * 4 + 2] = 0; vm->Memory[0x43 * 4 + 3] = 0xc0;
    vm->BiosFontsReady = TRUE;
}

static void dos_video_fps_sample(DOS_VM *vm)
{
    if (!vm || vm->VideoMode != 0x13u) return;
    U32 now = (U32)KeGetTickCount();
    if (!vm->VideoFpsLastTick) {
        vm->VideoFpsLastTick = now ? now : 1u;
        vm->VideoFpsFrames = 0;
        return;
    }
    vm->VideoFpsFrames++;
    U32 elapsed = now - vm->VideoFpsLastTick;
    if (elapsed >= 100u) {
        KeLog("[dos-fps] mode13=%u frames/s over %u ticks\n",
              (unsigned)(vm->VideoFpsFrames * 100u / elapsed), elapsed);
        vm->VideoFpsLastTick = now;
        vm->VideoFpsFrames = 0;
    }
}

#define VGA_MEMORY_BASE 0x000a0000u
#define VGA_MODE13_SIZE (320u * 200u)

static UINT32 dos_vga_color(DOS_VM *vm, U8 index)
{
    UINT32 at = (UINT32)index * 3u;
    U8 red = vm->VgaDac[at], green = vm->VgaDac[at + 1];
    U8 blue = vm->VgaDac[at + 2];
    /* VGA DAC components are six-bit values. */
    red = (U8)((red << 2) | (red >> 4));
    green = (U8)((green << 2) | (green >> 4));
    blue = (U8)((blue << 2) | (blue >> 4));
    return GfxColor(red, green, blue);
}

static UINT32 dos_vga_attribute_color(DOS_VM *vm, U8 pixel) {
    U8 index = vm->VgaAttribute[pixel & 15u] & 0x3fu;
    U8 select = vm->VgaAttribute[0x14];
    if (vm->VgaAttribute[0x10] & 0x80u) index = (index & 15u) | ((select & 3u) << 4);
    index |= (select & 12u) << 4;
    return dos_vga_color(vm, index & vm->VgaDacMask);
}

typedef struct _DOS_MODE13_RENDER {
    DOS_VM *Vm;
    UINT8 *Source;
    UINT32 Colors[256];
    const U16 *XMap, *YMap;
    UINT32 Width, Height, Stride, Start;
    BOOLEAN Mapped;
} DOS_MODE13_RENDER;

static void dos_video_render_mode13_rows(PVOID opaque, UINT32 first,
                                         UINT32 end)
{
    DOS_MODE13_RENDER *render = opaque;
    DOS_VM *vm = render->Vm;
    for (UINT32 y = first; y < end; ++y) {
        U32 sy = render->Mapped ? render->YMap[y]
                                : (U32)((U64)y * 200u / render->Height);
        U32 *dst = GfxFramebuffer.Base + y * render->Stride;
        /* Nearest-neighbour vertical scaling repeats source rows (commonly
         * four times at the 1200x800 boot mode). Copy completed rows with the
         * SSE2 bulk primitive instead of repeating palette conversion. */
        if (y > first) {
            U32 previous_sy = render->Mapped ? render->YMap[y - 1u]
                : (U32)((U64)(y - 1u) * 200u / render->Height);
            if (previous_sy == sy) {
                DosSimdCopy(dst, dst - render->Stride,
                            (SIZE_T)render->Width * sizeof(U32));
                continue;
            }
        }
        U32 source_row = sy * 320u;
        U32 x = 0;
        while (x < render->Width) {
            U32 sx = render->Mapped ? render->XMap[x]
                : (U32)((U64)x * 320u / render->Width);
            U8 pixel = (vm->VgaSequencer[4] & 8u)
                ? render->Source[source_row + sx]
                : vm->VgaPlanes[sx & 3u][(render->Start + sy * 80u + sx / 4u) & 0xffffu];
            U32 color = render->Colors[pixel];
            /* Horizontal nearest-neighbour expansion repeats source pixels.
             * Resolve the palette once per source pixel, then write the run. */
            do {
                dst[x++] = color;
                if (x >= render->Width) break;
                U32 next_sx = render->Mapped ? render->XMap[x]
                    : (U32)((U64)x * 320u / render->Width);
                if (next_sx != sx) break;
            } while (1);
        }
    }
}

static UINT32 dos_ega_color(U8 value)
{
    U8 blue = (U8)(((value & 0x01u) ? 42u : 0u) +
                   ((value & 0x08u) ? 21u : 0u));
    U8 green = (U8)(((value & 0x02u) ? 42u : 0u) +
                    ((value & 0x10u) ? 21u : 0u));
    U8 red = (U8)(((value & 0x04u) ? 42u : 0u) +
                  ((value & 0x20u) ? 21u : 0u));
    red = (U8)((red * 255u + 31u) / 63u);
    green = (U8)((green * 255u + 31u) / 63u);
    blue = (U8)((blue * 255u + 31u) / 63u);
    return GfxColor(red, green, blue);
}

void DosVideoSetPaletteRegister(DOS_VM *vm, U8 index, U8 value)
{
    if (!vm || (index >= 16u && index != 0x11u)) return;
    vm->VgaAttribute[index] = value & 0x3fu;
    vm->VideoDirty = TRUE;
}

U8 DosVideoGetPaletteRegister(DOS_VM *vm, U8 index)
{
    if (!vm || (index >= 16u && index != 0x11u)) return 0xffu;
    return vm->VgaAttribute[index];
}

static U32 dos_cga_scanline_offset(U32 y)
{
    return ((y & 1u) ? 0x2000u : 0u) + (y >> 1) * 80u;
}

void DosVideoWritePixel(DOS_VM *vm, U16 x, U16 y, U8 color)
{
    if (!vm || y >= 200u) return;
    if (vm->VideoMode == 0x06u && x < 640u) {
        U32 at = 0xb8000u + dos_cga_scanline_offset(y) + (x >> 3);
        U8 mask = (U8)(0x80u >> (x & 7u));
        if (color & 1u) vm->Memory[at] |= mask;
        else vm->Memory[at] &= (U8)~mask;
        vm->VideoDirty = TRUE;
    } else if ((vm->VideoMode == 0x04u || vm->VideoMode == 0x05u) &&
               x < 320u) {
        U32 at = 0xb8000u + dos_cga_scanline_offset(y) + (x >> 2);
        U32 shift = 6u - ((x & 3u) * 2u);
        U8 mask = (U8)(3u << shift);
        vm->Memory[at] = (U8)((vm->Memory[at] & (U8)~mask) |
                              ((color & 3u) << shift));
        vm->VideoDirty = TRUE;
    }
}

U8 DosVideoReadPixel(DOS_VM *vm, U16 x, U16 y)
{
    if (!vm || y >= 200u) return 0;
    if (vm->VideoMode == 0x06u && x < 640u) {
        U32 at = 0xb8000u + dos_cga_scanline_offset(y) + (x >> 3);
        return (U8)((vm->Memory[at] >> (7u - (x & 7u))) & 1u);
    }
    if ((vm->VideoMode == 0x04u || vm->VideoMode == 0x05u) && x < 320u) {
        U32 at = 0xb8000u + dos_cga_scanline_offset(y) + (x >> 2);
        return (U8)((vm->Memory[at] >> (6u - ((x & 3u) * 2u))) & 3u);
    }
    return 0;
}

U8 DosVideoReadMemory(DOS_VM *vm, U32 linear)
{
    if (vm->VideoMode >= 0x0du && vm->VideoMode <= 0x12u) {
        U32 offset = (linear - VGA_MEMORY_BASE) & 0xffffu;
        for (unsigned plane = 0; plane < 4; plane++)
            vm->VgaLatches[plane] = vm->VgaPlanes[plane][offset];
        if (vm->VgaGraphics[5] & 8u) {
            U8 result = 0xffu;
            for (unsigned plane = 0; plane < 4; plane++)
                if (vm->VgaGraphics[7] & (1u << plane))
                    result &= (U8)(vm->VgaLatches[plane] ^
                        ((vm->VgaGraphics[2] & (1u << plane)) ? 0 : 0xffu));
            return result;
        }
        return vm->VgaLatches[vm->VgaGraphics[4] & 3u];
    }
    if (vm->VideoMode == 0x13u && !(vm->VgaSequencer[4] & 8u))
        return vm->VgaPlanes[vm->VgaGraphics[4] & 3u][linear - VGA_MEMORY_BASE];
    return vm->Memory[linear];
}

void DosVideoWriteMemory(DOS_VM *vm, U32 linear, U8 value)
{
    vm->VideoDirty = TRUE;
    if (vm->VideoMode >= 0x0du && vm->VideoMode <= 0x12u) {
        U32 offset = (linear - VGA_MEMORY_BASE) & 0xffffu;
        unsigned rotate = vm->VgaGraphics[3] & 7u;
        U8 rotated = (U8)((value >> rotate) | (value << ((8u - rotate) & 7u)));
        unsigned mode = vm->VgaGraphics[5] & 3u;
        for (unsigned plane = 0; plane < 4; plane++) {
            if (!(vm->VgaSequencer[2] & (1u << plane))) continue;
            U8 latch = vm->VgaLatches[plane], mask = vm->VgaGraphics[8];
            U8 data = rotated;
            if (mode == 1u) {
                vm->VgaPlanes[plane][offset] = latch;
                continue;
            }
            if (mode == 2u) data = value & (1u << plane) ? 0xffu : 0;
            else if (mode == 3u || (vm->VgaGraphics[1] & (1u << plane)))
                data = vm->VgaGraphics[0] & (1u << plane) ? 0xffu : 0;
            if (mode == 3u) mask &= rotated;
            switch ((vm->VgaGraphics[3] >> 3) & 3u) {
            case 1: data &= latch; break;
            case 2: data |= latch; break;
            case 3: data ^= latch; break;
            }
            vm->VgaPlanes[plane][offset] = (U8)((data & mask) | (latch & ~mask));
        }
        return;
    }
    if (vm->VideoMode == 0x13u && !(vm->VgaSequencer[4] & 8u)) {
        for (unsigned plane = 0; plane < 4; plane++)
            if (vm->VgaSequencer[2] & (1u << plane))
                vm->VgaPlanes[plane][linear - VGA_MEMORY_BASE] = value;
    } else vm->Memory[linear] = value;
}

void DosVideoSetMode(DOS_VM *vm, U8 mode)
{
    if (!vm) return;
    vm->VideoDirty = TRUE;
    vm->VideoMode = mode & 0x7fu;
    /* BIOS clients also inspect the data area directly. In particular the
     * Windows keyboard hook delegates to BIOS in text modes (BDA 0040:49). */
    vm->Memory[0x449u] = vm->VideoMode;
    vm->Memory[0x44au] = vm->VideoMode == 4u || vm->VideoMode == 5u ? 40u : 80u;
    vm->Memory[0x44bu] = 0;
    vm->Memory[0x462u] = 0; /* active display page */
    vm->Memory[0x463u] = 0xd4u; vm->Memory[0x464u] = 3;
    vm->Memory[0x484u] = 24; /* text rows minus one */
    KeLog("[dos-vga] video mode=%02x\n", vm->VideoMode);

    if (vm->VideoMode == 0x13u) {
        vm->VgaSequencer[2] = 0x0fu;
        vm->VgaSequencer[4] = 0x0eu;
        vm->VgaCrtc[0x0c] = vm->VgaCrtc[0x0d] = 0;
        memset(vm->VgaPlanes, 0, sizeof(vm->VgaPlanes));
        vm->VgaDacMask = 0xffu;
        vm->VgaDacWriteIndex = 0;
        vm->VgaDacReadIndex = 0;
        vm->VgaDacComponent = 0;
        static const U8 ega_dac[16][3] = {
            {0,0,0},{0,0,42},{0,42,0},{0,42,42},
            {42,0,0},{42,0,42},{42,21,0},{42,42,42},
            {21,21,21},{21,21,63},{21,63,21},{21,63,63},
            {63,21,21},{63,21,63},{63,63,21},{63,63,63}
        };
        for (unsigned i = 0; i < 16; i++)
            memcpy(&vm->VgaDac[i * 3u], ega_dac[i], 3);
        for (unsigned i = 16; i < 256; i++) {
            U8 gray = (U8)((i - 16u) * 63u / 239u);
            vm->VgaDac[i * 3u] = gray;
            vm->VgaDac[i * 3u + 1] = gray;
            vm->VgaDac[i * 3u + 2] = gray;
        }
    } else if ((vm->VideoMode >= 4u && vm->VideoMode <= 6u) ||
               (vm->VideoMode >= 0x0du && vm->VideoMode <= 0x12u)) {
        static const U8 ega_palette[16] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
            0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f
        };
        memcpy(vm->VgaAttribute, ega_palette, sizeof(ega_palette));
        if (vm->VideoMode == 0x06u)
            vm->VgaAttribute[1] = 0x3fu; /* monochrome foreground is white */
        vm->VgaAttribute[0x11] = 0;
        vm->VgaAttribute[0x10] &= (U8)~0x08u;
        vm->VgaAttributeIndex = 0;
        vm->VgaAttributeDataPhase = FALSE;
        if (vm->VideoMode >= 0x0du) {
            vm->VgaAttribute[0x10] = 1; vm->VgaAttribute[0x14] = 0;
            vm->VgaDacMask = 0xff;
            for (U32 i = 0; i < 64; i++) {
                vm->VgaDac[i * 3] = ((i & 4u) ? 42u : 0u) + ((i & 32u) ? 21u : 0u);
                vm->VgaDac[i * 3 + 1] = ((i & 2u) ? 42u : 0u) + ((i & 16u) ? 21u : 0u);
                vm->VgaDac[i * 3 + 2] = ((i & 1u) ? 42u : 0u) + ((i & 8u) ? 21u : 0u);
            }
            memset(vm->VgaPlanes, 0, sizeof(vm->VgaPlanes));
            memset(vm->VgaLatches, 0, sizeof(vm->VgaLatches));
            memset(vm->VgaGraphics, 0, sizeof(vm->VgaGraphics));
            vm->VgaSequencer[2] = 0x0fu;
            vm->VgaSequencer[4] = 0x06u;
            vm->VgaGraphics[6] = 0x05u;
            vm->VgaGraphics[7] = 0x0fu;
            vm->VgaGraphics[8] = 0xffu;
            vm->VgaCrtc[0x0c] = vm->VgaCrtc[0x0d] = 0;
        } else memset(vm->Memory + 0xb8000u, 0, 0x4000u);
    }

    BOOLEAN needs_framebuffer = (vm->VideoMode == 0x13u ||
                                  (vm->VideoMode >= 0x0du && vm->VideoMode <= 0x12u) ||
                                  (vm->VideoMode >= 4u && vm->VideoMode <= 6u));
    if (needs_framebuffer && !vm->VideoActive && GfxAvailable()) {
        if (!vm->XSession) HalConsoleAcquireFramebuffer();
        vm->VideoActive = TRUE;
    } else if (!needs_framebuffer && vm->VideoActive) {
        vm->VideoActive = FALSE;
        if (!vm->XSession && HalConsoleReleaseFramebuffer()) ConRedraw();
    }
    if (!vm->XSession && GfxAvailable() && needs_framebuffer) GfxClear(GfxColor(0, 0, 0));
}

void DosVideoInherit(DOS_VM *destination, const DOS_VM *source)
{
    DosXInherit(destination, source);
    DosVideoSetMode(destination, source->VideoMode);
    memcpy(destination->Memory + 0xa0000u, source->Memory + 0xa0000u, 0x20000u);
    memcpy(destination->VgaPlanes, source->VgaPlanes, sizeof(source->VgaPlanes));
    memcpy(destination->VgaLatches, source->VgaLatches, sizeof(source->VgaLatches));
    memcpy(destination->VgaDac, source->VgaDac, sizeof(source->VgaDac));
    memcpy(destination->VgaAttribute, source->VgaAttribute, sizeof(source->VgaAttribute));
    memcpy(destination->VgaSequencer, source->VgaSequencer, sizeof(source->VgaSequencer));
    memcpy(destination->VgaGraphics, source->VgaGraphics, sizeof(source->VgaGraphics));
    memcpy(destination->VgaCrtc, source->VgaCrtc, sizeof(source->VgaCrtc));
    destination->VgaMiscOutput = source->VgaMiscOutput;
    destination->VgaDacMask = source->VgaDacMask;
    destination->VideoDirty = TRUE;
}

/* The standard 16x16 Microsoft-compatible DOS arrow.  The two masks follow
 * the DOS mouse-driver convention used by DOSBox: AND selects the old pixel,
 * then XOR modifies it.  This gives the familiar black outline and white fill
 * without changing the guest's video memory. */
static const U16 dos_mouse_default_and[16] = {
    0x3fff, 0x1fff, 0x0fff, 0x07ff,
    0x03ff, 0x01ff, 0x00ff, 0x007f,
    0x003f, 0x001f, 0x01ff, 0x00ff,
    0x30ff, 0xf87f, 0xf87f, 0xfcff
};

static const U16 dos_mouse_default_xor[16] = {
    0x0000, 0x4000, 0x6000, 0x7000,
    0x7800, 0x7c00, 0x7e00, 0x7f00,
    0x7f80, 0x7c00, 0x6c00, 0x4600,
    0x0600, 0x0300, 0x0300, 0x0000
};

void DosMouseSetDefaultCursor(DOS_VM *vm)
{
    if (!vm) return;
    memset(vm->MouseAndMask, 0, sizeof(vm->MouseAndMask));
    memset(vm->MouseXorMask, 0, sizeof(vm->MouseXorMask));
    for (U32 row = 0; row < 16; row++) {
        vm->MouseAndMask[row][0] = dos_mouse_default_and[row];
        vm->MouseXorMask[row][0] = dos_mouse_default_xor[row];
    }
    vm->MouseHotX = 0;
    vm->MouseHotY = 0;
    vm->MouseCursorWidthWords = 1;
    vm->MouseCursorHeight = 16;
}

static void dos_video_draw_mouse_cursor(DOS_VM *vm, U32 left, U32 top,
                                       U32 scale, U32 source_width,
                                       U32 source_height, U32 coordinate_width)
{
    if (!vm || !scale || (!vm->MouseInitialized && !vm->SerialMouseActive) ||
        (!vm->SerialMouseActive && vm->MouseDisabled) ||
        !GfxFramebuffer.Base)
        return;
    /* Windows 3.1 draws its own cursor through MOUSE.DRV/GDI.  Compositing
     * the DOS mouse driver's cursor as well leaves a second pointer behind. */
    if (vm->DosVersion == 0x1e03u) return;

    /* Windows 1.x uses its own serial driver rather than INT 33h.  Its guest
     * coordinates are updated asynchronously from COM1 packets, so draw the
     * host pointer at the host's absolute framebuffer position instead of
     * waiting for the guest driver to catch up. */
    if (!vm->SerialMouseActive && vm->MouseHideCount)
        return;
    if (!vm->SerialMouseActive && vm->MouseConditionalOff &&
        vm->MouseX >= vm->MouseConditionalLeft &&
        vm->MouseX <= vm->MouseConditionalRight &&
        vm->MouseY >= vm->MouseConditionalTop &&
        vm->MouseY <= vm->MouseConditionalBottom)
        return;

    U32 width_words = vm->MouseCursorWidthWords;
    U32 cursor_height = vm->MouseCursorHeight;
    BOOLEAN use_default = !width_words || !cursor_height;
    if (use_default) {
        width_words = 1;
        cursor_height = 16;
    }
    if (width_words > 8u) width_words = 8u;
    if (cursor_height > 64u) cursor_height = 64u;

    INT32 cursor_x, cursor_y;
    INT32 clip_left, clip_top, clip_right, clip_bottom;
    if (vm->SerialMouseActive) {
        INT32 host_x = MouseState.X;
        INT32 host_y = MouseState.Y;
        if (host_x < 0) host_x = 0;
        if (host_y < 0) host_y = 0;
        if (host_x >= (INT32)GfxFramebuffer.Width)
            host_x = (INT32)GfxFramebuffer.Width - 1;
        if (host_y >= (INT32)GfxFramebuffer.Height)
            host_y = (INT32)GfxFramebuffer.Height - 1;
        cursor_x = host_x - vm->MouseHotX;
        cursor_y = host_y - vm->MouseHotY;
        clip_left = 0;
        clip_top = 0;
        clip_right = (INT32)GfxFramebuffer.Width;
        clip_bottom = (INT32)GfxFramebuffer.Height;
        /* The serial path is a native desktop pointer: do not scale a 16px
         * Windows arrow with a low-resolution guest framebuffer. */
        scale = 1;
    } else {
        U32 guest_x = (U32)vm->MouseX * source_width /
                      (coordinate_width ? coordinate_width : source_width);
        U32 guest_y = vm->MouseY;
        if (guest_x >= source_width) guest_x = source_width - 1u;
        if (guest_y >= source_height) guest_y = source_height - 1u;

        cursor_x = (INT32)left + (INT32)(guest_x * scale) -
                   (INT32)vm->MouseHotX * (INT32)scale;
        cursor_y = (INT32)top + (INT32)(guest_y * scale) -
                   (INT32)vm->MouseHotY * (INT32)scale;
        clip_left = (INT32)left;
        clip_top = (INT32)top;
        clip_right = clip_left + (INT32)(source_width * scale);
        clip_bottom = clip_top + (INT32)(source_height * scale);
    }
    UINT32 stride = GfxFramebuffer.Pitch / sizeof(UINT32);
    UINT32 black = GfxColor(0, 0, 0);
    UINT32 white = GfxColor(255, 255, 255);

    for (U32 row = 0; row < cursor_height; row++) {
        for (U32 col = 0; col < width_words * 16u; col++) {
            U32 word = col >> 4;
            U16 bit = (U16)(0x8000u >> (col & 15u));
            U16 and_bits = use_default ?
                (word == 0u && row < 16u ? dos_mouse_default_and[row] : 0xffffu) :
                vm->MouseAndMask[row][word];
            U16 xor_bits = use_default ?
                (word == 0u && row < 16u ? dos_mouse_default_xor[row] : 0u) :
                vm->MouseXorMask[row][word];
            BOOLEAN and_pixel = (and_bits & bit) != 0;
            BOOLEAN xor_pixel = (xor_bits & bit) != 0;
            if (and_pixel && !xor_pixel)
                continue; /* transparent: preserve the framebuffer pixel */

            INT32 x = cursor_x + (INT32)(col * scale);
            INT32 y = cursor_y + (INT32)(row * scale);
            for (U32 dy = 0; dy < scale; dy++) {
                INT32 py = y + (INT32)dy;
                if (py < clip_top || py >= clip_bottom ||
                    py < 0 || py >= (INT32)GfxFramebuffer.Height)
                    continue;
                UINT32 *dst = GfxFramebuffer.Base + (UINT32)py * stride;
                for (U32 dx = 0; dx < scale; dx++) {
                    INT32 px = x + (INT32)dx;
                    if (px < clip_left || px >= clip_right || px < 0 ||
                        px >= (INT32)GfxFramebuffer.Width)
                        continue;
                    if (and_pixel) dst[px] ^= white;
                    else dst[px] = xor_pixel ? white : black;
                }
            }
        }
    }
}

__attribute__((optimize("O3"))) void DosVideoPresent(DOS_VM *vm)
{
    if (!vm || !vm->VideoActive || !vm->VideoDirty ||
        !GfxAvailable() || !vm->Memory)
        return;

    if (DosXPresent(vm)) return;

    UINT32 width = GfxFramebuffer.Width;
    UINT32 height = GfxFramebuffer.Height;
    UINT32 stride = GfxFramebuffer.Pitch / sizeof(UINT32);
    if (!width || !height || !stride || !GfxFramebuffer.Base) return;

    if (vm->VideoMode == 0x06u || vm->VideoMode == 0x04u ||
        vm->VideoMode == 0x05u) {
        UINT32 source_width = vm->VideoMode == 0x06u ? 640u : 320u;
        /* VGA modes describe a logical display, not a window. Stretch it to
         * the active framebuffer so legacy graphics programs use the whole
         * Svindows display at both low and high host resolutions. */
        UINT32 left = 0, top = 0;
        UINT32 palette[4];
        for (unsigned i = 0; i < 4; i++)
            palette[i] = dos_ega_color(vm->VgaAttribute[i]);
        const U16 *xmap = NULL, *ymap = NULL;
        int mapped = dos_video_prepare_scale_map(source_width, 200u, width,
                                                 height, &xmap, &ymap);
        const U8 *cga = vm->Memory + 0xb8000u;
        for (UINT32 y = 0; y < height; y++) {
            UINT32 sy = mapped ? ymap[y] : (UINT32)((U64)y * 200u / height);
            UINT32 row = dos_cga_scanline_offset(sy);
            UINT32 *dst = GfxFramebuffer.Base + y * stride;
            for (UINT32 x = 0; x < width; x++) {
                UINT32 sx = mapped ? xmap[x] : (UINT32)((U64)x * source_width / width);
                U8 bits = cga[row + (vm->VideoMode == 0x06u ? (sx >> 3) : (sx >> 2))];
                U32 pixel = vm->VideoMode == 0x06u
                    ? ((bits >> (7u - (sx & 7u))) & 1u)
                    : ((bits >> (6u - ((sx & 3u) * 2u))) & 3u);
                dst[x] = palette[pixel];
            }
        }
        dos_video_draw_mouse_cursor(vm, left, top, 1u, source_width,
                                    200u, 640u);
        vm->VideoDirty = FALSE;
        return;
    }

    if (vm->VideoMode >= 0x0du && vm->VideoMode <= 0x12u) {
        U32 sw = vm->VideoMode == 0x0du ? 320u : 640u;
        U32 sh = vm->VideoMode <= 0x0eu ? 200u : vm->VideoMode <= 0x10u ? 350u : 480u;
        U32 left = 0, top = 0;
        U32 start = ((U32)vm->VgaCrtc[0x0c] << 8) | vm->VgaCrtc[0x0d];
        U32 colors[16];
        for (unsigned i = 0; i < 16; i++) colors[i] = dos_vga_attribute_color(vm, (U8)i);
        const U16 *xmap = NULL, *ymap = NULL;
        int mapped = dos_video_prepare_scale_map(sw, sh, width, height,
                                                 &xmap, &ymap);
        for (U32 y = 0; y < height; y++) {
            U32 source_y = mapped ? ymap[y] : (U32)((U64)y * sh / height);
            for (U32 x = 0; x < width; x++) {
                U32 source_x = mapped ? xmap[x] : (U32)((U64)x * sw / width);
                U32 offset = (start + source_y * (sw / 8u) + source_x / 8u) & 0xffffu;
                U8 pixel = 0;
                for (unsigned plane = 0; plane < 4; plane++)
                    pixel |= ((vm->VgaPlanes[plane][offset] >> (7u - (source_x & 7u))) & 1u) << plane;
                GfxFramebuffer.Base[y * stride + x] = colors[pixel];
            }
        }
        dos_video_draw_mouse_cursor(vm, left, top, 1u, sw, sh,
                                    vm->VideoMode == 0x0du ? 320u : 640u);
        vm->VideoDirty = FALSE;
        return;
    }
    if (vm->VideoMode != 0x13u) return;
    UINT32 left = 0, top = 0;
    UINT8 *source = vm->Memory + VGA_MEMORY_BASE;

    const U16 *xmap = NULL, *ymap = NULL;
    int mapped = dos_video_prepare_scale_map(320u, 200u, width, height,
                                             &xmap, &ymap);
    DOS_MODE13_RENDER render = {0};
    render.Vm = vm;
    render.Source = source;
    render.XMap = xmap;
    render.YMap = ymap;
    render.Width = width;
    render.Height = height;
    render.Stride = stride;
    render.Start = ((U32)vm->VgaCrtc[0x0c] << 8) | vm->VgaCrtc[0x0d];
    render.Mapped = mapped;
    for (unsigned i = 0; i < 256; ++i)
        render.Colors[i] = dos_vga_color(vm, (U8)i);
    dos_video_render_mode13_rows(&render, 0, height);
    dos_video_draw_mouse_cursor(vm, left, top, 1u, 320u, 200u, 640u);
    dos_video_fps_sample(vm);
    vm->VideoDirty = FALSE;
}

void DosVideoStop(DOS_VM *vm)
{
    if (!vm) return;
    if (vm->VideoActive) {
        vm->VideoActive = FALSE;
        if (!vm->XSession && HalConsoleReleaseFramebuffer()) ConRedraw();
    }
    if (vm->KeyboardMirrorActive) {
        vm->KeyboardMirrorActive = FALSE;
        KbdInputMirrorRelease();
    }
}

static U8 dos_keyboard_read(DOS_VM *vm)
{
    if (vm->KeyboardDataReady) {
        vm->KeyboardDataReady = FALSE;
        return vm->KeyboardData;
    }
    UINT32 event;
    while (DosReadKey(vm, &event)) {
        /* The native driver also mirrors translated character events. The
         * virtual 8042 consumes only raw make/break scan-code events. */
        if (event & KBD_INPUT_CHARACTER) continue;
        vm->KeyboardData = (U8)(event & 0x7fu);
        if (event & 0x0100u) vm->KeyboardData |= 0x80u;
        /* Extended keys share the set-1 low scan code. This is sufficient for
         * DOS games' arrow/keypad bindings and avoids a second-byte IRQ race. */
        vm->KeyboardDataReady = FALSE;
        return vm->KeyboardData;
    }
    /* The 8042 data latch retains its last byte when its output buffer is empty. */
    return vm->KeyboardData;
}

static U16 dos_pit_counter(DOS_VM *vm)
{
    U32 reload = vm->PitReload ? vm->PitReload : 65536u;
    U32 elapsed = (U32)KeGetTickCount() * 11932u;
    U32 phase = elapsed % reload;
    U32 counter = reload - phase;
    if (counter >= 65536u) counter = 0;
    return (U16)counter;
}

static U8 dos_pit_read(DOS_VM *vm)
{
    U16 counter;
    U8 access = (vm->PitControl >> 4) & 3u;
    if (vm->PitLatched) counter = vm->PitLatch;
    else counter = dos_pit_counter(vm);
    U8 value;
    if (access == 1u) {
        value = (U8)counter;
    } else if (access == 2u) {
        value = (U8)(counter >> 8);
    } else {
        value = vm->PitReadPhase ? (U8)(counter >> 8) : (U8)counter;
        vm->PitReadPhase ^= 1u;
        if (vm->PitLatched && !vm->PitReadPhase) vm->PitLatched = FALSE;
    }
    return value;
}

static BOOLEAN dos_uart_rx_ready(DOS_VM *vm, unsigned uart)
{
    return vm->UartRxHead[uart] != vm->UartRxTail[uart];
}

static void dos_uart_rx_push(DOS_VM *vm, unsigned uart, U8 byte)
{
    U8 next = (U8)((vm->UartRxHead[uart] + 1u) % sizeof(vm->UartRx[uart]));
    if (next == vm->UartRxTail[uart]) return;
    vm->UartRx[uart][vm->UartRxHead[uart]] = byte;
    vm->UartRxHead[uart] = next;
}

/* Windows 1.x uses its own Microsoft Bus/Serial Mouse driver and does not
 * consume movement through the DOS INT 33h API. COM1 therefore exposes the
 * original three-byte, 7-bit Microsoft serial mouse protocol. */
void DosSerialMousePoll(DOS_VM *vm)
{
    if (!vm || !vm->SerialMouseActive) return;
    unsigned uart = vm->SerialMouseUart;
    U32 sequence = DosHostMouse(vm)->Seq;
    if (sequence == vm->SerialMouseHostSeq) return;

    INT32 dx = DosHostMouse(vm)->X - vm->SerialMouseHostX;
    INT32 dy = DosHostMouse(vm)->Y - vm->SerialMouseHostY;
    U8 buttons = DosHostMouse(vm)->Buttons;
    vm->SerialMouseHostSeq = sequence;
    vm->SerialMouseHostX = DosHostMouse(vm)->X;
    vm->SerialMouseHostY = DosHostMouse(vm)->Y;
    /* Cursor composition happens after the guest frame is redrawn.  A host
     * pointer move must therefore request a fresh presentation even when the
     * guest has not touched video memory. */
    vm->VideoDirty = TRUE;

    while (dx || dy || buttons != vm->SerialMouseButtons) {
        INT32 packet_x = dx < -128 ? -128 : dx > 127 ? 127 : dx;
        INT32 packet_y = dy < -128 ? -128 : dy > 127 ? 127 : dy;
        U8 x = (U8)(INT8)packet_x;
        U8 y = (U8)(INT8)packet_y;
        U8 first = 0x40u | ((buttons & MOUSE_BUTTON_LEFT) ? 0x20u : 0u) |
                     ((buttons & MOUSE_BUTTON_RIGHT) ? 0x10u : 0u) |
                     ((y >> 4) & 0x0cu) | ((x >> 6) & 0x03u);
        dos_uart_rx_push(vm, uart, first);
        dos_uart_rx_push(vm, uart, x & 0x3fu);
        dos_uart_rx_push(vm, uart, y & 0x3fu);
        dx -= packet_x;
        dy -= packet_y;
        vm->SerialMouseButtons = buttons;
    }
}

BOOLEAN DosSerialMouseIrqPending(DOS_VM *vm, unsigned uart_index)
{
    if (!vm || !vm->SerialMouseActive || vm->SerialMouseUart != uart_index)
        return FALSE;
    U8 *uart = vm->UartRegisters[uart_index];
    return dos_uart_rx_ready(vm, uart_index) && (uart[1] & 0x01u) &&
           (uart[4] & 0x08u);
}

static int dos_sb_hw_write(U8 value)
{
    for (unsigned tries = 0; tries < 100000u; ++tries)
        if (!(__inbyte(0x022cu) & 0x80u)) {
            __outbyte(0x022cu, value);
            return 1;
        }
    return 0;
}

static void dos_sb_hw_start(DOS_VM *vm, U8 command, const U8 *args,
                            unsigned arg_count)
{
    if (!vm->SbHardwarePresent || !vm->SbDmaBuffer || !arg_count) return;
    unsigned count_arg = command == 0xc0u || command == 0xc6u ? 1u : 0u;
    if (!((command == 0x14u || command == 0x1cu ||
           command == 0xc0u || command == 0xc6u) &&
          arg_count == (count_arg ? 3u : 2u))) return;

    U32 length = (U32)args[count_arg] |
                 ((U32)args[count_arg + 1u] << 8);
    length++;
    U32 source = ((U32)vm->DmaPageRegisters[3] << 16) |
                 vm->DmaRegisters[2];
    if (!length || length > 0x10000u || source >= MEM_SIZE ||
        length > MEM_SIZE - source ||
        (vm->DmaMask & 0x02u) ||
        vm->SbDmaAllocationPhysical == MM_INVALID_PHYS) return;

    memcpy(vm->SbDmaBuffer, vm->Memory + source, length);
    UINT64 buffer_phys = (vm->SbDmaAllocationPhysical + 0xffffu) & ~0xffffULL;
    U16 dma_address = (U16)buffer_phys;
    /* 8237 channel 1: single-cycle memory-to-device, 8-bit samples. */
    __outbyte(0x000a, 0x05u); /* mask channel 1 */
    __outbyte(0x000c, 0);
    __outbyte(0x000b, 0x49u);
    __outbyte(0x0002, (U8)dma_address);
    __outbyte(0x0002, (U8)(dma_address >> 8));
    __outbyte(0x0003, (U8)(length - 1u));
    __outbyte(0x0003, (U8)((length - 1u) >> 8));
    __outbyte(0x0083, (U8)(buffer_phys >> 16));
    __outbyte(0x000a, 0x01u); /* unmask channel 1 */

    (void)dos_sb_hw_write(command);
    for (unsigned i = 0; i < arg_count; ++i)
        if (!dos_sb_hw_write(args[i])) break;
}

static void dos_sb_hw_command(DOS_VM *vm, U8 command, const U8 *args,
                              unsigned arg_count)
{
    if (!vm->SbHardwarePresent) return;
    if (command == 0xd1u || command == 0xd3u || command == 0xd8u ||
        command == 0xd0u || command == 0xd4u) {
        (void)dos_sb_hw_write(command);
        return;
    }
    if (command == 0x40u || command == 0x41u || command == 0xe0u ||
        command == 0x10u) {
        if (dos_sb_hw_write(command))
            for (unsigned i = 0; i < arg_count; ++i)
                if (!dos_sb_hw_write(args[i])) break;
        return;
    }
    dos_sb_hw_start(vm, command, args, arg_count);
}

U32 DosPortIn(DOS_VM *vm, U16 port, unsigned width)
{
    if (width > 8) {
        U32 combined = 0;
        for (unsigned i = 0; i < width / 8; i++) combined |= DosPortIn(vm, (U16)(port + i), 8) << (i * 8);
        return combined;
    }
    U32 value;
    U8 byte = 0xffu;
    /* Sound Blaster 2.0 DSP at the conventional 220h base. */
    if (port == 0x022au) {
        if (vm->SbDspResponseCount) {
            byte = vm->SbDspResponse[vm->SbDspResponseHead++ & 7u];
            vm->SbDspResponseCount--;
        } else byte = 0xffu;
        return byte;
    }
    if (port == 0x022cu) return 0;
    if (port == 0x022eu) return vm->SbDspResponseCount ? 0x80u : 0;
    if (port == 0x022fu) return vm->SbDspResponseCount ? 0x80u : 0;
    if ((port >= 0x02f8u && port <= 0x02ffu) ||
        (port >= 0x03f8u && port <= 0x03ffu)) {
        unsigned uart_index = port >= 0x03f8u;
        U8 *uart = vm->UartRegisters[uart_index];
        unsigned reg = port & 7u;
        DosSerialMousePoll(vm);
        if (reg == 5u) return 0x60u | (dos_uart_rx_ready(vm, uart_index) ? 1u : 0u);
        if (reg == 2u)
            return dos_uart_rx_ready(vm, uart_index) && (uart[1] & 1u) ? 0x04u : 0x01u;
        if (reg == 6u) return uart_index == 1 ? 0xb0u : 0;
        if (reg == 0u && !(uart[3] & 0x80u)) {
            if (!dos_uart_rx_ready(vm, uart_index)) return 0;
            U8 result = vm->UartRx[uart_index][vm->UartRxTail[uart_index]];
            vm->UartRxTail[uart_index] = (U8)((vm->UartRxTail[uart_index] + 1u) %
                                               sizeof(vm->UartRx[uart_index]));
            return result;
        }
        return uart[reg];
    }
    switch (port) {
    case 0x0000: case 0x0001: case 0x0002: case 0x0003:
    case 0x0004: case 0x0005: case 0x0006: case 0x0007: {
        U16 *reg = &vm->DmaRegisters[port];
        byte = vm->DmaFlipFlop ? (U8)(*reg >> 8) : (U8)*reg;
        vm->DmaFlipFlop ^= 1u;
        break;
    }
    case 0x0008:
        byte = vm->DmaStatus;
        vm->DmaStatus &= 0xf0u; /* reading status clears terminal-count bits */
        break;
    case 0x000f: byte = vm->DmaMask; break;
    case 0x0080: case 0x0081: case 0x0082: case 0x0083:
    case 0x0084: case 0x0085: case 0x0086: case 0x0087:
    case 0x0088: case 0x0089: case 0x008a: case 0x008b:
    case 0x008c: case 0x008d: case 0x008e: case 0x008f:
        byte = vm->DmaPageRegisters[port - 0x80u];
        break;
    case 0x0060: byte = dos_keyboard_read(vm); break;
    case 0x0064: byte = (vm->KeyboardDataReady || DosKeyAvailable(vm)) ? 0x01u : 0x00u; break;
    case 0x0061: byte = vm->VgaSpeakerControl; break;
    case 0x0040: byte = dos_pit_read(vm); break;
    case 0x0041: case 0x0042: byte = 0; break;
    case 0x0020: case 0x00a0: byte = 0; break;
    case 0x0021: byte = vm->PicMasterMask; break;
    case 0x00a1: byte = vm->PicSlaveMask; break;
    case 0x03c6: byte = vm->VgaDacMask ? vm->VgaDacMask : 0xffu; break;
    case 0x03c7: byte = 0x03u; break;
    case 0x03c8: byte = vm->VgaDacWriteIndex; break;
    case 0x03c9:
        byte = vm->VgaDac[(UINT32)vm->VgaDacReadIndex * 3u +
                          vm->VgaDacComponent];
        if (++vm->VgaDacComponent == 3) {
            vm->VgaDacComponent = 0;
            vm->VgaDacReadIndex++;
        }
        break;
    case 0x03cc: byte = vm->VgaMiscOutput; break;
    case 0x03c1:
        byte = vm->VgaAttribute[vm->VgaAttributeIndex % sizeof(vm->VgaAttribute)];
        break;
    case 0x03c5:
        byte = vm->VgaSequencer[vm->VgaSequencerIndex % sizeof(vm->VgaSequencer)];
        break;
    case 0x03cf:
        byte = vm->VgaGraphics[vm->VgaGraphicsIndex % sizeof(vm->VgaGraphics)];
        break;
    case 0x03d5:
        byte = vm->VgaCrtc[vm->VgaCrtcIndex % sizeof(vm->VgaCrtc)];
        break;
    case 0x03da: case 0x03ba:
        vm->VgaAttributeDataPhase = FALSE;
        /* A changing retrace bit allows the common VGA wait loops to make
         * forward progress; Svindows presents at the same 60-Hz-like cadence. */
        byte = (KeGetTickCount() & 1u) ? 0x08u : 0x00u;
        break;
    default:
        byte = 0xffu;
        break;
    }
    value = byte;
    if (width == 16) value |= 0xff00u;
    else if (width == 32) value |= 0xffffff00u;
    if (vm->PortTraceCount < 64u) {
        vm->PortTraceCount++;
        KeLog("[dos-vga] IN port=0x%04x width=%u -> 0x%08x\n",
              port, width, value);
    }
    return value;
}

void DosPortOut(DOS_VM *vm, U16 port, U32 value, unsigned width)
{
    if (width > 8) {
        for (unsigned i = 0; i < width / 8; i++) DosPortOut(vm, (U16)(port + i), (value >> (i * 8)) & 0xffu, 8);
        return;
    }
    U8 byte = (U8)value;
    if (port == 0x0226u) {
        if (byte & 1u) {
            vm->SbDspReset = TRUE;
            vm->SbDspResponseHead = vm->SbDspResponseCount = 0;
        } else if (vm->SbDspReset) {
            vm->SbDspReset = FALSE;
            vm->SbDspResponse[0] = 0xaau;
            vm->SbDspResponseHead = 0;
            vm->SbDspResponseCount = 1;
            /* The emulated DSP remains available without a physical card.
             * If a QEMU SB16 is present, use it as the host PCM sink. */
            vm->SbHardwarePresent = FALSE;
            __outbyte(0x0226u, 1u);
            __iodelay();
            __outbyte(0x0226u, 0u);
            for (unsigned tries = 0; tries < 100000u; ++tries) {
                if (__inbyte(0x022eu) & 0x80u) {
                    if (__inbyte(0x022au) == 0xaau)
                        vm->SbHardwarePresent = TRUE;
                    break;
                }
            }
        }
        return;
    }
    if (port == 0x022cu) {
        if (vm->SbDspArgsNeeded) {
            if (vm->SbDspArgCount < sizeof(vm->SbDspArgs))
                vm->SbDspArgs[vm->SbDspArgCount] = byte;
            vm->SbDspArgCount++;
            if (vm->SbDspArgCount >= vm->SbDspArgsNeeded) {
                dos_sb_hw_command(vm, vm->SbDspCommand, vm->SbDspArgs,
                                  vm->SbDspArgsNeeded);
                vm->SbDspArgsNeeded = vm->SbDspArgCount = 0;
            }
            return;
        }
        vm->SbDspCommand = byte;
        vm->SbDspArgCount = 0;
        switch (byte) {
        case 0xe1: /* Get DSP version. */
            vm->SbDspResponse[0] = vm->SbDspVersionMajor ? vm->SbDspVersionMajor : 4;
            vm->SbDspResponse[1] = vm->SbDspVersionMinor;
            vm->SbDspResponseHead = 0;
            vm->SbDspResponseCount = 2;
            break;
        case 0x10: case 0x40: case 0xe0:
            vm->SbDspArgsNeeded = 1; break;
        case 0x41:
            vm->SbDspArgsNeeded = 2; break;
        case 0x14: case 0x24: case 0x48: case 0x80:
            vm->SbDspArgsNeeded = 2; break;
        case 0x1c: case 0xc0: case 0xc6:
            vm->SbDspArgsNeeded = byte == 0x1cu ? 2 : 3; break;
        case 0xd0: case 0xd1: case 0xd3: case 0xd4: case 0xd8:
            break; /* DMA pause/resume and speaker controls are acknowledged. */
        default:
            break;
        }
        if (!vm->SbDspArgsNeeded)
            dos_sb_hw_command(vm, byte, NULL, 0);
        return;
    }
    if ((port >= 0x02f8u && port <= 0x02ffu) ||
        (port >= 0x03f8u && port <= 0x03ffu)) {
        unsigned uart_index = port >= 0x03f8u;
        U8 *uart = vm->UartRegisters[uart_index];
        unsigned reg = port & 7u;
        if (reg == 2u) {
            uart[reg] = byte;
            if (byte & 0x02u) vm->UartRxHead[uart_index] = vm->UartRxTail[uart_index] = 0;
            return;
        }
        if (reg == 4u) {
            U8 previous = uart[4];
            uart[4] = byte;
            if ((byte & 0x03u) == 0x03u &&
                (previous & 0x03u) != 0x03u) {
                if (!vm->SerialMouseActive) {
                    vm->SerialMouseActive = TRUE;
                    vm->SerialMouseUart = (U8)uart_index;
                    vm->SerialMouseHostSeq = DosHostMouse(vm)->Seq;
                    vm->SerialMouseHostX = DosHostMouse(vm)->X;
                    vm->SerialMouseHostY = DosHostMouse(vm)->Y;
                    vm->SerialMouseButtons = DosHostMouse(vm)->Buttons;
                    vm->VideoDirty = TRUE;
                }
                if (vm->SerialMouseUart == uart_index)
                    dos_uart_rx_push(vm, uart_index, 'M');
            }
            return;
        }
        if (reg == 0u && !(uart[3] & 0x80u)) return; /* transmit byte */
        if (reg == 0u || reg == 1u || reg == 3u || reg == 7u) uart[reg] = byte;
        return;
    }
    switch (port) {
    case 0x0000: case 0x0001: case 0x0002: case 0x0003:
    case 0x0004: case 0x0005: case 0x0006: case 0x0007: {
        U16 *reg = &vm->DmaRegisters[port];
        if (vm->DmaFlipFlop) *reg = (U16)((*reg & 0x00ffu) | ((U16)byte << 8));
        else *reg = (U16)((*reg & 0xff00u) | byte);
        vm->DmaFlipFlop ^= 1u;
        break;
    }
    case 0x0008: vm->DmaCommand = byte; break;
    case 0x0009: /* DMA request register: retain in channel mask state. */
        vm->DmaStatus = (U8)((vm->DmaStatus & 0xf0u) | (byte & 0x0fu));
        break;
    case 0x000a:
        if (byte & 0x04u) vm->DmaMask |= (U8)(1u << (byte & 3u));
        else vm->DmaMask &= (U8)~(1u << (byte & 3u));
        break;
    case 0x000b: vm->DmaMode = byte; break;
    case 0x000c: vm->DmaFlipFlop = 0; break;
    case 0x000d:
        vm->DmaFlipFlop = 0;
        vm->DmaStatus = 0;
        vm->DmaMask = 0x0fu;
        vm->DmaCommand = 0;
        vm->DmaMode = 0;
        break;
    case 0x000e: vm->DmaMask = 0; break;
    case 0x000f: vm->DmaMask = byte & 0x0fu; break;
    case 0x0080: case 0x0081: case 0x0082: case 0x0083:
    case 0x0084: case 0x0085: case 0x0086: case 0x0087:
    case 0x0088: case 0x0089: case 0x008a: case 0x008b:
    case 0x008c: case 0x008d: case 0x008e: case 0x008f:
        vm->DmaPageRegisters[port - 0x80u] = byte;
        break;
    case 0x0060: /* 8042 command/data port: acknowledge, do not control host */
    case 0x0064:
        break;
    case 0x0061: vm->VgaSpeakerControl = byte; break;
    case 0x0020: case 0x00a0: /* PIC command: ICW1 or EOI */
        if (byte & 0x10u) {
            if (port == 0x0020) vm->PicMasterInitStep = 1;
            else vm->PicSlaveInitStep = 1;
        }
        /* EOI is acknowledged by the virtual interrupt dispatcher. */
        break;
    case 0x0021:
        if (vm->PicMasterInitStep == 1) {
            vm->PicMasterVectorBase = byte & 0xf8u;
            vm->PicMasterInitStep = 2;
        } else if (vm->PicMasterInitStep == 2) {
            vm->PicMasterInitStep = 3; /* ICW3 */
        } else if (vm->PicMasterInitStep == 3) {
            vm->PicMasterInitStep = 0; /* ICW4 */
        } else vm->PicMasterMask = byte;
        break;
    case 0x00a1:
        if (vm->PicSlaveInitStep == 1) {
            vm->PicSlaveVectorBase = byte & 0xf8u;
            vm->PicSlaveInitStep = 2;
        } else if (vm->PicSlaveInitStep == 2) {
            vm->PicSlaveInitStep = 3;
        } else if (vm->PicSlaveInitStep == 3) {
            vm->PicSlaveInitStep = 0;
        } else vm->PicSlaveMask = byte;
        break;
    case 0x03c0:
        if (!vm->VgaAttributeDataPhase) {
            vm->VgaAttributeIndex = byte & 0x1fu;
            vm->VgaAttributeDataPhase = TRUE;
        } else {
            if (vm->VgaAttributeIndex < sizeof(vm->VgaAttribute)) {
                vm->VgaAttribute[vm->VgaAttributeIndex] = byte;
                vm->VideoDirty = TRUE;
            }
            vm->VgaAttributeDataPhase = FALSE;
        }
        break;
    case 0x03c2:
        vm->VgaMiscOutput = byte;
        break;
    case 0x03c4: vm->VgaSequencerIndex = byte; break;
    case 0x03ce: vm->VgaGraphicsIndex = byte; break;
    case 0x03d4: vm->VgaCrtcIndex = byte; break;
    case 0x03c6: vm->VgaDacMask = byte; vm->VideoDirty = TRUE; break;
    case 0x03c7:
        vm->VgaDacReadIndex = byte;
        vm->VgaDacComponent = 0;
        break;
    case 0x03c8:
        vm->VgaDacWriteIndex = byte;
        vm->VgaDacComponent = 0;
        break;
    case 0x03c9:
      vm->VideoDirty = TRUE;
        vm->VgaDac[(UINT32)vm->VgaDacWriteIndex * 3u +
                   vm->VgaDacComponent] = byte & 0x3fu;
        if (++vm->VgaDacComponent == 3) {
            vm->VgaDacComponent = 0;
            vm->VgaDacWriteIndex++;
        }
        break;
    case 0x03c5:
        if (vm->VgaSequencerIndex == 4 && vm->VideoMode == 0x13u &&
            ((vm->VgaSequencer[4] ^ byte) & 8u)) {
            for (U32 i = 0; i < VGA_MODE13_SIZE; i++) {
                if (byte & 8u) vm->Memory[VGA_MEMORY_BASE + i] = vm->VgaPlanes[i & 3u][i >> 2];
                else vm->VgaPlanes[i & 3u][i >> 2] = vm->Memory[VGA_MEMORY_BASE + i];
            }
        }
        vm->VgaSequencer[vm->VgaSequencerIndex % sizeof(vm->VgaSequencer)] = byte;
        break;
    case 0x03cf:
        vm->VgaGraphics[vm->VgaGraphicsIndex % sizeof(vm->VgaGraphics)] = byte;
        break;
    case 0x03d5:
        vm->VideoDirty = TRUE;
        vm->VgaCrtc[vm->VgaCrtcIndex % sizeof(vm->VgaCrtc)] = byte;
        break;
    case 0x0043: {
        U8 channel = byte >> 6;
        U8 access = (byte >> 4) & 3u;
        if (channel == 0u) {
            if (!access) {
                /* Counter-latch commands do not replace the mode word. */
                vm->PitLatch = dos_pit_counter(vm);
                vm->PitLatched = TRUE;
                vm->PitReadPhase = 0;
            } else {
                vm->PitControl = byte;
                vm->PitReadPhase = 0;
                vm->PitWritePhase = 0;
                vm->PitLatched = FALSE;
            }
        }
        break;
    }
    case 0x0040: {
        U8 access = (vm->PitControl >> 4) & 3u;
        if (access == 1u) vm->PitReload = (U16)((vm->PitReload & 0xff00u) | byte);
        else if (access == 2u) vm->PitReload = (U16)((vm->PitReload & 0x00ffu) | ((U16)byte << 8));
        else if (!vm->PitWritePhase) {
            vm->PitLowByte = byte;
            vm->PitWritePhase = 1;
        } else {
            vm->PitReload = (U16)(vm->PitLowByte | ((U16)byte << 8));
            vm->PitWritePhase = 0;
        }
        break;
    }
    case 0x0041: case 0x0042:
        break;
    default:
        break;
    }
    if (vm->PortTraceCount < 64u) {
        vm->PortTraceCount++;
        KeLog("[dos-vga] OUT port=0x%04x value=0x%08x width=%u\n",
              port, value, width);
    }
}

/* Unscaled VGA scanout, shared by each HWND's crop in the X11 backend. */
void DosVideoDimensions(DOS_VM *vm, U16 *width, U16 *height)
{
    *width = (vm->VideoMode == 0x13 || vm->VideoMode == 4 ||
              vm->VideoMode == 5 || vm->VideoMode == 0x0d) ? 320 : 640;
    *height = vm->VideoMode == 0x0f || vm->VideoMode == 0x10 ? 350 :
              vm->VideoMode == 0x11 || vm->VideoMode == 0x12 ? 480 : 200;
}
U32 DosVideoRgbPixel(DOS_VM *vm, INT32 x, INT32 y)
{
    U16 width,height; DosVideoDimensions(vm,&width,&height);
    if (x<0 || y<0 || x>=width || y>=height) return 0;
    U8 index=0;
    U32 start=((U32)vm->VgaCrtc[0x0c]<<8)|vm->VgaCrtc[0x0d];
    if (vm->VideoMode>=0x0d && vm->VideoMode<=0x12) {
        U32 offset=(start+(U32)y*(width/8u)+(U32)x/8u)&0xffff;
        for (U32 p=0;p<4;p++) index|=((vm->VgaPlanes[p][offset]>>(7-(x&7)))&1)<<p;
        U8 select=vm->VgaAttribute[0x14];
        index=vm->VgaAttribute[index]&63;
        if (vm->VgaAttribute[0x10]&0x80) index=(index&15)|((select&3)<<4);
        index=(index|((select&12)<<4))&vm->VgaDacMask;
    } else if (vm->VideoMode==0x13) {
        index=(vm->VgaSequencer[4]&8)?vm->Memory[0xa0000+y*320+x]:
              vm->VgaPlanes[x&3][(start+y*80+x/4)&0xffff];
    } else if (vm->VideoMode>=4 && vm->VideoMode<=6) {
        U8 b=vm->Memory[0xb8000+dos_cga_scanline_offset(y)+(vm->VideoMode==6?x/8:x/4)];
        U8 value=vm->VideoMode==6?((b>>(7-(x&7)))&1):((b>>(6-2*(x&3)))&3);
        U8 c=vm->VgaAttribute[value];
        U32 red=((c&4)?42:0)+((c&32)?21:0),g=((c&2)?42:0)+((c&16)?21:0),blue=((c&1)?42:0)+((c&8)?21:0);
        return (((red*255+31)/63)<<16)|(((g*255+31)/63)<<8)|((blue*255+31)/63);
    } else return 0;
    U32 at=(U32)index*3;
    U8 red=vm->VgaDac[at],g=vm->VgaDac[at+1],b=vm->VgaDac[at+2];
    return ((U32)((red<<2)|(red>>4))<<16)|((U32)((g<<2)|(g>>4))<<8)|((b<<2)|(b>>4));
}
