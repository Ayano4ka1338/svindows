#!/usr/bin/env python3
"""Source-level regression checks for DOS nested PSP termination."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
interrupt = (root / "kernel/dos/Interrupt.c").read_text(encoding="utf-8")
dos = (root / "kernel/dos/Dos.c").read_text(encoding="utf-8")

helper_start = interrupt.index("static BOOLEAN dos_terminate_nested_psp(U8 code)")
helper_end = interrupt.index("void dos_int(U8 n)", helper_start)
helper = interrupt[helper_start:helper_end]
assert "rw(psp, 0x16u)" in helper, "PSP parent link is not followed"
assert "rw(psp, 0x0au)" in helper and "rw(psp, 0x0cu)" in helper, \
    "PSP INT 22h termination address is not used"
assert "rw(parent, 0x2eu)" in helper and "rw(parent, 0x30u)" in helper, \
    "saved parent SS:SP is not restored"
assert "block->OwnerPsp == psp" in helper, "PSP-owned blocks are not released"
assert "vector = 0x23u; vector <= 0x24u" in helper, \
    "INT 23h/24h vectors are not restored"
assert "vm->DtaSegment = parent" in helper and "vm->DtaOffset = 0x80u" in helper
assert "seg[CS] = terminate_segment" in helper and "ip = terminate_offset" in helper

exec_case = interrupt[interrupt.index("  case 0x4c: {"):interrupt.index("  case 0x4b:", interrupt.index("  case 0x4c: {"))]
assert "dos_terminate_nested_psp((U8)exit_code)" in exec_case, \
    "INT 21h/AH=4Ch still unconditionally terminates the DOS VM"

psp_case_start = interrupt.index("  case 0x26:\n  case 0x55:")
psp_case_end = interrupt.index("  default:", psp_case_start)
psp_case = interrupt[psp_case_start:psp_case_end]
assert "ww(child, 0x30u, 0)" in psp_case, "new PSP inherits stale EXEC stack state"
assert "if (ah == 0x55u)" in psp_case and "CurrentPsp = child" in psp_case
assert "vector = 0x22u; vector <= 0x24u" in psp_case
assert "for (unsigned vector = 0x22u; vector <= 0x24u; vector++)" in dos, \
    "initial PSP does not save DOS termination/control vectors"
assert "ConRedraw();" in dos[dos.index("void DosDestroyVm(PVOID context)"):], \
    "native console is not redrawn after DOS VM teardown"
print("PASS DOS nested PSP: AH=26h/55h creation, AH=4Ch return, vector/MCB/DTA cleanup, console redraw")
