#!/usr/bin/env python3
"""Check the DOS x87 bridge, 16/32-bit FPU save formats, and a real WIN100 opcode."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "kernel/dos/Cpu.c").read_text(encoding="utf-8")

required = (
    "static void x87_apply_control(DOS_VM *vm)",
    "static void x87_env16_to_host28",
    "static void x87_env28_to_guest16",
    "static void x87_state16_to_host108",
    "static void x87_state108_to_guest94",
    "case 0xd8:",
    "case 0xd9:",
    "case 0xda:",
    "case 0xdb:",
    "case 0xdc:",
    "case 0xdd:",
    "case 0xde:",
    "case 0xdf:",
    'X87_REG_CASE("fdiv", 6)',
    'if (m.reg == 3 && m.rm == 1)',
    '"fcompp"',
    'if (m.reg == 1) { /* FISTTP m64int (truncate, independent of RC). */',
    '"fnstenv %0"',
    '"fldenv %0"',
    '"fnsave %0"',
    '"frstor %0"',
    'operand32, instruction_cs, instruction_ip',
    "vm->FpuInitialized",
    "vm->FpuControl = word(guest_state)",
)
missing = [item for item in required if item not in source]
if missing:
    raise SystemExit(f"FAIL x87 implementation: missing {missing}")

start = source.index("static void x87_env16_to_host28")
end = source.index("static int x87_real", start)
conversion_helpers = source[start:end]
prelude = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef uint8_t U8;
typedef uint32_t U32;
typedef uint64_t U64;
'''
checks = r'''
int main(void) {
  U8 guest_env[14] = {
    0x7f, 0x03, 0x34, 0x12, 0xcd, 0xab, 0x78,
    0x56, 0xbc, 0x9a, 0xf0, 0xde, 0x57, 0x13
  };
  U8 host_env[28];
  U8 roundtrip_env[14];
  x87_env16_to_host28(guest_env, host_env);
  assert(host_env[0] == 0x7f && host_env[1] == 0x03);
  assert(host_env[4] == 0x34 && host_env[5] == 0x12);
  assert(host_env[8] == 0xcd && host_env[9] == 0xab);
  assert(host_env[12] == 0x78 && host_env[13] == 0x56);
  assert(host_env[16] == 0xbc && host_env[17] == 0x9a);
  assert(host_env[20] == 0xf0 && host_env[21] == 0xde);
  assert(host_env[24] == 0x57 && host_env[25] == 0x13);
  x87_env28_to_guest16(host_env, roundtrip_env);
  assert(memcmp(guest_env, roundtrip_env, sizeof(guest_env)) == 0);

  U8 guest_state[94];
  U8 host_state[108];
  U8 roundtrip_state[94];
  for (unsigned i = 0; i < sizeof(guest_state); ++i)
    guest_state[i] = (U8)(i * 17u + 3u);
  x87_state16_to_host108(guest_state, host_state);
  assert(memcmp(host_state + 28, guest_state + 14, 80) == 0);
  x87_state108_to_guest94(host_state, roundtrip_state);
  assert(memcmp(guest_state, roundtrip_state, sizeof(guest_state)) == 0);
  return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="sv-dos-x87-") as directory:
    cfile = Path(directory) / "x87_formats.c"
    exe = Path(directory) / "x87_formats"
    cfile.write_text(prelude + conversion_helpers + checks)
    subprocess.run(
        [os.environ.get("HOSTCC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
         str(cfile), "-o", str(exe)],
        check=True,
    )
    subprocess.run([str(exe)], check=True)

# Confirm the exact instruction reported by the user's trace is in Windows 1.01.
win_bin = ROOT / "user/d/windows1/WIN100.BIN"
data = win_bin.read_bytes()
if data[0x5A00:0x5A03] != bytes.fromhex("d8 f6 c3"):
    raise SystemExit("FAIL WIN100.BIN: expected x87 FDIV ST(0),ST(6); RET at file offset 0x5a00")

# Compile the actual kernel interpreter with its production x86-64/no-SIMD
# flags; this catches assembler-template mistakes in the x87 translation path.
with tempfile.TemporaryDirectory(prefix="sv-dos-x87-kernel-") as directory:
    obj = Path(directory) / "Cpu.o"
    subprocess.run(
        [os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-ffreestanding",
         "-fno-stack-protector", "-fno-stack-clash-protection", "-fno-pic",
         "-fno-pie", "-no-pie", "-mno-red-zone", "-mcmodel=kernel",
         "-mgeneral-regs-only", "-fno-asynchronous-unwind-tables", "-Wall",
         "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-multichar",
         "-O2", "-Ikernel/include", "-c", "kernel/dos/Cpu.c", "-o", str(obj)],
        cwd=ROOT,
        check=True,
    )
    if bytes.fromhex("d8 f6") not in obj.read_bytes():
        raise SystemExit("FAIL Cpu.o: x87 D8 F6 translation was not emitted")

# Execute the exact FDIV ST(0),ST(6) bytes from the Windows trace on the host x87.
# Six stack entries are arranged beneath ST(0), then D8 F6 must compute 42 / 6.
host_fpu = r'''
#include <assert.h>
int main(void) {
  double denominator = 6.0, numerator = 42.0, result = 0.0;
  __asm__ volatile(
    "fninit\n\t"
    "fldl %[den]\n\t"
    "fld1\n\t" "fld1\n\t" "fld1\n\t" "fld1\n\t" "fld1\n\t"
    "fldl %[num]\n\t"
    ".byte 0xd8, 0xf6\n\t"
    "fstpl %[out]\n\t"
    "fninit\n\t"
    : [out] "=m"(result) : [den] "m"(denominator), [num] "m"(numerator) : "memory");
  assert(result > 6.999999 && result < 7.000001);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="sv-dos-x87-op-") as directory:
    cfile = Path(directory) / "x87_op.c"
    exe = Path(directory) / "x87_op"
    cfile.write_text(host_fpu)
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

print("PASS DOS x87 bridge, environment conversion and exact WIN100 FDIV operation")
