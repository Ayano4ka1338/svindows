/* Virtual VGA, PC timer/keyboard I/O and mode-13h presentation for DOS VMs. */
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

U8 DosVideoReadMemory(DOS_VM *vm, U32 linear)
{
    if (vm->VideoMode == 0x13u && !(vm->VgaSequencer[4] & 8u))
        return vm->VgaPlanes[vm->VgaGraphics[4] & 3u][linear - VGA_MEMORY_BASE];
    return vm->Memory[linear];
}

void DosVideoWriteMemory(DOS_VM *vm, U32 linear, U8 value)
{
    vm->VideoDirty = TRUE;
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
    KeLog("[dos-vga] video mode=%02x\n", vm->VideoMode);
    if (vm->VideoMode == 0x13u) {
        vm->VgaSequencer[2] = 0x0fu;
        vm->VgaSequencer[4] = 0x0eu;
        vm->VgaCrtc[0x0c] = vm->VgaCrtc[0x0d] = 0;
        memset(vm->VgaPlanes, 0, sizeof(vm->VgaPlanes));
        if (!vm->VideoActive && GfxAvailable()) {
            HalConsoleAcquireFramebuffer();
            vm->VideoActive = TRUE;
        }
        vm->VgaDacMask = 0xffu;
        vm->VgaDacWriteIndex = 0;
        vm->VgaDacReadIndex = 0;
        vm->VgaDacComponent = 0;
        /* Deterministic EGA-like fallback palette until the program loads its
         * own VGA DAC palette through ports 3C8h/3C9h. */
        static const U8 ega[16][3] = {
            {0,0,0},{0,0,42},{0,42,0},{0,42,42},
            {42,0,0},{42,0,42},{42,21,0},{42,42,42},
            {21,21,21},{21,21,63},{21,63,21},{21,63,63},
            {63,21,21},{63,21,63},{63,63,21},{63,63,63}
        };
        for (unsigned i = 0; i < 16; i++)
            memcpy(&vm->VgaDac[i * 3u], ega[i], 3);
        for (unsigned i = 16; i < 256; i++) {
            U8 gray = (U8)((i - 16u) * 63u / 239u);
            vm->VgaDac[i * 3u] = gray;
            vm->VgaDac[i * 3u + 1] = gray;
            vm->VgaDac[i * 3u + 2] = gray;
        }
        if (GfxAvailable()) GfxClear(GfxColor(0, 0, 0));
    } else if (vm->VideoActive) {
        vm->VideoActive = FALSE;
        if (HalConsoleReleaseFramebuffer()) ConRedraw();
    }
}

void DosVideoPresent(DOS_VM *vm)
{
    if (!vm || !vm->VideoActive || !vm->VideoDirty || vm->VideoMode != 0x13u ||
        !GfxAvailable() || !vm->Memory)
        return;

    UINT32 width = GfxFramebuffer.Width;
    UINT32 height = GfxFramebuffer.Height;
    UINT32 stride = GfxFramebuffer.Pitch / sizeof(UINT32);
    if (!width || !height || !stride || !GfxFramebuffer.Base) return;

    UINT32 scale_x = width / 320u;
    UINT32 scale_y = height / 200u;
    UINT32 scale = scale_x < scale_y ? scale_x : scale_y;
    if (!scale) scale = 1;
    if (scale > 4) scale = 4;
    UINT32 out_w = 320u * scale;
    UINT32 out_h = 200u * scale;
    UINT32 left = width > out_w ? (width - out_w) / 2u : 0;
    UINT32 top = height > out_h ? (height - out_h) / 2u : 0;
    UINT32 draw_w = width - left < out_w ? width - left : out_w;
    UINT32 draw_h = height - top < out_h ? height - top : out_h;
    UINT8 *source = vm->Memory + VGA_MEMORY_BASE;

    UINT32 colors[256];
    for (unsigned i = 0; i < 256; i++) colors[i] = dos_vga_color(vm, (U8)i);
    U32 start = ((U32)vm->VgaCrtc[0x0c] << 8) | vm->VgaCrtc[0x0d];
    for (UINT32 y = 0; y < draw_h; y++) {
        UINT32 sy = y / scale;
        UINT32 *dst = GfxFramebuffer.Base + (top + y) * stride + left;
        if (y % scale) {
            memcpy(dst, dst - stride, draw_w * sizeof(*dst));
            continue;
        }
        for (UINT32 sx = 0; sx < 320u && sx * scale < draw_w; sx++) {
            U8 pixel = (vm->VgaSequencer[4] & 8u) ? source[sy * 320u + sx]
                : vm->VgaPlanes[sx & 3u][(start + sy * 80u + sx / 4u) & 0xffffu];
            UINT32 color = colors[pixel];
            for (UINT32 dx = 0; dx < scale && sx * scale + dx < draw_w; dx++)
                dst[sx * scale + dx] = color;
        }
    }
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
    return 0;
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

U32 DosPortIn(DOS_VM *vm, U16 port, unsigned width)
{
    if (width > 8) {
        U32 combined = 0;
        for (unsigned i = 0; i < width / 8; i++) combined |= DosPortIn(vm, (U16)(port + i), 8) << (i * 8);
        return combined;
    }
    U32 value;
    U8 byte = 0xffu;
    switch (port) {
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
    switch (port) {
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
