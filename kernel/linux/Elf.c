/* Validated ELF64 image mapping. Relocation belongs to the user interpreter. */
#include <svindows/Linux.h>
#include <svindows/Mm.h>
#include <svindows/Rtl.h>

typedef struct _ELF64_HEADER {
    UINT8 Ident[16];
    UINT16 Type, Machine;
    UINT32 Version;
    UINT64 Entry, ProgramOffset, SectionOffset;
    UINT32 Flags;
    UINT16 HeaderSize, ProgramEntrySize, ProgramCount;
    UINT16 SectionEntrySize, SectionCount, SectionNameIndex;
} ELF64_HEADER;
typedef struct _ELF64_PROGRAM {
    UINT32 Type, Flags;
    UINT64 Offset, VirtualAddress, PhysicalAddress, FileSize, MemorySize, Align;
} ELF64_PROGRAM;

_Static_assert(sizeof(ELF64_HEADER) == 64, "ELF64 header layout");
_Static_assert(sizeof(ELF64_PROGRAM) == 56, "ELF64 program layout");

#define ELF_PT_LOAD 1u
#define ELF_PT_DYNAMIC 2u
#define ELF_PT_INTERP 3u
#define ELF_ET_EXEC 2u
#define ELF_ET_DYN 3u
#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define LINUX_IMAGE_LOW 0x10000ULL
#define LINUX_IMAGE_HIGH 0x60000000ULL
#define LINUX_IMAGE_MAX_BYTES (64ULL * 1024 * 1024)

BOOLEAN LinuxIsElfImage(const void *image, SIZE_T size)
{
    const UINT8 *bytes = (const UINT8 *)image;
    return image && size >= 4 && bytes[0] == 0x7f && bytes[1] == 'E' &&
           bytes[2] == 'L' && bytes[3] == 'F';
}

