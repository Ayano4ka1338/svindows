/* Virtual VGA/CGA, PC timer/keyboard I/O and DOS video presentation. */
#include <svindows/Gfx.h>
#include <svindows/Console.h>
#include <svindows/Hal.h>
#include "Internal.h"

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
        HalConsoleAcquireFramebuffer();
        vm->VideoActive = TRUE;
    } else if (!needs_framebuffer && vm->VideoActive) {
        vm->VideoActive = FALSE;
        if (HalConsoleReleaseFramebuffer()) ConRedraw();
    }
    if (GfxAvailable() && needs_framebuffer) GfxClear(GfxColor(0, 0, 0));
}

void DosVideoInherit(DOS_VM *destination, const DOS_VM *source)
{
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

void DosVideoPresent(DOS_VM *vm)
{
    if (!vm || !vm->VideoActive || !vm->VideoDirty ||
        !GfxAvailable() || !vm->Memory)
        return;

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
        const U8 *cga = vm->Memory + 0xb8000u;
        for (UINT32 y = 0; y < height; y++) {
            UINT32 sy = y * 200u / height;
            UINT32 row = dos_cga_scanline_offset(sy);
            UINT32 *dst = GfxFramebuffer.Base + y * stride;
            for (UINT32 x = 0; x < width; x++) {
                UINT32 sx = x * source_width / width;
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
        for (unsigned i = 0; i < 16; i++) colors[i] = dos_ega_color(vm->VgaAttribute[i]);
        for (U32 y = 0; y < height; y++) {
            U32 source_y = y * sh / height;
            for (U32 x = 0; x < width; x++) {
                U32 source_x = x * sw / width;
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

    UINT32 colors[256];
    for (unsigned i = 0; i < 256; i++) colors[i] = dos_vga_color(vm, (U8)i);
    U32 start = ((U32)vm->VgaCrtc[0x0c] << 8) | vm->VgaCrtc[0x0d];
    for (UINT32 y = 0; y < height; y++) {
        UINT32 sy = y * 200u / height;
        UINT32 *dst = GfxFramebuffer.Base + y * stride;
        for (UINT32 x = 0; x < width; x++) {
            UINT32 sx = x * 320u / width;
            U8 pixel = (vm->VgaSequencer[4] & 8u) ? source[sy * 320u + sx]
                : vm->VgaPlanes[sx & 3u][(start + sy * 80u + sx / 4u) & 0xffffu];
            UINT32 color = colors[pixel];
            dst[x] = color;
        }
    }
    dos_video_draw_mouse_cursor(vm, left, top, 1u, 320u, 200u, 640u);
    vm->VideoDirty = FALSE;
}

void DosVideoStop(DOS_VM *vm)
{
    if (!vm) return;
    if (vm->VideoActive) {
        vm->VideoActive = FALSE;
        if (HalConsoleReleaseFramebuffer()) ConRedraw();
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
    while (KbdReadMirroredInputEvent(&event)) {
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
    U32 sequence = MouseState.Seq;
    if (sequence == vm->SerialMouseHostSeq) return;

    INT32 dx = MouseState.X - vm->SerialMouseHostX;
    INT32 dy = MouseState.Y - vm->SerialMouseHostY;
    U8 buttons = MouseState.Buttons;
    vm->SerialMouseHostSeq = sequence;
    vm->SerialMouseHostX = MouseState.X;
    vm->SerialMouseHostY = MouseState.Y;
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

U32 DosPortIn(DOS_VM *vm, U16 port, unsigned width)
{
    if (width > 8) {
        U32 combined = 0;
        for (unsigned i = 0; i < width / 8; i++) combined |= DosPortIn(vm, (U16)(port + i), 8) << (i * 8);
        return combined;
    }
    U32 value;
    U8 byte = 0xffu;
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
    case 0x0064: byte = (vm->KeyboardDataReady || KbdMirroredInputEventAvailable()) ? 0x01u : 0x00u; break;
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
                    vm->SerialMouseHostSeq = MouseState.Seq;
                    vm->SerialMouseHostX = MouseState.X;
                    vm->SerialMouseHostY = MouseState.Y;
                    vm->SerialMouseButtons = MouseState.Buttons;
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
            if (vm->VgaAttributeIndex < sizeof(vm->VgaAttribute))
                vm->VgaAttribute[vm->VgaAttributeIndex] = byte;
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
