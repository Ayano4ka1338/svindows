/* DOS 3.1-3.3 internal data structures exposed to real-mode applications. */
#include "Internal.h"

#define DOS_SYSTEM_DATA_BYTES 0x1800u
#define DOS_FIRST_SYSTEM_MCB 0x05ffu
#define DOS_SFT_HEADER_SIZE 6u

static U8 *dos_system_base(DOS_VM *vm) {
  return vm->Memory + ((U32)DOS_SYSTEM_DATA_SEGMENT << 4);
}

static void dos_put8(U8 *base, U16 offset, U8 value) {
  base[offset] = value;
}

static void dos_put16(U8 *base, U16 offset, U16 value) {
  base[offset] = (U8)value;
  base[(U16)(offset + 1u)] = (U8)(value >> 8);
}

static void dos_put32(U8 *base, U16 offset, U32 value) {
  dos_put16(base, offset, (U16)value);
  dos_put16(base, (U16)(offset + 2u), (U16)(value >> 16));
}

static void dos_put_far(U8 *base, U16 offset, U16 target_offset,
                        U16 target_segment) {
  dos_put16(base, offset, target_offset);
  dos_put16(base, (U16)(offset + 2u), target_segment);
}

static U8 dos_upper(U8 value) {
  return value >= 'a' && value <= 'z' ? (U8)(value - ('a' - 'A')) : value;
}

static void dos_write_device_header(U8 *base, U16 offset, U16 next_offset,
                                    U16 attributes, const char name[8]) {
  dos_put_far(base, offset, next_offset,
              next_offset == 0xffffu ? 0xffffu : DOS_SYSTEM_DATA_SEGMENT);
  dos_put16(base, (U16)(offset + 4u), attributes);
  dos_put16(base, (U16)(offset + 6u), 0); /* strategy entry: metadata only */
  dos_put16(base, (U16)(offset + 8u), 0); /* interrupt entry: metadata only */
  memcpy(base + offset + 0x0au, name, 8);
}

static void dos_write_sft_device(U8 *base, U16 slot, U16 device_offset,
                                 U16 attributes, const char name[11]) {
  U16 offset = (U16)(DOS_SFT_OFFSET + DOS_SFT_HEADER_SIZE +
                     slot * DOS_SFT_ENTRY_SIZE);
  dos_put16(base, offset, 1);             /* open reference count */
  dos_put16(base, (U16)(offset + 2u), 0); /* access mode */
  dos_put8(base, (U16)(offset + 4u), 0);  /* file attributes */
  dos_put16(base, (U16)(offset + 5u), attributes);
  dos_put_far(base, (U16)(offset + 7u), device_offset,
              DOS_SYSTEM_DATA_SEGMENT);
  memcpy(base + offset + 0x20u, name, 11);
  dos_put16(base, (U16)(offset + 0x31u), 0x0008u); /* DOS-owned device */
}

static void dos_write_mcb(U8 *memory, U16 segment, U8 type, U16 owner,
                          U16 paragraphs, const char name[8]) {
  U8 *mcb = memory + ((U32)segment << 4);
  mcb[0] = type;
  mcb[1] = (U8)owner;
  mcb[2] = (U8)(owner >> 8);
  mcb[3] = (U8)paragraphs;
  mcb[4] = (U8)(paragraphs >> 8);
  if (name) memcpy(mcb + 8, name, 8);
}

static void dos_update_mcb_chain(DOS_VM *vm) {
  U32 next_user = (U32)PSP - 1u;
  U32 next_free = next_user + (U32)vm->InitialParagraphs + 1u;
  U32 free_paragraphs = next_free < 0xa000u ? 0xa000u - next_free - 1u : 0;
  static const char system_name[8] = {'S','V','D','O','S',' ',' ',' '};
  static const char app_name[8] = {'D','O','S','A','P','P',' ',' '};
  dos_put16(dos_system_base(vm), (U16)(DOS_LISTS_OFFSET - 2u),
            DOS_FIRST_SYSTEM_MCB);
  dos_write_mcb(vm->Memory, DOS_FIRST_SYSTEM_MCB, 'M', 0x0008u,
                (U16)(PSP - DOS_SYSTEM_DATA_SEGMENT - 1u), system_name);
  dos_write_mcb(vm->Memory, (U16)next_user,
                free_paragraphs ? 'M' : 'Z', PSP,
                vm->InitialParagraphs, app_name);
  if (free_paragraphs)
    dos_write_mcb(vm->Memory, (U16)next_free, 'Z', 0, (U16)free_paragraphs,
                  NULL);
}

