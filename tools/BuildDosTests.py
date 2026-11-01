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


def fcb_parse_test() -> bytes:
    # INT 21h/AH=29h parses a DOS 8.3 filespec into an unopened FCB.
    # AL=01h also verifies that leading separators are skipped and SI is
    # returned at the first byte after the filespec.
    code = bytearray(b"\x0e\x1f\x0e\x07")  # DS=ES=CS
    code += b"\xbe\x00\x00"  # mov si, filespec (patched below)
    si_patch = 5
    code += b"\xbf\x00\x00"  # mov di, FCB (patched below)
    di_patch = 8
    code += b"\xb8\x01\x29\xcd\x21"  # AX=2901h / INT 21h

    failure_branches: list[int] = []

    def jne_fail() -> None:
        failure_branches.append(len(code) + 1)
        code.extend(b"\x75\x00")

    code += b"\x3c\x00"  # cmp al,00h: no wildcards
    jne_fail()
    code += b"\x26\x80\x3d\x03"  # ES:[DI] = drive C (3)
    jne_fail()
    expected = {1: ord('W'), 2: ord('I'), 3: ord('N'),
                4: ord(' '), 5: ord(' '), 6: ord(' '), 7: ord(' '),
                8: ord(' '), 9: ord('C'), 10: ord('O'), 11: ord('M')}
    for offset, value in expected.items():
        code += bytes((0x26, 0x80, 0x7d, offset, value))
        jne_fail()
    # SI must point at the space immediately after C:WIN.COM.
    code += b"\x81\xfe\x00\x00"  # cmp si, filespec+10 (patched below)
    si_result_patch = len(code) - 2
    jne_fail()

    success = len(code)
    code += b"\xba\x00\x00\xb4\x09\xcd\x21"  # print PASS
    success_dx_patch = success + 1
    code += b"\xb8\x00\x4c\xcd\x21"  # exit 0
    failure = len(code)
    code += b"\xba\x00\x00\xb4\x09\xcd\x21"  # print FAIL
    failure_dx_patch = failure + 1
    code += b"\xb8\x01\x4c\xcd\x21"  # exit 1

    spec = b" C:WIN.COM tail\x00"
    ok = b"DOS-FCB PASS: INT 21h AH=29h\r\n$"
    bad = b"DOS-FCB FAIL: INT 21h AH=29h\r\n$"
    spec_offset = 0x100 + len(code)
    fcb_offset = spec_offset + len(spec)
    ok_offset = fcb_offset + 37
    bad_offset = ok_offset + len(ok)

    code[si_patch:si_patch + 2] = spec_offset.to_bytes(2, "little")
    code[di_patch:di_patch + 2] = fcb_offset.to_bytes(2, "little")
    code[si_result_patch:si_result_patch + 2] = (spec_offset + 10).to_bytes(2, "little")
    code[success_dx_patch:success_dx_patch + 2] = ok_offset.to_bytes(2, "little")
    code[failure_dx_patch:failure_dx_patch + 2] = bad_offset.to_bytes(2, "little")
    for patch in failure_branches:
        code[patch] = (failure - (patch + 1)) & 0xff
    return bytes(code) + spec + (b"\x00" * 37) + ok + bad



