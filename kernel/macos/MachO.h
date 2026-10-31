#ifndef SVINDOWS_MACOS_MACHO_PRIVATE_H
#define SVINDOWS_MACOS_MACHO_PRIVATE_H

#include <svindows/MacOS.h>

typedef struct _MACHO_HEADER_64 {
    UINT32 Magic;
    INT32 CpuType;
    INT32 CpuSubtype;
    UINT32 FileType;
    UINT32 CommandCount;
    UINT32 CommandBytes;
    UINT32 Flags;
    UINT32 Reserved;
} MACHO_HEADER_64;

typedef struct _MACHO_LOAD_COMMAND {
    UINT32 Command;
    UINT32 Size;
} MACHO_LOAD_COMMAND;

typedef struct _MACHO_SEGMENT_64 {
    UINT32 Command;
    UINT32 Size;
    char Name[16];
    UINT64 VmAddress;
    UINT64 VmSize;
    UINT64 FileOffset;
    UINT64 FileSize;
    INT32 MaximumProtection;
    INT32 InitialProtection;
    UINT32 SectionCount;
    UINT32 Flags;
} MACHO_SEGMENT_64;

typedef struct _MACHO_MAIN_COMMAND {
    UINT32 Command;
    UINT32 Size;
    UINT64 EntryOffset;
    UINT64 StackSize;
} MACHO_MAIN_COMMAND;

_Static_assert(sizeof(MACHO_HEADER_64) == 32, "Mach-O 64 header layout");
_Static_assert(sizeof(MACHO_SEGMENT_64) == 72, "Mach-O segment layout");
_Static_assert(sizeof(MACHO_MAIN_COMMAND) == 24, "Mach-O main command layout");

#define MACHO_MAGIC_64 0xfeedfacfu
#define MACHO_FAT_MAGIC 0xcafebabeu
#define MACHO_FAT_MAGIC_64 0xcafebabfu
#define MACHO_CPU_TYPE_X86_64 0x01000007
#define MACHO_FILE_EXECUTE 2u
#define MACHO_COMMAND_SEGMENT_64 0x19u
#define MACHO_COMMAND_MAIN 0x80000028u
#define MACHO_COMMAND_LOAD_DYLIB 0xcu
#define MACHO_COMMAND_LOAD_DYLINKER 0xeu
#define MACHO_COMMAND_LAZY_LOAD_DYLIB 0x20u
#define MACHO_COMMAND_RPATH 0x8000001cu
#define MACHO_COMMAND_LOAD_WEAK_DYLIB 0x80000018u
#define MACHO_COMMAND_REEXPORT_DYLIB 0x8000001fu
#define MACHO_COMMAND_LOAD_UPWARD_DYLIB 0x80000023u
#define MACHO_FLAG_PIE 0x00200000u
#define MACHO_PROTECTION_READ 1
#define MACHO_PROTECTION_WRITE 2
#define MACHO_PROTECTION_EXECUTE 4
#define MACHO_USER_LOW 0x10000ULL
#define MACHO_USER_HIGH 0x0000800000000000ULL
#define MACHO_MAX_COMMANDS 256u
#define MACHO_MAX_SEGMENTS 64u
#define MACHO_MAX_ARCHITECTURES 64u
#define MACHO_MAX_MAPPED_BYTES (256ULL * 1024 * 1024)

NTSTATUS MacOsPrepareMachO64(const void *image, SIZE_T size,
                             const void **slice_out,
                             SIZE_T *slice_size_out,
                             MACHO_SEGMENT_64 segments[MACHO_MAX_SEGMENTS],
                             MACOS_MACHO_INFO *info);

#endif