NTSTATUS LinuxLoadElf64(const void *image, SIZE_T size, UINT64 pie_base,
                        BOOLEAN is_interpreter, LINUX_ELF_INFO *info)
{
    if (!image || !info ||
        size < sizeof(ELF64_HEADER))
        return STATUS_INVALID_IMAGE_FORMAT;
    const UINT8 *bytes = (const UINT8 *)image;
    const ELF64_HEADER *header = (const ELF64_HEADER *)image;
    if (!LinuxIsElfImage(image, size) || header->Ident[4] != 2 ||
        header->Ident[5] != 1 || header->Ident[6] != 1 ||
        (header->Type != ELF_ET_EXEC && header->Type != ELF_ET_DYN) ||
        (is_interpreter && header->Type != ELF_ET_DYN) ||
        header->Machine != 62 || header->Version != 1 ||
        header->HeaderSize != sizeof(*header) ||
        header->ProgramEntrySize != sizeof(ELF64_PROGRAM) ||
        !header->ProgramCount || header->ProgramCount > 32 ||
        header->ProgramOffset > size ||
        (UINT64)header->ProgramCount * sizeof(ELF64_PROGRAM) >
            size - header->ProgramOffset)
        return STATUS_INVALID_IMAGE_FORMAT;

    UINT64 base = header->Type == ELF_ET_DYN ? pie_base : 0;
    if ((base & PAGE_MASK) || (header->Type == ELF_ET_DYN && !base))
        return STATUS_INVALID_IMAGE_FORMAT;
    memset(info, 0, sizeof(*info));

    const ELF64_PROGRAM *programs = (const ELF64_PROGRAM *)(bytes +
                                                           header->ProgramOffset);
    UINT64 mapped_bytes = 0, phdr_va = 0, image_end = 0;
    BOOLEAN entry_executable = FALSE, loaded = FALSE;
    for (UINT16 i = 0; i < header->ProgramCount; i++) {
        const ELF64_PROGRAM *program = &programs[i];
        if (program->Type == ELF_PT_INTERP) {
            if (is_interpreter || info->Interpreter[0] ||
                program->FileSize < 2 ||
                program->FileSize > sizeof(info->Interpreter) ||
                program->Offset > size ||
                program->FileSize > size - program->Offset)
                return STATUS_INVALID_IMAGE_FORMAT;
            const char *path = (const char *)(bytes + program->Offset);
            if (path[0] != '/' || path[program->FileSize - 1] != 0)
                return STATUS_INVALID_IMAGE_FORMAT;
            for (UINT64 j = 0; j + 1 < program->FileSize; j++)
                if (!path[j] || path[j] == '\\' || path[j] == ':')
                    return STATUS_INVALID_IMAGE_FORMAT;
            memcpy(info->Interpreter, path, (SIZE_T)program->FileSize);
            continue;
        }
        if (program->Type != ELF_PT_LOAD) continue;
        if (program->VirtualAddress > LINUX_IMAGE_HIGH ||
            base > LINUX_IMAGE_HIGH - program->VirtualAddress)
            return STATUS_INVALID_IMAGE_FORMAT;
        UINT64 virtual_address = base + program->VirtualAddress;
        if (program->FileSize > program->MemorySize ||
            (program->Flags & ~7u) ||
            program->Offset > size || program->FileSize > size - program->Offset ||
            virtual_address < LINUX_IMAGE_LOW ||
            virtual_address >= LINUX_IMAGE_HIGH ||
            program->MemorySize > LINUX_IMAGE_HIGH - virtual_address ||
            ((program->VirtualAddress - program->Offset) & PAGE_MASK) ||
            (program->Align && (program->Align & (program->Align - 1))) ||
            (program->Align > 1 &&
             ((program->VirtualAddress - program->Offset) &
              (program->Align - 1))))
            return STATUS_INVALID_IMAGE_FORMAT;
        if (!program->MemorySize) continue;
        UINT64 first = PAGE_ALIGN(virtual_address);
        UINT64 last = PAGE_ALIGN_UP(virtual_address + program->MemorySize);
        if (last < first || last - first > LINUX_IMAGE_MAX_BYTES ||
            mapped_bytes > LINUX_IMAGE_MAX_BYTES - (last - first))
            return STATUS_INVALID_IMAGE_FORMAT;
        mapped_bytes += last - first;
        if (last > image_end) image_end = last;
        loaded = TRUE;
        if ((program->Flags & ELF_PF_X) &&
            header->Entry >= program->VirtualAddress &&
            header->Entry - program->VirtualAddress < program->MemorySize)
            entry_executable = TRUE;
        if (header->ProgramOffset >= program->Offset &&
            header->ProgramOffset - program->Offset < program->FileSize &&
            (UINT64)header->ProgramCount * sizeof(ELF64_PROGRAM) <=
                program->FileSize - (header->ProgramOffset - program->Offset))
            phdr_va = virtual_address +
                      (header->ProgramOffset - program->Offset);
    }
    if (!loaded || !entry_executable || !phdr_va)
        return STATUS_INVALID_IMAGE_FORMAT;

    /* Validate every page before mapping any; segment page overlap is rejected. */
    for (UINT16 i = 0; i < header->ProgramCount; i++) {
        const ELF64_PROGRAM *a = &programs[i];
        if (a->Type != ELF_PT_LOAD || !a->MemorySize) continue;
        UINT64 a_first = PAGE_ALIGN(base + a->VirtualAddress);
        UINT64 a_last = PAGE_ALIGN_UP(base + a->VirtualAddress + a->MemorySize);
        for (UINT16 j = 0; j < i; j++) {
            const ELF64_PROGRAM *b = &programs[j];
            if (b->Type != ELF_PT_LOAD || !b->MemorySize) continue;
            UINT64 b_first = PAGE_ALIGN(base + b->VirtualAddress);
            UINT64 b_last = PAGE_ALIGN_UP(base + b->VirtualAddress + b->MemorySize);
            if (a_first < b_last && b_first < a_last)
                return STATUS_INVALID_IMAGE_FORMAT;
        }
        for (UINT64 page = a_first; page < a_last; page += PAGE_SIZE)
            if (MmGetPageFlags(page) & PTE_PRESENT)
                return STATUS_CONFLICTING_ADDRESSES;
    }
    for (UINT16 i = 0; i < header->ProgramCount; i++) {
        const ELF64_PROGRAM *program = &programs[i];
        if (program->Type != ELF_PT_LOAD || !program->MemorySize) continue;
        UINT64 virtual_address = base + program->VirtualAddress;
        UINT64 first = PAGE_ALIGN(virtual_address);
        UINT64 last = PAGE_ALIGN_UP(virtual_address + program->MemorySize);
        for (UINT64 page = first; page < last; page += PAGE_SIZE) {
            UINT64 physical = MmAllocatePage();
            if (physical == MM_INVALID_PHYS)
                return STATUS_NO_MEMORY;
            if (!MmMapPage(page, physical, PTE_USER | PTE_WRITE)) {
                MmFreePages(physical, 1);
                return STATUS_NO_MEMORY;
            }
            memset((void *)page, 0, PAGE_SIZE);
        }
        memcpy((void *)(ULONG_PTR)virtual_address, bytes + program->Offset,
               (SIZE_T)program->FileSize);
    }
    for (UINT16 i = 0; i < header->ProgramCount; i++) {
        const ELF64_PROGRAM *program = &programs[i];
        if (program->Type != ELF_PT_LOAD || !program->MemorySize) continue;
        UINT64 protection = PTE_USER |
            ((program->Flags & ELF_PF_W) ? PTE_WRITE : 0) |
            ((program->Flags & ELF_PF_X) ? 0 : PTE_NX);
        MmProtectRangeFlags(base + program->VirtualAddress, program->MemorySize,
                            protection);
    }
    info->Entry = base + header->Entry;
    info->ProgramHeaders = phdr_va;
    info->ProgramCount = header->ProgramCount;
    info->ImageEnd = image_end;
    info->LoadBase = base;
    return STATUS_SUCCESS;
}