void DosSyncSystemData(DOS_VM *vm, const char *current_directory) {
  if (!vm || !vm->Memory) return;
  U8 *base = dos_system_base(vm);
  U16 sda = DOS_SDA_OFFSET;

  /* Keep the two DOS-visible DTA pointers identical:
   * SDA+0Ch == ListOfLists+202h. */
  dos_put16(base, (U16)(sda + 0x0cu), vm->DtaOffset);
  dos_put16(base, (U16)(sda + 0x0eu), vm->DtaSegment);
  dos_put16(base, (U16)(sda + 0x10u), vm->CurrentPsp);
  dos_put16(base, (U16)(sda + 0x14u), vm->LastChildExitCode);
  dos_put8(base, (U16)(sda + 0x16u), 2u); /* current drive C: */
  dos_put8(base, (U16)(sda + 0x17u), vm->CtrlBreak ? 0xffu : 0u);
  dos_put16(base, (U16)(sda + 0x1au), vm->CurrentPsp);
  dos_put8(base, (U16)(sda + 0x01u), 0); /* idle between guest INT 21h calls */

  /* Windows 1.x checks this DOS 3.x compatibility byte by subtracting
   * 01A8h from the InDOS pointer returned by AH=34h. */
  dos_put8(base, (U16)(sda + 1u - 0x01a8u),
           vm->CtrlBreak ? 0xffu : 0u);

  /* DOS 3.x CDS has five LASTDRIVE entries by default. Keep C:'s current
   * directory in sync with the process's DOS working directory. */
  U16 c_entry = (U16)(DOS_CDS_OFFSET + 2u * DOS_CDS_ENTRY_SIZE);
  char path[67] = "C:\\";
  if (current_directory && current_directory[0] &&
      current_directory[1] == ':') {
    unsigned i = 0;
    while (current_directory[i] && i < sizeof(path) - 1u) {
      path[i] = current_directory[i] == '/' ? '\\' : current_directory[i];
      i++;
    }
    path[i] = 0;
    if (i == 2u) {
      path[2] = '\\';
      path[3] = 0;
    }
  }
  memset(base + c_entry, 0, DOS_CDS_ENTRY_SIZE);
  unsigned path_length = 0;
  while (path[path_length] && path_length < sizeof(path) - 1u)
    path_length++;
  memcpy(base + c_entry, path, path_length + 1u);
  dos_put16(base, (U16)(c_entry + 0x43u), 0x4000u); /* local physical C: */
  dos_update_mcb_chain(vm);
}

