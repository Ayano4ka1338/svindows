#!/usr/bin/env python3
"""Host-side checks for DOSBox-style software mouse cursor composition."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "kernel/dos/Video.c").read_text()
start = source.index("/* The standard 16x16 Microsoft-compatible DOS arrow.")
end = source.index("\nvoid DosVideoPresent(", start)
implementation = source[start:end]

prelude = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef uint8_t U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef uint32_t UINT32;
typedef int16_t INT16;
typedef int32_t INT32;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
typedef struct {
  U32 *Base;
  U32 Pitch, Width, Height;
} GFX_FRAMEBUFFER;
typedef struct {
  INT32 X, Y;
  U8 Buttons;
  U32 Seq;
} MOUSE_STATE;
static MOUSE_STATE MouseState;
static GFX_FRAMEBUFFER GfxFramebuffer;
static U32 GfxColor(U8 r, U8 g, U8 b) {
  return ((U32)r << 16) | ((U32)g << 8) | b;
}
typedef struct {
  U16 MouseAndMask[64][8], MouseXorMask[64][8];
  U16 MouseX, MouseY;
  INT16 MouseHotX, MouseHotY;
  U16 MouseConditionalLeft, MouseConditionalTop;
  U16 MouseConditionalRight, MouseConditionalBottom;
  U8 MouseCursorWidthWords, MouseCursorHeight;
  U8 MouseHideCount;
  BOOLEAN MouseInitialized, SerialMouseActive;
  BOOLEAN MouseDisabled, MouseConditionalOff;
} DOS_VM;
'''
checks = r'''
int main(void) {
  enum { W = 640, H = 200 };
  U32 pixels[W * H];
  DOS_VM vm;
  memset(&vm, 0, sizeof(vm));
  for (unsigned i = 0; i < W * H; ++i) pixels[i] = 0x123456u;
  GfxFramebuffer.Base = pixels;
  GfxFramebuffer.Width = W;
  GfxFramebuffer.Height = H;
  GfxFramebuffer.Pitch = W * sizeof(U32);

  vm.MouseInitialized = TRUE;
  vm.MouseX = 40;
  vm.MouseY = 20;
  DosMouseSetDefaultCursor(&vm);
  assert(vm.MouseCursorWidthWords == 1 && vm.MouseCursorHeight == 16);
  assert(vm.MouseHotX == 0 && vm.MouseHotY == 0);
  assert(vm.MouseAndMask[0][0] == 0x3fff);
  assert(vm.MouseXorMask[1][0] == 0x4000);

  dos_video_draw_mouse_cursor(&vm, 0, 0, 1, 640, 200, 640);
  /* Arrow tip/outline are black, its interior is white, and the rest is
     transparent.  The guest framebuffer is not modified outside the mask. */
  assert(pixels[20 * W + 40] == 0x000000u);
  assert(pixels[20 * W + 41] == 0x000000u);
  assert(pixels[21 * W + 41] == 0xffffffu);
  assert(pixels[20 * W + 42] == 0x123456u);

  /* The hotspot is applied before clipping and scaling. */
  for (unsigned i = 0; i < W * H; ++i) pixels[i] = 0x123456u;
  vm.MouseX = 50; vm.MouseY = 30;
  vm.MouseHotX = 2; vm.MouseHotY = 3;
  dos_video_draw_mouse_cursor(&vm, 0, 0, 1, 640, 200, 640);
  assert(pixels[27 * W + 48] == 0x000000u);

  /* INT 33h hide suppresses the cursor, but an active serial mouse has no
     guest hide call, so the normal Windows pointer stays visible. */
  for (unsigned i = 0; i < W * H; ++i) pixels[i] = 0x123456u;
  vm.MouseHideCount = 1;
  vm.MouseHotX = vm.MouseHotY = 0;
  dos_video_draw_mouse_cursor(&vm, 0, 0, 1, 640, 200, 640);
  assert(pixels[30 * W + 50] == 0x123456u);
  vm.SerialMouseActive = TRUE;
  MouseState.X = 50; MouseState.Y = 30;
  dos_video_draw_mouse_cursor(&vm, 0, 0, 1, 640, 200, 640);
  assert(pixels[30 * W + 50] == 0x000000u);

  /* The Windows 1.x serial driver uses COM1 instead of INT 33h.  Its
     pointer overlay follows the host absolute position, remains visible if
     INT 33h hide/disable was called, and is not scaled with guest pixels. */
  for (unsigned i = 0; i < W * H; ++i) pixels[i] = 0x123456u;
  DosMouseSetDefaultCursor(&vm);
  vm.SerialMouseActive = TRUE;
  vm.MouseHideCount = 2;
  vm.MouseDisabled = TRUE;
  vm.MouseX = 10; vm.MouseY = 10;
  MouseState.X = 300; MouseState.Y = 100;
  dos_video_draw_mouse_cursor(&vm, 0, 0, 2, 320, 200, 640);
  assert(pixels[100 * W + 300] == 0x000000u);
  assert(pixels[101 * W + 301] == 0xffffffu);
  assert(pixels[10 * W + 10] == 0x123456u);
  vm.SerialMouseActive = FALSE;
  vm.MouseDisabled = FALSE;
  vm.MouseHideCount = 0;

  /* A custom cursor with an all-zero AND/XOR mask is fully black. */
  memset(vm.MouseAndMask, 0, sizeof(vm.MouseAndMask));
  memset(vm.MouseXorMask, 0, sizeof(vm.MouseXorMask));
  vm.MouseCursorWidthWords = 1;
  vm.MouseCursorHeight = 1;
  vm.MouseX = 100; vm.MouseY = 50;
  for (unsigned i = 0; i < W * H; ++i) pixels[i] = 0x123456u;
  dos_video_draw_mouse_cursor(&vm, 0, 0, 1, 320, 200, 640);
  assert(pixels[50 * W + 50] == 0x000000u); /* 320px image: 640->320 */
  assert(pixels[50 * W + 66] == 0x123456u);
  return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="sv-dos-cursor-") as directory:
    cfile = Path(directory) / "cursor.c"
    exe = Path(directory) / "cursor"
    cfile.write_text(prelude + implementation + checks)
    subprocess.run([
        os.environ.get("HOSTCC", "cc"), "-std=c11", "-Wall", "-Wextra",
        "-Werror", str(cfile), "-o", str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)

# Ensure all framebuffer presenters composite the cursor only after the guest
# image is painted, and that the software cursor remains outside guest memory.
present = source[source.index("void DosVideoPresent("):source.index("void DosVideoStop(")]
assert present.count("dos_video_draw_mouse_cursor(vm,") == 3
assert "vm->VideoDirty = FALSE;" in present
assert "DosVideoWriteMemory" not in implementation
serial_poll = source[source.index("void DosSerialMousePoll("):source.index("BOOLEAN DosSerialMouseIrqPending(")]
assert "vm->VideoDirty = TRUE;" in serial_poll
interrupt_source = (ROOT / "kernel/dos/Interrupt.c").read_text()
mouse_poll = interrupt_source[interrupt_source.index("static void dos_mouse_poll("):interrupt_source.index("void DosMousePoll(")]
assert "vm->VideoDirty = TRUE;" in mouse_poll
print("PASS DOS mouse default arrow, host serial pointer, masks, hotspot, hide state, scaling, movement invalidation, and CGA/EGA/VGA presenters")
