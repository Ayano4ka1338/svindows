#!/usr/bin/env python3
"""Check DOS INT 21h/AH=0Ch and the console input-queue flush primitive."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
console = (ROOT / "kernel/io/Console.c").read_text(encoding="utf-8")
interrupt = (ROOT / "kernel/dos/Interrupt.c").read_text(encoding="utf-8")
header = (ROOT / "kernel/include/svindows/Console.h").read_text(encoding="utf-8")
start = console.index("void ConFlushInput(void)")
end = console.index("static char con_pop_input", start)
implementation = console[start:end]

prelude = r'''
#include <stdint.h>
#include <assert.h>
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef int BOOLEAN;
typedef struct { UINT8_PLACEHOLDER; } unused_type;
static struct { UINT32 InputHead, InputTail; } g_console;
static UINT64 KiIrqSave(void) { return 0; }
static void KiIrqRestore(UINT64 flags) { (void)flags; }
'''.replace('typedef struct { UINT8_PLACEHOLDER; } unused_type;\n', '')
checks = r'''
int main(void) {
  g_console.InputHead = 7;
  g_console.InputTail = 2;
  ConFlushInput();
  assert(g_console.InputHead == 7);
  assert(g_console.InputTail == 7);
  g_console.InputHead = 19;
  g_console.InputTail = 19;
  ConFlushInput();
  assert(g_console.InputHead == 19 && g_console.InputTail == 19);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="sv-dos-console-") as directory:
    cfile = Path(directory) / "console.c"
    exe = Path(directory) / "console"
    cfile.write_text(prelude + implementation + checks)
    subprocess.run([
        os.environ.get("HOSTCC", "clang"), "-std=c11", "-Wall", "-Wextra",
        "-Werror", str(cfile), "-o", str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)

assert "void ConFlushInput(void);" in header
case_start = interrupt.index("  case 0x0c: {")
case_end = interrupt.index("\n  case 0x0e:", case_start)
flush_case = interrupt[case_start:case_end]
assert flush_case.index("ConFlushInput();") < flush_case.index("U8 function = get8(0);")
for subfunction in ("0x01u", "0x06u", "0x07u", "0x08u", "0x0au"):
    assert subfunction in flush_case, subfunction
assert "if (function == 0x01u || function == 0x07u || function == 0x08u)" in flush_case
# AL=00 and other unsupported selectors intentionally flush and return;
# they must not fall through to DOS AH=00 process termination.
assert "running = 0" not in flush_case
assert "exit_code = 0" not in flush_case
print("PASS DOS INT 21h/AH=0Ch flush semantics and supported input selectors")
