#!/usr/bin/env python3
"""Check DOS INT 33h cursor composition and forbid a synthetic Windows cursor."""
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
  BOOLEAN MouseInitialized, MouseDisabled, MouseConditionalOff;
  BOOLEAN IsWindows100, IsWindows200;
} DOS_VM;
'''

checks = r'''
int main(void) {
  enum { W = 1024, H = 768 };
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
  assert(pixels[20 * W + 40] == 0x000000u);
  assert(pixels[20 * W + 41] == 0x000000u);
  assert(pixels[21 * W + 41] == 0xffffffu);
  assert(pixels[20 * W + 42] == 0x123456u);

  /* DOS mouse hotspot is applied before clipping and scaling. */
  for (unsigned i = 0; i < W * H; ++i) pixels[i] = 0x123456u;
  vm.MouseX = 50; vm.MouseY = 30;
  vm.MouseHotX = 2; vm.MouseHotY = 3;
  dos_video_draw_mouse_cursor(&vm, 0, 0, 1, 640, 200, 640);
  assert(pixels[27 * W + 48] == 0x000000u);

  /* DOS INT 33h hide and disable state still work. */
  for (unsigned i = 0; i < W * H; ++i) pixels[i] = 0x123456u;
  vm.MouseHideCount = 1;
  vm.MouseHotX = vm.MouseHotY = 0;
  dos_video_draw_mouse_cursor(&vm, 0, 0, 1, 640, 200, 640);
  assert(pixels[30 * W + 50] == 0x123456u);
  vm.MouseHideCount = 0;
  vm.MouseDisabled = TRUE;
  dos_video_draw_mouse_cursor(&vm, 0, 0, 1, 640, 200, 640);
  assert(pixels[30 * W + 50] == 0x123456u);
  vm.MouseDisabled = FALSE;

  /* Windows 1.x must never receive a cursor from this compositor, even if
     INT 33h state was initialized by an unrelated DOS probe. */
  vm.IsWindows100 = TRUE;
  for (unsigned i = 0; i < W * H; ++i) pixels[i] = 0x123456u;
  dos_video_draw_mouse_cursor(&vm, 0, 0, 1, 640, 200, 640);
  assert(pixels[30 * W + 50] == 0x123456u);
  vm.IsWindows100 = FALSE;
  vm.IsWindows200 = TRUE;
  for (unsigned i = 0; i < W * H; ++i) pixels[i] = 0x123456u;
  dos_video_draw_mouse_cursor(&vm, 0, 0, 1, 640, 200, 640);
  assert(pixels[30 * W + 50] == 0x123456u);
  vm.IsWindows200 = FALSE;

  /* A custom DOS cursor with an all-zero AND/XOR mask is fully black. */
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

# Presenters may call the DOS cursor after painting guest memory, but Windows
# 1.x must have no host-side cursor overlay or host-pointer position path.
present = source[source.index("void DosVideoPresent("):source.index("void DosVideoStop(")]
assert present.count("dos_video_draw_mouse_cursor(vm,") == 3
assert "vm->VideoDirty = FALSE;" in present
assert "MouseState" not in implementation
assert "SerialMouseActive" not in implementation
assert "vm->IsWindows100" in implementation
assert "host_overlay" not in source
serial_poll = source[source.index("void DosSerialMousePoll("):source.index("BOOLEAN DosSerialMouseIrqPending(")]
assert "vm->VideoDirty" not in serial_poll
assert "WindowsMouseHostSeq" not in serial_poll
assert "SerialMouseDeltaX" in serial_poll and "SerialMouseDeltaY" in serial_poll
init_source = (ROOT / "kernel/dos/Dos.c").read_text()
assert "vm->IsWindows100 = dos_command_is_win100_bin(command);" in init_source
interrupt_source = (ROOT / "kernel/dos/Interrupt.c").read_text()
mouse_poll = interrupt_source[interrupt_source.index("static void dos_mouse_poll("):interrupt_source.index("void DosMousePoll(")]
assert "vm->VideoDirty = TRUE;" in mouse_poll
mouse_ini = (ROOT / "user/d/windows1/MOUSE.INI").read_text()
assert "MouseType=Serial1" in mouse_ini
assert "MouseType=PS2" not in mouse_ini
windows_files = {p.name.upper() for p in (ROOT / "user/d/windows1").iterdir() if p.is_file()}
assert {"MOUSE.COM", "MOUSE.SYS"}.issubset(windows_files)
print("PASS DOS INT 33h cursor, Windows-native-cursor exclusion, COM1 serial mouse path and no host cursor overlay")
