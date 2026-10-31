#!/usr/bin/env python3
"""Regression checks for FAT case preservation and Imlib2 mmap ABI."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
fat = (ROOT / "kernel/io/Fat.c").read_text(encoding="utf-8")
mem = (ROOT / "kernel/linux/Memory.c").read_text(encoding="utf-8")

assert "FAT_NTRES_LOWER_BASE 0x08u" in fat
assert "FAT_NTRES_LOWER_EXT  0x10u" in fat
assert "from_83_case(e->Name, e->NtReserved, out->Name)" in fat
assert "from_83(e->Name, out->ShortName)" in fat
assert "LINUX_MAP_PRIVATE | LINUX_MAP_SHARED" in mem
assert "map_type == LINUX_MAP_SHARED && (protection & LINUX_PROT_WRITE)" in mem
assert "Imlib2 to mmap image" in mem

# Mirror the FAT NTRes transformation for the canonical Alpine loader name.
def decode_83(raw: bytes, flags: int) -> str:
    base = raw[:8].rstrip(b" ").decode("ascii")
    ext = raw[8:11].rstrip(b" ").decode("ascii")
    if flags & 0x08:
        base = base.lower()
    if flags & 0x10:
        ext = ext.lower()
    return base + ("." + ext if ext else "")

assert decode_83(b"PNG     SO ", 0x18) == "png.so"
assert decode_83(b"JPEG    SO ", 0x18) == "jpeg.so"
assert decode_83(b"GIF     SO ", 0x18) == "gif.so"
print("feh-fs-check: PASS")