void DosInitializeSystemData(DOS_VM *vm, const char *current_directory) {
  if (!vm || !vm->Memory) return;
  U8 *base = dos_system_base(vm);
  memset(base, 0, DOS_SYSTEM_DATA_BYTES);

  /* The DOS data segment identifies the INT 21h/5D06 area as DOS 3.x. */
  dos_put8(base, 0x0004u, 0x00u);

  /* DOS 3.1-3.3 List of Lists. */
  dos_put_far(base, DOS_LISTS_OFFSET + 0x00u, DOS_DPB_OFFSET,
              DOS_SYSTEM_DATA_SEGMENT);
  dos_put_far(base, DOS_LISTS_OFFSET + 0x04u, DOS_SFT_OFFSET,
              DOS_SYSTEM_DATA_SEGMENT);
  dos_put_far(base, DOS_LISTS_OFFSET + 0x08u, DOS_DEVICE_CLOCK_OFFSET,
              DOS_SYSTEM_DATA_SEGMENT);
  dos_put_far(base, DOS_LISTS_OFFSET + 0x0cu, DOS_DEVICE_CON_OFFSET,
              DOS_SYSTEM_DATA_SEGMENT);
  dos_put16(base, DOS_LISTS_OFFSET + 0x10u, 512u);
  dos_put_far(base, DOS_LISTS_OFFSET + 0x12u, 0xffffu, 0xffffu);
  dos_put_far(base, DOS_LISTS_OFFSET + 0x16u, DOS_CDS_OFFSET,
              DOS_SYSTEM_DATA_SEGMENT);
  dos_put_far(base, DOS_LISTS_OFFSET + 0x1au, 0xffffu, 0xffffu);
  dos_put16(base, DOS_LISTS_OFFSET + 0x1eu, 0);
  dos_put8(base, DOS_LISTS_OFFSET + 0x20u, 1u); /* mounted C: volume */
  dos_put8(base, DOS_LISTS_OFFSET + 0x21u, 5u); /* DOS default LASTDRIVE */
  dos_put8(base, DOS_LISTS_OFFSET + 0x34u, 0);

  /* NUL is embedded at ListOfLists+22h; the remaining device headers are
   * placed away from the DOS 3.x compatibility byte at offset 014Fh. */
  static const char nul_name[8] = {'N','U','L',' ',' ',' ',' ',' '};
  static const char clock_name[8] = {'C','L','O','C','K','$',' ',' '};
  static const char con_name[8] = {'C','O','N',' ',' ',' ',' ',' '};
  static const char aux_name[8] = {'A','U','X',' ',' ',' ',' ',' '};
  static const char prn_name[8] = {'P','R','N',' ',' ',' ',' ',' '};
  U16 nul = (U16)(DOS_LISTS_OFFSET + 0x22u);
  dos_write_device_header(base, nul, DOS_DEVICE_CLOCK_OFFSET, 0x8004u,
                          nul_name);
  dos_write_device_header(base, DOS_DEVICE_CLOCK_OFFSET,
                          DOS_DEVICE_CON_OFFSET, 0x8008u, clock_name);
  dos_write_device_header(base, DOS_DEVICE_CON_OFFSET,
                          DOS_DEVICE_AUX_OFFSET, 0x8013u, con_name);
  dos_write_device_header(base, DOS_DEVICE_AUX_OFFSET,
                          DOS_DEVICE_PRN_OFFSET, 0x8000u, aux_name);
  dos_write_device_header(base, DOS_DEVICE_PRN_OFFSET, 0xffffu, 0x8000u,
                          prn_name);

  /* A single DOS 3.x DPB for drive C:. This is a compatibility description;
   * actual host filesystem requests continue through the FAT VFS backend. */
  dos_put8(base, DOS_DPB_OFFSET + 0x00u, 2u);
  dos_put8(base, DOS_DPB_OFFSET + 0x01u, 0u);
  dos_put16(base, DOS_DPB_OFFSET + 0x02u, 512u);
  dos_put8(base, DOS_DPB_OFFSET + 0x04u, 7u);
  dos_put8(base, DOS_DPB_OFFSET + 0x05u, 3u);
  dos_put16(base, DOS_DPB_OFFSET + 0x06u, 1u);
  dos_put8(base, DOS_DPB_OFFSET + 0x08u, 2u);
  dos_put16(base, DOS_DPB_OFFSET + 0x09u, 512u);
  dos_put16(base, DOS_DPB_OFFSET + 0x0bu, 2048u);
  dos_put16(base, DOS_DPB_OFFSET + 0x0du, 0x7fffu);
  dos_put8(base, DOS_DPB_OFFSET + 0x0fu, 128u);
  dos_put16(base, DOS_DPB_OFFSET + 0x10u, 257u);
  dos_put_far(base, DOS_DPB_OFFSET + 0x12u, 0xffffu, 0xffffu);
  dos_put8(base, DOS_DPB_OFFSET + 0x16u, 0xf8u);
  dos_put8(base, DOS_DPB_OFFSET + 0x17u, 0);
  dos_put_far(base, DOS_DPB_OFFSET + 0x18u, 0xffffu, 0xffffu);
  dos_put16(base, DOS_DPB_OFFSET + 0x1cu, 0);
  dos_put16(base, DOS_DPB_OFFSET + 0x1eu, 0xffffu);

  /* DOS 3.1-3.3 SFT chain: next pointer, entry count, 20 entries of 35h bytes. */
  dos_put_far(base, DOS_SFT_OFFSET, 0xffffu, 0xffffu);
  dos_put16(base, (U16)(DOS_SFT_OFFSET + 4u), DOS_SFT_ENTRY_COUNT);
  static const char con_fcb[11] = {'C','O','N',' ',' ',' ',' ',' ',' ',' ',' '};
  static const char aux_fcb[11] = {'A','U','X',' ',' ',' ',' ',' ',' ',' ',' '};
  static const char prn_fcb[11] = {'P','R','N',' ',' ',' ',' ',' ',' ',' ',' '};
  dos_write_sft_device(base, 0, DOS_DEVICE_CON_OFFSET, 0x80d3u, con_fcb);
  dos_write_sft_device(base, 1, DOS_DEVICE_CON_OFFSET, 0x80d3u, con_fcb);
  dos_write_sft_device(base, 2, DOS_DEVICE_CON_OFFSET, 0x80d3u, con_fcb);
  dos_write_sft_device(base, 3, DOS_DEVICE_AUX_OFFSET, 0x80c0u, aux_fcb);
  dos_write_sft_device(base, 4, DOS_DEVICE_PRN_OFFSET, 0x80a0u, prn_fcb);

  /* CDS entries A:-E:, with only C: marked as a mounted local volume. */
  static const char a_path[] = "A:\\";
  static const char b_path[] = "B:\\";
  memcpy(base + DOS_CDS_OFFSET, a_path, sizeof(a_path));
  memcpy(base + DOS_CDS_OFFSET + DOS_CDS_ENTRY_SIZE, b_path, sizeof(b_path));
  dos_put16(base, (U16)(DOS_CDS_OFFSET + 0x43u), 0);
  dos_put16(base, (U16)(DOS_CDS_OFFSET + DOS_CDS_ENTRY_SIZE + 0x43u), 0);
  dos_put16(base, (U16)(DOS_CDS_OFFSET + 3u * DOS_CDS_ENTRY_SIZE + 0x43u), 0);
  dos_put16(base, (U16)(DOS_CDS_OFFSET + 4u * DOS_CDS_ENTRY_SIZE + 0x43u), 0);

  dos_update_mcb_chain(vm);
  DosSyncSystemData(vm, current_directory);
}

