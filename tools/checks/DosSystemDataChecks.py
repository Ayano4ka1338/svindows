#!/usr/bin/env python3
"""Runtime checks for DOS 3.x List of Lists, SFT, CDS, SDA and MCB data."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "kernel/dos/DosSystemData.c").read_text(encoding="utf-8")
interrupt = (ROOT / "kernel/dos/Interrupt.c").read_text(encoding="utf-8")
for fragment in (
    "case 0x34: /* Get address of DOS InDOS flag. */",
    "case 0x52: /* Get DOS List of Lists (AL is not a subfunction). */",
    "case 0x5d:",
    "r[SI] = DOS_SDA_OFFSET",
    "r[CX] = DOS_SDA_SIZE",
):
    if fragment not in interrupt:
        raise SystemExit(f"FAIL Interrupt.c: missing {fragment!r}")
for fragment in (
    "DOS_SFT_ENTRY_SIZE",
    "DOS_LISTS_OFFSET + 0x04u",
    "DOS_SDA_OFFSET",
    "DOS_CDS_OFFSET",
    "vm->InDosCount",
    "dos_update_mcb_chain(vm)",
):
    if fragment not in source:
        raise SystemExit(f"FAIL DosSystemData.c: missing {fragment!r}")

harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
void *calloc(size_t count, size_t size);
void free(void *pointer);
#include "Internal.h"

static U16 rd16(const U8 *p) { return (U16)p[0] | ((U16)p[1] << 8); }
static U32 linear(U16 segment, U16 offset) {
  return (((U32)segment << 4) + offset) & 0xfffffu;
}

int main(void) {
  /* Windows 1.x checks this exact DOS 3.x AH=52h return offset. */
  assert(DOS_LISTS_OFFSET == 0x0022u);
  assert(DOS_SDA_OFFSET == DOS_LISTS_OFFSET + 0x202u - 0x0cu);
  assert(DOS_SDA_OFFSET + 1u - 0x01a8u == 0x0071u);
  DOS_VM *vm = calloc(1, sizeof(*vm));
  assert(vm);
  vm->Memory = calloc(1, DOS_REAL_MEMORY_SIZE);
  assert(vm->Memory);
  vm->DtaSegment = PSP;
  vm->DtaOffset = 0x80;
  vm->InitialParagraphs = 0x8000;
  vm->CurrentPsp = PSP;
  vm->CtrlBreak = 1;
  vm->LastChildExitCode = 0x2a;

  DosInitializeSystemData(vm, "C:\\WINDOWS1");
  U8 *base = vm->Memory + ((U32)DOS_SYSTEM_DATA_SEGMENT << 4);

  assert(DOS_LISTS_OFFSET == 0x22u);
  assert(rd16(base + DOS_LISTS_OFFSET + 0x00) == DOS_DPB_OFFSET);
  assert(rd16(base + DOS_LISTS_OFFSET + 0x04) == DOS_SFT_OFFSET);
  assert(rd16(base + DOS_LISTS_OFFSET + 0x06) == DOS_SYSTEM_DATA_SEGMENT);
  assert(rd16(base + DOS_SFT_OFFSET + 0x00) == 0xffff);
  assert(rd16(base + DOS_SFT_OFFSET + 0x02) == 0xffff);
  assert(rd16(base + DOS_SFT_OFFSET + 0x04) == DOS_SFT_ENTRY_COUNT);

  /* DTA is mirrored in ListOfLists+202h and SDA+0Ch. */
  assert(rd16(base + DOS_SDA_OFFSET + 0x0c) == 0x80);
  assert(rd16(base + DOS_SDA_OFFSET + 0x0e) == PSP);
  assert(rd16(base + DOS_LISTS_OFFSET + 0x202) == 0x80);
  assert(rd16(base + DOS_LISTS_OFFSET + 0x204) == PSP);
  assert(rd16(base + DOS_SDA_OFFSET + 0x10) == PSP);
  assert(rd16(base + DOS_SDA_OFFSET + 0x14) == 0x2a);
  assert(base[DOS_SDA_OFFSET + 1] == 0);
  assert(base[DOS_SDA_OFFSET + 0x17] == 0xff);
  assert(base[DOS_SDA_OFFSET + 1 - 0x1a8] == 0xff);

  /* MCB chain begins with the DOS system block, then the PSP and free block. */
  assert(rd16(base + DOS_LISTS_OFFSET - 2) == 0x05ff);
  U8 *system_mcb = vm->Memory + linear(0x05ff, 0);
  assert(system_mcb[0] == 'M');
  assert(rd16(system_mcb + 1) == 8);
  assert(rd16(system_mcb + 3) == 0x09ff);
  U8 *program_mcb = vm->Memory + linear(PSP - 1, 0);
  assert(program_mcb[0] == 'M');
  assert(rd16(program_mcb + 1) == PSP);
  assert(rd16(program_mcb + 3) == 0x8000);
  U16 free_mcb_segment = (U16)(PSP - 1 + vm->InitialParagraphs + 1);
  U8 *free_mcb = vm->Memory + linear(free_mcb_segment, 0);
  assert(free_mcb[0] == 'Z');
  assert(rd16(free_mcb + 1) == 0);

  /* Allocated DOS blocks appear as real MCB nodes, with header paragraphs
   * excluded from both neighbouring free ranges. */
  vm->Blocks[0] = (DOS_BLOCK){0x9100, 0x20, TRUE, PSP};
  DosUpdateMemoryControlBlocks(vm);
  U8 *leading_free_mcb = vm->Memory + linear(0x9000, 0);
  U8 *allocated_mcb = vm->Memory + linear(0x90ff, 0);
  U8 *trailing_free_mcb = vm->Memory + linear(0x9120, 0);
  assert(leading_free_mcb[0] == 'M');
  assert(rd16(leading_free_mcb + 3) == 0x00fe);
  assert(allocated_mcb[0] == 'M');
  assert(rd16(allocated_mcb + 1) == PSP);
  assert(rd16(allocated_mcb + 3) == 0x20);
  vm->CurrentPsp = 0x2345;
  DosUpdateMemoryControlBlocks(vm);
  assert(rd16(vm->Memory + linear(0x90ff, 1)) == PSP);
  assert(trailing_free_mcb[0] == 'Z');
  assert(rd16(trailing_free_mcb + 1) == 0);
  vm->Blocks[0].Used = FALSE;
  DosUpdateMemoryControlBlocks(vm);
  free_mcb = vm->Memory + linear(0x9000, 0);
  assert(free_mcb[0] == 'Z');
  assert(rd16(free_mcb + 3) == 0x0fff);

  /* Device chain and standard SFT handles. */
  U16 nul = (U16)(DOS_LISTS_OFFSET + 0x22);
  assert(rd16(base + nul) == DOS_DEVICE_CLOCK_OFFSET);
  assert(rd16(base + nul + 2) == DOS_SYSTEM_DATA_SEGMENT);
  assert(rd16(base + DOS_DEVICE_PRN_OFFSET) == 0xffff);
  assert(rd16(base + DOS_DEVICE_PRN_OFFSET + 2) == 0xffff);
  assert(!memcmp(base + DOS_SFT_OFFSET + 6 + 0x20, "CON        ", 11));
  assert(!memcmp(base + DOS_SFT_OFFSET + 6 + DOS_SFT_ENTRY_SIZE + 0x20,
                 "CON        ", 11));

  /* C: CDS and current-DTA updates remain coherent. */
  U16 c_cds = (U16)(DOS_CDS_OFFSET + 2 * DOS_CDS_ENTRY_SIZE);
  assert(!strcmp((const char *)(base + c_cds), "C:\\WINDOWS1"));
  assert(rd16(base + c_cds + 0x43) == 0x4000);
  vm->DtaSegment = 0x1234;
  vm->DtaOffset = 0x5678;
  vm->CtrlBreak = 0;
  vm->InDosCount = 1;
  vm->CurrentPsp = 0x2345;
  DosSyncSystemData(vm, "C:\\WINDOWS1\\SYSTEM");
  assert(rd16(base + DOS_SDA_OFFSET + 0x10) == 0x2345);
  assert(rd16(base + DOS_SDA_OFFSET + 0x1a) == 0x2345);
  assert(rd16(base + DOS_SDA_OFFSET + 0x0c) == 0x5678);
  assert(rd16(base + DOS_SDA_OFFSET + 0x0e) == 0x1234);
  assert(rd16(base + DOS_LISTS_OFFSET + 0x202) == 0x5678);
  assert(rd16(base + DOS_LISTS_OFFSET + 0x204) == 0x1234);
  assert(base[DOS_SDA_OFFSET + 1] == 1);
  assert(base[DOS_SDA_OFFSET + 1 - 0x1a8] == 0);
  assert(!strcmp((const char *)(base + c_cds), "C:\\WINDOWS1\\SYSTEM"));

  /* SFT tracks live DOS file handles and clears entries on close. */
  DOS_FILE file = {0};
  file.Size = 0x12345;
  file.Position = 0x2345;
  file.Readable = TRUE;
  file.Writable = FALSE;
  strcpy(file.Path, "C:\\WINDOWS1\\WIN.INI");
  vm->Files[5] = &file;
  DosUpdateSftEntry(vm, 5);
  U16 file_sft = (U16)(DOS_SFT_OFFSET + 6 + 5 * DOS_SFT_ENTRY_SIZE);
  assert(rd16(base + file_sft) == 1);
  assert(rd16(base + file_sft + 2) == 0);
  assert(!memcmp(base + file_sft + 0x20, "WIN     INI", 11));
  assert(base[file_sft + 0x11] == 0x45);
  assert(base[file_sft + 0x12] == 0x23);
  assert(base[file_sft + 0x13] == 0x01);
  assert(rd16(base + file_sft + 0x15) == 0x2345);
  vm->Files[5] = NULL;
  DosUpdateSftEntry(vm, 5);
  assert(rd16(base + file_sft) == 0);

  free(vm->Memory);
  free(vm);
  return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="sv-dos-sysdata-") as directory:
    cfile = Path(directory) / "sysdata.c"
    exe = Path(directory) / "sysdata"
    cfile.write_text(harness, encoding="utf-8")
    subprocess.run([
        os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-O2",
        "-Wall", "-Wextra", "-Werror", "-Ikernel/include",
        "-Ikernel/dos", str(cfile), "kernel/dos/DosSystemData.c",
        "-o", str(exe)
    ], cwd=ROOT, check=True)
    subprocess.run([str(exe)], check=True)
print("PASS DOS INT 21h/34h, 52h, 5D06h and List-of-Lists/SFT/SDA/CDS/MCB data")
