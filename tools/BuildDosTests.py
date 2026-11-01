#!/usr/bin/env python3
"""Generate small DOS compatibility probes installed on the Svindows disk."""
from __future__ import annotations

import sys
from pathlib import Path


def com_psp_smoke() -> bytes:
    # COM entry has DS=ES=PSP. Verify INT 20h at PSP:0000 and the
    # INT 21h/RETF trampoline at PSP:0050 before reporting success.
    code = bytearray()
    code += b"\xA1\x00\x00"       # mov ax,[0000]
    code += b"\x3D\xCD\x20"       # cmp ax,20CDh
    first_fail = len(code); code += b"\x75\x00"
    code += b"\xA1\x50\x00"       # mov ax,[0050]
    code += b"\x3D\xCD\x21"       # cmp ax,21CDh
    second_fail = len(code); code += b"\x75\x00"
    success = len(code)
    code += b"\xBA\x00\x00\xB4\x09\xCD\x21"  # print success
    code += b"\xB8\x00\x4C\xCD\x21"              # exit 0
    failure = len(code)
    code += b"\xBA\x00\x00\xB4\x09\xCD\x21"  # print failure
    code += b"\xB8\x01\x4C\xCD\x21"              # exit 1
    message_offset = 0x100 + len(code)
    ok = b"DOS-COM PASS: PSP vectors/INT 21h/exit$"
    bad = b"DOS-COM FAIL: PSP layout$"
    code[first_fail + 1] = (failure - (first_fail + 2)) & 0xff
    code[second_fail + 1] = (failure - (second_fail + 2)) & 0xff
    code[success + 1:success + 3] = message_offset.to_bytes(2, "little")
    code[failure + 1:failure + 3] = (message_offset + len(ok) + 1).to_bytes(2, "little")
    return bytes(code) + ok + b"\0" + bad + b"$"


def dos_memory_test() -> bytes:
    # Allocate 64 paragraphs with AH=48h, then free them with AH=49h.
    # Branch offsets are relative to the byte following the short Jcc.
    # DOS gives COM programs all conventional memory. Release the excess
    # while retaining the 64-KiB segment containing the initial stack.
    shrink = b"\xBB\x00\x10\xB4\x4A\xCD\x21"
    code = bytearray(shrink)
    code += b"\xBB\x40\x00"       # mov bx,0040h
    code += b"\xB4\x48\xCD\x21" # mov ah,48h / int 21h
    jnc_patch = len(code); code += b"\x72\x00" # jc fail
    code += b"\x8E\xC0"           # mov es,ax
    code += b"\xB4\x49\xCD\x21" # mov ah,49h / int 21h
    jc_patch = len(code); code += b"\x72\x00"
    code += b"\xBA\x00\x00\xB4\x09\xCD\x21"
    code += b"\xB8\x00\x4C\xCD\x21"
    fail_offset = len(code)
    code += b"\xBA\x00\x00\xB4\x09\xCD\x21\xB8\x01\x4C\xCD\x21"
    message_offset = 0x100 + len(code)
    ok = b"DOS-MEM PASS: INT 21h AH=48/49$"
    bad = b"DOS-MEM FAIL: INT 21h allocation/free$"
    ok_offset = message_offset
    bad_offset = message_offset + len(ok) + 1
    # Patch both branch displacements.
    code[jnc_patch + 1] = (fail_offset - (jnc_patch + 2)) & 0xff
    code[jc_patch + 1] = (fail_offset - (jc_patch + 2)) & 0xff
    # First message address is in the success-path MOV DX; second is failure.
    first_dx = len(shrink) + len(b"\xBB\x40\x00\xB4\x48\xCD\x21\x72\x00\x8E\xC0\xB4\x49\xCD\x21\x72\x00")
    code[first_dx + 1:first_dx + 3] = ok_offset.to_bytes(2, "little")
    code[fail_offset + 1:fail_offset + 3] = bad_offset.to_bytes(2, "little")
    return bytes(code) + ok + b"\0" + bad + b"$"


def dpmi_info_test() -> bytes:
    # INT 2Fh AX=1687h is the standard DPMI host discovery call. Validate
    # the 32-bit capability, 386 minimum, DPMI 1.0 version, private data size,
    # and the synthetic real-mode entry point returned by this host.
    code = bytearray(b"\xB8\x87\x16\xCD\x2F")
    branches: list[int] = []

    def compare_ax_immediate8(opcode: bytes, immediate: int) -> None:
        code.extend(opcode)
        code.append(immediate & 0xff)
        branches.append(len(code))
        code.extend(b"\x75\x00")  # jne fail

    def compare_register16_immediate16(opcode: bytes, immediate: int) -> None:
        code.extend(opcode)
        code.extend(int(immediate).to_bytes(2, "little"))
        branches.append(len(code))
        code.extend(b"\x75\x00")

    code += b"\x09\xC0"  # or ax,ax: AX must be zero
    branches.append(len(code)); code.extend(b"\x75\x00")
    compare_ax_immediate8(b"\x83\xFB", 1)  # BX: 32-bit clients supported
    compare_ax_immediate8(b"\x83\xF9", 3)  # CX: 80386
    compare_register16_immediate16(b"\x81\xFA", 0x0100)  # DX: DPMI 1.0
    compare_ax_immediate8(b"\x83\xFE", 1)  # SI: private data paragraphs
    compare_register16_immediate16(b"\x81\xFF", 0x0100)  # DI: entry offset
    code += b"\x06\x58\x3D\x00\xF0"  # push es; pop ax; cmp ax,F000h
    branches.append(len(code)); code.extend(b"\x75\x00")

    success = len(code)
    code += b"\xBA\x00\x00\xB4\x09\xCD\x21"
    code += b"\xB8\x00\x4C\xCD\x21"
    failure = len(code)
    code += b"\xBA\x00\x00\xB4\x09\xCD\x21"
    code += b"\xB8\x01\x4C\xCD\x21"
    message_offset = 0x100 + len(code)
    ok = b"DOS-DPMI PASS: 386/32-bit DPMI 1.0 host$"
    bad = b"DOS-DPMI FAIL: host response fields$"

    for patch in branches:
        code[patch + 1] = (failure - (patch + 2)) & 0xff
    success_dx = success + 1
    failure_dx = failure + 1
    code[success_dx:success_dx + 2] = message_offset.to_bytes(2, "little")
    code[failure_dx:failure_dx + 2] = (message_offset + len(ok) + 1).to_bytes(2, "little")
    return bytes(code) + ok + b"\0" + bad + b"$"