void DosUpdateSftEntry(DOS_VM *vm, U16 handle) {
  if (!vm || !vm->Memory || handle < 5u || handle >= DOS_SFT_ENTRY_COUNT)
    return;
  U8 *base = dos_system_base(vm);
  U16 offset = (U16)(DOS_SFT_OFFSET + DOS_SFT_HEADER_SIZE +
                     handle * DOS_SFT_ENTRY_SIZE);
  memset(base + offset, 0, DOS_SFT_ENTRY_SIZE);
  DOS_FILE *file = vm->Files[handle];
  if (!file) return;

  dos_put16(base, offset, 1u);
  dos_put16(base, (U16)(offset + 2u),
            (U16)((file->Readable ? 0u : 1u) |
                  (file->Writable ? 2u : 0u)));
  dos_put16(base, (U16)(offset + 5u), 2u); /* drive C: */
  dos_put32(base, (U16)(offset + 0x11u), (U32)file->Size);
  dos_put32(base, (U16)(offset + 0x15u), (U32)file->Position);
  memset(base + offset + 0x20u, ' ', 11);

  const char *name = file->Path;
  for (const char *p = file->Path; *p; p++)
    if (*p == '/' || *p == '\\') name = p + 1;
  unsigned i = 0;
  while (name[i] && name[i] != '.' && i < 8u) {
    base[offset + 0x20u + i] = dos_upper((U8)name[i]);
    i++;
  }
  const char *dot = name;
  while (*dot && *dot != '.') dot++;
  if (*dot == '.') {
    dot++;
    for (i = 0; dot[i] && i < 3u; i++)
      base[offset + 0x28u + i] = dos_upper((U8)dot[i]);
  }
  dos_put16(base, (U16)(offset + 0x31u), PSP);
}
