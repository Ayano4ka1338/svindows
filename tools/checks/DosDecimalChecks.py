#!/usr/bin/env python3
"""Exercise the production 8086 DAA/DAS/AAA/AAS arithmetic helper."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "kernel/dos/Cpu.c").read_text()
start = source.index("static U16 decimal_adjust(")
end = source.index("\nstatic U16 alu(", start)
implementation = source[start:end]
prelude = r"""
#include <stdint.h>
#include <assert.h>
typedef uint8_t U8;
typedef uint16_t U16;
#define CF 1u
#define AF 0x10u
#define ZF 0x40u
#define SF 0x80u
static U8 parity(U8 v) {
  v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
  return !(v & 1u);
}
"""
checks = r"""
int main(void) {
  U16 f, ax;

  /* DAA after 0x09 + 1: 0x0a becomes packed-BCD 0x10. */
  f = 2; ax = decimal_adjust(0x27, 0x000a, &f);
  assert(ax == 0x0010 && (f & AF) && !(f & CF));
  assert(!(f & ZF) && !(f & SF));

  /* DAA carries from both decimal nibbles: 0x9a -> 0x00, with CF/AF. */
  f = 2; ax = decimal_adjust(0x27, 0x009a, &f);
  assert(ax == 0x0000 && (f & (CF | AF | ZF)) == (CF | AF | ZF));
  assert(f & 4u); /* zero has even parity */

  /* A prior auxiliary carry must trigger the low-nibble correction. */
  f = AF | 2; ax = decimal_adjust(0x27, 0x0003, &f);
  assert(ax == 0x0009 && (f & AF));

  /* DAS mirrors DAA subtraction and keeps the original carry decision. */
  f = 2; ax = decimal_adjust(0x2f, 0x000b, &f);
  assert(ax == 0x0005 && (f & AF) && !(f & CF));
  f = CF | 2; ax = decimal_adjust(0x2f, 0x0000, &f);
  assert(ax == 0x00a0 && (f & CF));

  /* AAA/AAS change AX and only define AF/CF; other flags are preserved. */
  f = ZF | SF | 4u | 2u; ax = decimal_adjust(0x37, 0x000a, &f);
  assert(ax == 0x0100 && (f & (CF | AF)) == (CF | AF));
  assert((f & (ZF | SF | 4u)) == (ZF | SF | 4u));
  f = ZF | 2u; ax = decimal_adjust(0x3f, 0x000b, &f);
  assert(ax == 0xff05 && (f & (CF | AF)) == (CF | AF));
  assert(f & ZF);

  /* No-adjust ASCII correction clears stale CF/AF and masks AL's high nibble. */
  f = CF | 2u; ax = decimal_adjust(0x37, 0x1204, &f);
  assert(ax == 0x1204 && !(f & (CF | AF)));
  return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="sv-dos-decimal-") as directory:
    cfile = Path(directory) / "decimal.c"
    exe = Path(directory) / "decimal"
    cfile.write_text(prelude + implementation + checks)
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", str(cfile), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS DOS 8086 DAA/DAS/AAA/AAS, status flags, and boundary cases")