def mz_smoke() -> bytes:
    # A tiny MZ EXE. CS:IP points at the body; DS is initialized from CS.
    body = bytearray(b"\x0E\x1F\xBA\x00\x00\xB4\x09\xCD\x21\xB8\x00\x4C\xCD\x21")
    message = b"DOS-MZ PASS: MZ load/relocation/exit$"
    body[3:5] = len(body).to_bytes(2, "little")
    image = bytearray(32) + body + message
    if len(image) > 512:
        raise ValueError("MZ smoke image unexpectedly exceeds one page")
    image.extend(b"\0" * (512 - len(image)))
    # MZ header, 32 bytes (two paragraphs).
    header = bytearray(32)
    header[0:2] = b"MZ"
    header[2:4] = (0).to_bytes(2, "little")  # bytes in last page (512)
    header[4:6] = (1).to_bytes(2, "little")  # pages
    header[6:8] = (0).to_bytes(2, "little")  # relocations
    header[8:10] = (2).to_bytes(2, "little") # header paragraphs
    header[10:12] = (0x20).to_bytes(2, "little") # min extra paragraphs
    header[12:14] = (0xffff).to_bytes(2, "little") # max extra paragraphs
    header[14:16] = (0).to_bytes(2, "little") # SS
    header[16:18] = (0x03de).to_bytes(2, "little") # below image + guaranteed minalloc
    header[18:20] = (0).to_bytes(2, "little") # checksum
    header[20:22] = (0).to_bytes(2, "little") # IP
    header[22:24] = (0).to_bytes(2, "little") # CS
    header[24:26] = (0x1c).to_bytes(2, "little") # relocation table
    header[26:28] = (0).to_bytes(2, "little") # overlay
    return bytes(header) + bytes(image[32:])


def build(output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    files = {
        "COMSMOKE.COM": com_psp_smoke(),
        "MEMSMOKE.COM": dos_memory_test(),
        "DPMIINFO.COM": dpmi_info_test(),
        "MZSMOKE.EXE": mz_smoke(),
    }
    for name, data in files.items():
        (output / name).write_bytes(data)
    (output / ".ready").write_text("DOS compatibility probes v1\n", encoding="ascii")

    # DOS command prompt batch script is intended to be invoked from C:\\.
    (output / "DOSCHECK.BAT").write_text(
        "@ECHO OFF\r\n"
        "ECHO Svindows DOS compatibility smoke tests\r\n"
        "C:\\TESTS\\COMSMOKE.COM\r\n"
        "C:\\TESTS\\MEMSMOKE.COM\r\n"
        "C:\\TESTS\\DPMIINFO.COM\r\n"
        "C:\\TESTS\\MZSMOKE.EXE\r\n",
        encoding="ascii",
    )


def verify(output: Path) -> None:
    expected = {"COMSMOKE.COM", "MEMSMOKE.COM", "DPMIINFO.COM", "MZSMOKE.EXE"}
    missing = expected - {p.name for p in output.iterdir()}
    if missing:
        raise RuntimeError("missing test binaries: " + ", ".join(sorted(missing)))
    for name in expected:
        data = (output / name).read_bytes()
        if name.endswith(".COM") and not (1 <= len(data) <= 0xff00):
            raise RuntimeError(f"invalid COM test size: {name}")
        if name == "MZSMOKE.EXE":
            if len(data) != 512 or data[:2] != b"MZ" or int.from_bytes(data[4:6], "little") != 1:
                raise RuntimeError("invalid generated MZ test header")
            header_paragraphs = int.from_bytes(data[8:10], "little")
            min_extra = int.from_bytes(data[10:12], "little")
            stack_pointer = int.from_bytes(data[16:18], "little")
            loaded_bytes = len(data) - header_paragraphs * 16
            guaranteed_bytes = loaded_bytes + min_extra * 16
            if stack_pointer > guaranteed_bytes:
                raise RuntimeError("MZ test stack exceeds its guaranteed allocation")
    print("DOS test images validated:", ", ".join(sorted(expected)))


if __name__ == "__main__":
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        raise SystemExit("usage: BuildDosTests.py <output-dir> [--verify]")
    target = Path(sys.argv[1])
    if len(sys.argv) == 3 and sys.argv[2] == "--verify":
        verify(target)
    else:
        build(target)
        verify(target)