def ff_group_test() -> bytes:
    """Exercise 386 operand-size forms of the real-mode FF/8F/C7 groups."""
    code = bytearray()
    data_refs: list[tuple[int, str]] = []
    jne_refs: list[int] = []
    near_refs: list[tuple[int, str]] = []

    def emit_abs(prefix: bytes, opcode: bytes, modrm: int, key: str,
                 suffix: bytes = b"") -> None:
        code.extend(prefix + opcode + bytes((modrm,)))
        data_refs.append((len(code), key))
        code.extend(b"\x00\x00")
        code.extend(suffix)

    def mov_dword_imm(key: str, value: int | None = None,
                      label: str | None = None) -> None:
        code.extend(b"\x66\xc7\x06")
        data_refs.append((len(code), key))
        code.extend(b"\x00\x00")
        if label is not None:
            near_refs.append((len(code), label))
            code.extend(b"\x00\x00\x00\x00")
        else:
            code.extend(int(value or 0).to_bytes(4, "little"))

    def cmp_eax(value: int) -> None:
        code.extend(b"\x66\x3d" + value.to_bytes(4, "little"))
        jne_refs.append(len(code) + 1)
        code.extend(b"\x75\x00")

    # FF /6 PUSH dword [value], followed by 8F /0 POP dword [result].
    mov_dword_imm("value", 0x12345678)
    emit_abs(b"\x66", b"\xff", 0x36, "value")
    code.extend(b"\x66\x31\xc0")  # xor eax,eax
    emit_abs(b"\x66", b"\x8f", 0x06, "result")
    code.extend(b"\x66\xa1")
    data_refs.append((len(code), "result")); code.extend(b"\x00\x00")
    cmp_eax(0x12345678)

    # FF /0 INC and /1 DEC must operate on the full dword and preserve CF.
    mov_dword_imm("value", 1)
    code.extend(b"\xf9")  # stc: INC must preserve CF
    emit_abs(b"\x66", b"\xff", 0x06, "value")
    jne_refs.append(len(code) + 1)
    code.extend(b"\x73\x00")  # jnc fail
    code.extend(b"\x66\xa1")
    data_refs.append((len(code), "value")); code.extend(b"\x00\x00")
    cmp_eax(2)
    code.extend(b"\xf9")  # stc: DEC must preserve CF
    emit_abs(b"\x66", b"\xff", 0x0e, "value")
    jne_refs.append(len(code) + 1)
    code.extend(b"\x73\x00")  # jnc fail
    code.extend(b"\x66\xa1")
    data_refs.append((len(code), "value")); code.extend(b"\x00\x00")
    cmp_eax(1)

    # FF /2 indirect CALL with a 32-bit return address, paired with 66 C3.
    mov_dword_imm("near_ptr", label="near_callee")
    emit_abs(b"\x66", b"\xff", 0x16, "near_ptr")

    # FF /3 far CALL m16:32; the callee returns with operand-size 32 RETF.
    mov_dword_imm("far_ptr", label="far_callee")
    code.extend(b"\x8c\xc8\xa3")  # mov ax,cs / mov [far_ptr+4],ax
    data_refs.append((len(code), "far_segment")); code.extend(b"\x00\x00")
    emit_abs(b"\x66", b"\xff", 0x1e, "far_ptr")

    # FF /4 near JMP must skip the fail sentinel and land at near_jump_ok.
    mov_dword_imm("near_ptr", label="near_jump_ok")
    emit_abs(b"\x66", b"\xff", 0x26, "near_ptr")
    near_fail_patch = len(code) + 1
    code.extend(b"\xe9\x00\x00")
    near_jump_ok = len(code)

    # FF /5 far JMP m16:32 must skip a second fail sentinel.
    mov_dword_imm("far_ptr", label="success")
    code.extend(b"\x8c\xc8\xa3")
    data_refs.append((len(code), "far_segment")); code.extend(b"\x00\x00")
    emit_abs(b"\x66", b"\xff", 0x2e, "far_ptr")
    far_fail_patch = len(code) + 1
    code.extend(b"\xe9\x00\x00")

    # These are the targets of the two indirect calls. 32-bit RET/RETF must
    # consume two/four-byte stack slots, not silently pop only 16 bits.
    near_callee = len(code)
    code.extend(b"\x66\xc3")
    far_callee = len(code)
    code.extend(b"\x66\xcb")

    success = len(code)
    code.extend(b"\xba\x00\x00\xb4\x09\xcd\x21")
    success_dx = success + 1
    code.extend(b"\xb8\x00\x4c\xcd\x21")
    failure = len(code)
    code.extend(b"\xba\x00\x00\xb4\x09\xcd\x21")
    failure_dx = failure + 1
    code.extend(b"\xb8\x01\x4c\xcd\x21")

    data_start = len(code)
    data_offsets = {
        "value": data_start,
        "result": data_start + 4,
        "near_ptr": data_start + 8,
        "far_ptr": data_start + 12,
        "far_segment": data_start + 16,
    }
    code.extend(b"\x00" * 18)
    message_ok = b"DOS-FF PASS: FF groups and 32-bit stack\r\n$"
    message_fail = b"DOS-FF FAIL: FF groups or stack width\r\n$"
    ok_offset = len(code)
    code.extend(message_ok)
    fail_offset = len(code)
    code.extend(message_fail)

    labels = {
        "near_callee": near_callee,
        "far_callee": far_callee,
        "near_jump_ok": near_jump_ok,
        "success": success,
    }
    for at, key in data_refs:
        absolute = 0x100 + data_offsets[key]
        code[at:at + 2] = absolute.to_bytes(2, "little")
    for at, label in near_refs:
        code[at:at + 4] = (0x100 + labels[label]).to_bytes(4, "little")
    for patch in jne_refs:
        code[patch] = (failure - (patch + 1)) & 0xff
    code[near_fail_patch:near_fail_patch + 2] = (failure - (near_fail_patch + 2)).to_bytes(2, "little", signed=True)
    code[far_fail_patch:far_fail_patch + 2] = (failure - (far_fail_patch + 2)).to_bytes(2, "little", signed=True)
    code[success_dx:success_dx + 2] = (0x100 + ok_offset).to_bytes(2, "little")
    code[failure_dx:failure_dx + 2] = (0x100 + fail_offset).to_bytes(2, "little")
    return bytes(code)

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
        "FCBPARSE.COM": fcb_parse_test(),
        "FFGROUP.COM": ff_group_test(),
        "MZSMOKE.EXE": mz_smoke(),
    }
    for name, data in files.items():
        (output / name).write_bytes(data)
    (output / ".ready").write_text("DOS compatibility probes v3 - FCB and FF group\n", encoding="ascii")

    # DOS command prompt batch script is intended to be invoked from C:\\.
    (output / "DOSCHECK.BAT").write_text(
        "@ECHO OFF\r\n"
        "ECHO Svindows DOS compatibility smoke tests\r\n"
        "C:\\TESTS\\COMSMOKE.COM\r\n"
        "C:\\TESTS\\MEMSMOKE.COM\r\n"
        "C:\\TESTS\\DPMIINFO.COM\r\n"
        "C:\\TESTS\\FCBPARSE.COM\r\n"
        "C:\\TESTS\\FFGROUP.COM\r\n"
        "C:\\TESTS\\MZSMOKE.EXE\r\n",
        encoding="ascii",
    )


def verify(output: Path) -> None:
    expected = {"COMSMOKE.COM", "MEMSMOKE.COM", "DPMIINFO.COM", "FCBPARSE.COM", "FFGROUP.COM", "MZSMOKE.EXE"}
    missing = expected - {p.name for p in output.iterdir()}
    if missing:
        raise RuntimeError("missing test binaries: " + ", ".join(sorted(missing)))
    for name in expected:
        data = (output / name).read_bytes()
        if name.endswith(".COM") and not (1 <= len(data) <= 0xff00):
            raise RuntimeError(f"invalid COM test size: {name}")
        if name == "FCBPARSE.COM" and b" C:WIN.COM tail\x00" not in data:
            raise RuntimeError("FCB parse probe is missing its filespec")
        if name == "FFGROUP.COM":
            for signature in (b"\x66\xff\x36", b"\x66\x8f\x06",
                              b"\x66\xff\x16", b"\x66\xff\x1e",
                              b"\x66\xff\x26", b"\x66\xff\x2e",
                              b"\x66\xc3", b"\x66\xcb"):
                if signature not in data:
                    raise RuntimeError(f"FF group probe missing instruction {signature.hex()}")
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
