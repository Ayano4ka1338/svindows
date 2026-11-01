#!/usr/bin/env python3
"""Cross-check the Windows 1.01 DOS-version probe against the DOS 3.x ABI."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
header = (ROOT / "kernel/dos/Internal.h").read_text(encoding="utf-8")
dos_source = (ROOT / "kernel/dos/Dos.c").read_text(encoding="utf-8")
interrupt = (ROOT / "kernel/dos/Interrupt.c").read_text(encoding="utf-8")
win100 = (ROOT / "user/d/windows1/WIN100.BIN").read_bytes()


def macro(name: str) -> int:
    match = re.search(rf"^#define\s+{name}\s+(0x[0-9a-fA-F]+|[0-9]+)u?\b", header, re.M)
    if not match:
        raise SystemExit(f"FAIL missing {name}")
    return int(match.group(1), 0)


lists = macro("DOS_LISTS_OFFSET")
sda = macro("DOS_SDA_OFFSET")
sda_size = macro("DOS_SDA_SIZE")
if lists != 0x22:
    raise SystemExit(f"FAIL DOS 3.x AH=52h offset is {lists:#x}, expected 0x22")
if sda != 0x218:
    raise SystemExit(f"FAIL DOS 3.x SDA offset is {sda:#x}, expected 0x218")
if sda + 0x0C != lists + 0x202:
    raise SystemExit("FAIL SDA DTA and List-of-Lists DTA fields do not alias")
if sda + sda_size > 0xA40:
    raise SystemExit("FAIL SDA overlaps the DOS DPB area")
if "0x2203u" not in dos_source:
    raise SystemExit("FAIL WIN100.BIN is not given the DOS 3.34 version override")
if "r[BX] = DOS_LISTS_OFFSET" not in interrupt:
    raise SystemExit("FAIL AH=52h does not expose DOS_LISTS_OFFSET")

# The retail Windows 1.01 binary probes DOS version (AH=30h), List of Lists
# (AH=52h), and the DOS 3.x swappable data area (AX=5D06h).
for signature, description in (
    (bytes.fromhex("B4 30 CD 21 3C 02 72 04 3C 04"), "DOS version probe"),
    (bytes.fromhex("B4 52 CD 21 8D 5F 04"), "List-of-Lists/SFT probe"),
    (bytes.fromhex("B8 06 5D CD 21"), "DOS 3.x SDA probe"),
):
    if signature not in win100:
        raise SystemExit(f"FAIL WIN100.BIN missing {description}")

print("PASS Windows 1.01 DOS 3.x version, List-of-Lists and SDA ABI layout")
