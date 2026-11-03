#!/usr/bin/env python3
"""Regression checks for DOS/Windows/XMS INT 2Fh multiplex semantics."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
interrupt = (root / "kernel/dos/Interrupt.c").read_text(encoding="utf-8")
dos = (root / "kernel/dos/Dos.c").read_text(encoding="utf-8")

start = interrupt.index("  if (n == 0x2f) {")
end = interrupt.index("  if (n == 0x20)", start)
handler = interrupt[start:end]
for service in ("case 0x1600:", "case 0x1605:", "case 0x1606:",
                "case 0x160a:", "case 0x1680:", "case 0x1686:",
                "case 0x1687:", "case 0x4300:", "case 0x4310:"):
    assert service in handler, f"missing INT 2Fh service {service}"
assert "set8(0, 0x00);" in handler
assert "seg[ES] = 0;" in handler and "r[BX] = 0;" in handler
assert "r[AX] = 0x1680; /* unsupported multiplex function */" not in handler
assert "seg[ES] = 0xf000u;" in handler and "r[BX] = 0x0600u;" in handler
assert "r[BX] = 0x0301;" in interrupt
for service in ("case 0x09: case 0x89:", "case 0x0e: case 0x8e:",
                "case 0x0f: case 0x8f:", "case 0x88:"):
    assert service in interrupt, f"missing XMS service {service}"
assert "r32[AX] = dos_xms_largest_free(vm);" in interrupt
assert "vm->DosVersion" in dos
print("PASS DOS INT 2Fh: Windows detection, XMS entry point, DPMI mode, yield")
