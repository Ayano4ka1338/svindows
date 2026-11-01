#!/usr/bin/env python3
"""Source-level checks for the DOS/DPMI virtual hardware path."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def require(path: str, fragments: tuple[str, ...]) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    missing = [item for item in fragments if item not in text]
    if missing:
        raise SystemExit(f"FAIL {path}: missing {missing}")
    print(f"PASS {path}")

require("kernel/dos/Cpu.c", (
    "static int dos_dispatch_hardware(DOS_VM *vm)",
    "KbdReadMirroredInputEvent(&event)",
    "protected_interrupt(timer_vector, 0",
    "protected_interrupt(keyboard_vector, 0",
    "dos_timer_update(vm)", "vm->PicMasterMask & 0x01u",
    "vm->PicMasterVectorBase + 0u", "vm->PicMasterVectorBase + 1u",
    "dos_hardware_poll_due(vm)", "if (dos_hardware_poll_due(vm) && dos_dispatch_hardware(vm)) return",
    "DosPortIn(vm, port, io_width)", "DosPortOut(vm, port",
    "protected string I/O count", "op >= 0x6c && op <= 0x6f",
    "if (client32)", "0xffffu, 0xfau, 0u", "0xffffu, 0xf2u, client32 ? 4u : 0u",
    "DPMI 32-bit entry registers",
    "if (environment) ww(PSP, 0x2cu, selectors[5]);",
    "0xffu, 0xf2u, client32 ? 4u : 0u",
))
require("kernel/dos/Video.c", (
    "void DosVideoSetMode", "vm->VideoMode == 0x13u",
    "VGA_MEMORY_BASE", "DosVideoPresent", "HalConsoleAcquireFramebuffer",
    "case 0x03c9", "case 0x0060", "case 0x03da",
    "dos_pit_read", "vm->PicMasterMask = byte", "PicMasterInitStep",
    "PicMasterVectorBase = byte & 0xf8u", "PicSlaveVectorBase = byte & 0xf8u",
    "PitLatched",
    "KbdReadMirroredInputEvent(&event)",
    "KbdMirroredInputEventAvailable()",
))
require("kernel/hal/Keyboard.c", (
    "KbdInputMirrorAcquire", "KbdInputMirrorRelease",
    "KbdReadMirroredInputEvent", "KbdMirroredInputEventAvailable",
))
require("kernel/dos/Dos.c", (
    "KbdInputMirrorAcquire();", "vm->KeyboardMirrorActive = TRUE;",
    "vm->PicMasterVectorBase = 0x08u;", "vm->PicSlaveVectorBase = 0x70u;",
))
require("kernel/dos/Interrupt.c", (
    "static U8 dos_parse_fcb(void)",
    "case 0x29:",
    "set8(0, dos_parse_fcb());",
    "for (U8 i = 12; i < 16; i++)",
))
print("PASS DOS/DPMI virtual hardware and DOS FCB source checks")

# Compile and execute the production INT 21h/AH=29h parser against a compact
# real-mode memory model. Windows 1.01 calls it twice for PSP:005C/006C, so the
# PSP command tail at PSP:0080 must remain intact.
interrupt_source = (ROOT / "kernel/dos/Interrupt.c").read_text(encoding="utf-8")
fcb_start = interrupt_source.index("static BOOLEAN dos_fcb_terminator")
fcb_end = interrupt_source.index("\nstatic void dos_bios_keyboard", fcb_start)
fcb_source = interrupt_source[fcb_start:fcb_end]
fcb_prelude = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef uint8_t U8;
typedef uint16_t U16;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
#define AX 0
#define SI 6
#define DI 7
#define ES 0
#define DS 3
static U8 memory[1u << 20];
static U16 regs[8], segments[6];
#define r regs
#define seg segments
static U8 get8(unsigned n) { return n < 4 ? (U8)r[n] : (U8)(r[n - 4] >> 8); }
static unsigned address(U16 s, U16 o) { return (((unsigned)s << 4) + o) & 0xfffffu; }
static U8 rb(U16 s, U16 o) { return memory[address(s, o)]; }
static void wb(U16 s, U16 o, U8 v) { memory[address(s, o)] = v; }
'''
fcb_checks = r'''
int main(void) {
  memset(memory, 0, sizeof(memory));
  segments[DS] = segments[ES] = 0x1000;
  memory[0x10080] = 0;
  memory[0x10081] = 0x0d;
  for (unsigned i = 0x10082; i < 0x100a0; ++i) memory[i] = 0x5a;
  regs[AX] = 0x2901; regs[SI] = 0x81; regs[DI] = 0x5c;
  assert(dos_parse_fcb() == 0 && regs[SI] == 0x81);
  assert(memory[0x10080] == 0 && memory[0x10081] == 0x0d);
  for (unsigned i = 1; i <= 11; ++i) assert(memory[0x1005c + i] == ' ');
  assert(memory[0x10068] == 0 && memory[0x1006b] == 0);
  assert(memory[0x1006c] == 0);

  memset(memory, 0, sizeof(memory));
  segments[DS] = 0x2000; segments[ES] = 0x2100;
  const char *spec = " c:win.com tail";
  for (unsigned i = 0; spec[i]; ++i) memory[0x20000 + i] = (U8)spec[i];
  regs[AX] = 0x2901; regs[SI] = 0; regs[DI] = 0x20;
  assert(dos_parse_fcb() == 0);
  assert(memory[0x21020] == 3);
  assert(!memcmp(memory + 0x21021, "WIN     COM", 11));
  assert(regs[SI] == 10);

  memset(memory, 0, sizeof(memory));
  segments[DS] = 0x2000; segments[ES] = 0x2100;
  const char *wild = "C:AB*.EX? tail";
  for (unsigned i = 0; wild[i]; ++i) memory[0x20000 + i] = (U8)wild[i];
  regs[AX] = 0x2900; regs[SI] = 0; regs[DI] = 0x20;
  assert(dos_parse_fcb() == 1);
  assert(!memcmp(memory + 0x21021, "AB??????EX?", 11));

  memset(memory, 0, sizeof(memory));
  segments[DS] = 0x2000; segments[ES] = 0x2100;
  memory[0x20000] = 'D'; memory[0x20001] = ':'; memory[0x20002] = 'X';
  regs[AX] = 0x2900; regs[SI] = 0; regs[DI] = 0x20;
  assert(dos_parse_fcb() == 0xff && regs[SI] == 0);

  memset(memory, 0, sizeof(memory));
  segments[DS] = 0x2000; segments[ES] = 0x2100;
  memory[0x20000] = '1'; memory[0x20001] = ':'; memory[0x20002] = 'X';
  regs[AX] = 0x2900; regs[SI] = 0; regs[DI] = 0x20;
  assert(dos_parse_fcb() == 0xff && regs[SI] == 0);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="sv-dos-fcb-") as directory:
    cfile = Path(directory) / "fcb.c"
    exe = Path(directory) / "fcb"
    cfile.write_text(fcb_prelude + fcb_source + fcb_checks, encoding="utf-8")
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=c11", "-O2",
                    "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)
print("PASS DOS INT 21h/AH=29h FCB parsing and PSP preservation")
