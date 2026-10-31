#!/usr/bin/env python3
"""Source-level checks for the DOS/DPMI virtual hardware path."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def require(path: str, fragments: tuple[str, ...]) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    missing = [item for item in fragments if item not in text]
    if missing:
        raise SystemExit(f"FAIL {path}: missing {missing}")
    print(f"PASS {path}")

require("kernel/dos/Cpu.c", (
    "static int dos_dispatch_hardware(void)",
    "KbdReadMirroredInputEvent(&event)",
    "protected_interrupt(timer_vector, 0",
    "protected_interrupt(keyboard_vector, 0",
    "dos_timer_update(vm)", "vm->PicMasterMask & 0x01u",
    "vm->PicMasterVectorBase + 0u", "vm->PicMasterVectorBase + 1u",
    "if (dos_dispatch_hardware()) return",
    "DosPortIn(vm, port, io_width)", "DosPortOut(vm, port",
    "protected string I/O count", "op >= 0x6c && op <= 0x6f",
    "if (client32)", "vm->Registers32[i] = (U32)vm->Registers[i]",
    "DPMI 32-bit entry registers",
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
print("PASS DOS/DPMI virtual hardware source checks")
