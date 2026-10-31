/* PE image mapping, section layout and page protection.
 * Included by kernel/ldr/Pe.c. */
/* ------------------------------------------------------------------ */
/* Mapping and protection                                             */
/* ------------------------------------------------------------------ */
#include "Validation.inc"

/* Only private image frames allocated by this loader belong here. */
static void LdrpReleaseImagePages(UINT64 base, UINT64 size)
{
    for (UINT64 off = 0; off < size; off += PAGE_SIZE) {
        UINT64 pa = MmGetPhysicalAddress(base + off);
        if (pa != MM_INVALID_PHYS && MmUnmapPage(base + off))
            MmFreePages(PAGE_ALIGN(pa), 1);
    }
}

static NTSTATUS LdrpAllocateImagePages(UINT64 base, UINT64 size)
{
    for (UINT64 off = 0; off < size; off += PAGE_SIZE) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS) {
            LdrpReleaseImagePages(base, off);
            return STATUS_NO_MEMORY;
        }
        if (!MmMapPage(base + off, pa, PTE_USER | PTE_WRITE)) {
            MmFreePages(pa, 1);
            LdrpReleaseImagePages(base, off);
            return STATUS_CONFLICTING_ADDRESSES;
        }
    }
    return STATUS_SUCCESS;
}

static void set_range_perms(UINT64 start, UINT64 size, UINT64 flags)
{
    UINT64 first = PAGE_ALIGN(start);
    UINT64 last = PAGE_ALIGN_UP(start + size);
    for (UINT64 va = first; va < last; va += PAGE_SIZE) {
        UINT64 pa = MmGetPhysicalAddress(va);
        if (pa != MM_INVALID_PHYS)
            MmMapPage(va, pa, flags);
    }
}

static BOOLEAN apply_relocations(UINT64 base, INT64 delta, PIMAGE_NT_HEADERS64 nt)
{
    if (delta == 0)
        return TRUE;
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (dir.Size == 0)
        return FALSE;

    UINT64 cursor = base + dir.VirtualAddress;
    UINT64 end = cursor + dir.Size;
    UINT64 image_size = nt->OptionalHeader.SizeOfImage;
    while (cursor < end) {
        if (end - cursor < sizeof(IMAGE_BASE_RELOCATION)) return FALSE;
        IMAGE_BASE_RELOCATION *block = (IMAGE_BASE_RELOCATION *)cursor;
        UINT32 block_size = block->SizeOfBlock;
        UINT32 block_rva = block->VirtualAddress;
        if (block_size < sizeof(IMAGE_BASE_RELOCATION) ||
            block_size > end - cursor || (block_size & 1))
            return FALSE;
        UINT32 count =
            (block_size - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(UINT16);
        UINT16 *entries = (UINT16 *)(cursor + sizeof(IMAGE_BASE_RELOCATION));
        for (UINT32 i = 0; i < count; i++) {
            if ((entries[i] >> 12) == IMAGE_REL_BASED_DIR64) {
                UINT64 target = (UINT64)block_rva + (entries[i] & 0x0FFF);
                if (target > image_size || sizeof(UINT64) > image_size - target)
                    return FALSE;
                UINT64 value;
                memcpy(&value, (void *)(base + target), sizeof(value));
                value += (UINT64)delta;
                memcpy((void *)(base + target), &value, sizeof(value));
            } else if (entries[i] >> 12) return FALSE;
        }
        cursor += block_size;
    }
    return TRUE;
}

static BOOLEAN image_range_free(UINT64 base, UINT64 size)
{
    for (UINT64 off = 0; off < size; off += PAGE_SIZE)
        if (MmGetPhysicalAddress(base + off) != MM_INVALID_PHYS)
            return FALSE;
    return TRUE;
}

static UINT64 choose_image_base(UINT64 preferred, UINT64 image_size,
                                const IMAGE_OPTIONAL_HEADER64 *opt)
{
    if (!preferred || preferred >= MM_USER_MAX ||
        image_size > MM_USER_MAX - preferred)
        return 0;
    if (image_range_free(preferred, image_size))
        return preferred;

    IMAGE_DATA_DIRECTORY reloc =
        opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!reloc.VirtualAddress || !reloc.Size)
        return 0; /* fixed image collides and cannot be rebased */

    UINT64 base = (g_dynamic_image_next + 0xffffULL) & ~0xffffULL;
    while (!image_range_free(base, image_size))
        base += (image_size + 0xffffULL) & ~0xffffULL;
    g_dynamic_image_next = base + ((image_size + 0xffffULL) & ~0xffffULL);
    return base;
}

/*
 * Map a resource-only image (no entry point, no imports, no relocations) at a
 * free address. Nothing in it depends on the load address: resource lookups are
 * all relative to the resource directory, so any base will do.
 */
static NTSTATUS LdrpMapResourceImage(const void *file, SIZE_T file_size,
                                     UINT64 *base_out)
{
    const UINT8 *bytes = (const UINT8 *)file;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)bytes;
    const UINT8 *nt = bytes + dos->e_lfanew;
    UINT16 sections = *(const UINT16 *)(nt + 6);
    UINT16 opt_size = *(const UINT16 *)(nt + 20);
    const UINT8 *opt = nt + 24;
    UINT32 size_of_image = *(const UINT32 *)(opt + 56);
    UINT32 size_of_headers = *(const UINT32 *)(opt + 60);

    UINT64 image_size = PAGE_ALIGN_UP(size_of_image);
    if (!image_size || image_size > 0x4000000ULL)
        return STATUS_INVALID_IMAGE_FORMAT;

    UINT64 base = (g_dynamic_image_next + 0xffffULL) & ~0xffffULL;
    while (!image_range_free(base, image_size))
        base += (image_size + 0xffffULL) & ~0xffffULL;
    g_dynamic_image_next = base + ((image_size + 0xffffULL) & ~0xffffULL);

    NTSTATUS allocation = LdrpAllocateImagePages(base, image_size);
    if (!NT_SUCCESS(allocation)) return allocation;
    memset((void *)base, 0, image_size);
    if (size_of_headers <= file_size)
        memcpy((void *)base, bytes, size_of_headers);

    const IMAGE_SECTION_HEADER *sec =
        (const IMAGE_SECTION_HEADER *)(opt + opt_size);
    for (UINT16 i = 0; i < sections; i++) {
        const IMAGE_SECTION_HEADER *sh = &sec[i];
        if (!sh->SizeOfRawData)
            continue;
        if ((UINT64)sh->PointerToRawData + sh->SizeOfRawData > file_size)
            continue;
        if ((UINT64)sh->VirtualAddress + sh->SizeOfRawData > image_size)
            continue;
        memcpy((void *)(base + sh->VirtualAddress),
               bytes + sh->PointerToRawData, sh->SizeOfRawData);
    }

    KeLog("[ldr]  resource-only image mapped at %p (size 0x%lx)\n",
          (void *)base, (unsigned long)image_size);
    *base_out = base;
    return STATUS_SUCCESS;
}

/* Map an image at its preferred base and lay out headers + sections (writable). */
static NTSTATUS LdrpMapImage(const void *file, SIZE_T file_size, UINT64 *base_out)
{
    const UINT8 *bytes = (const UINT8 *)file;
    if (!LdrpValidateImageFile(file, file_size))
        return STATUS_INVALID_IMAGE_FORMAT;

    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)bytes;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > file_size)
        return STATUS_INVALID_IMAGE_FORMAT;

    const IMAGE_NT_HEADERS64 *nt =
        (const IMAGE_NT_HEADERS64 *)(bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return STATUS_INVALID_IMAGE_FORMAT;

    /*
     * A resource-only image has no entry point or imports, so its contents
     * are address-independent even when it has no relocation directory.
     * Localized .mui files are often PE32, while imageres.dll is PE32+ with
     * a fixed base occupied by ntdll. Both must take the resource mapping
     * path before code-module base selection and machine checks.
     */
    {
        const UINT8 *opt = (const UINT8 *)&nt->OptionalHeader;
        UINT16 magic = *(const UINT16 *)opt;
        UINT32 entry = *(const UINT32 *)(opt + 16);
        UINT32 directories = magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC ? 96 : 112;
        UINT32 imports = *(const UINT32 *)(opt + directories + 8);
        UINT32 resources = *(const UINT32 *)(opt + directories + 16);
        if ((magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
             magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) &&
            entry == 0 && imports == 0 && resources != 0)
            return LdrpMapResourceImage(file, file_size, base_out);
    }

    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return STATUS_INVALID_IMAGE_FORMAT;

    const IMAGE_OPTIONAL_HEADER64 *opt = &nt->OptionalHeader;
    UINT64 image_size = PAGE_ALIGN_UP(opt->SizeOfImage);
    UINT64 base = choose_image_base(opt->ImageBase, image_size, opt);
    if (!base)
        return STATUS_CONFLICTING_ADDRESSES;

    if (base != opt->ImageBase)
        KeLog("[ldr]  rebasing image %p -> %p (size 0x%lx)\n",
              (void *)opt->ImageBase, (void *)base,
              (unsigned long)image_size);

    NTSTATUS allocation = LdrpAllocateImagePages(base, image_size);
    if (!NT_SUCCESS(allocation)) return allocation;

    memset((void *)base, 0, image_size);
    if (opt->SizeOfHeaders <= file_size)
        memcpy((void *)base, bytes, opt->SizeOfHeaders);

    const IMAGE_SECTION_HEADER *sec =
        (const IMAGE_SECTION_HEADER *)((const UINT8 *)opt +
                                       nt->FileHeader.SizeOfOptionalHeader);
    for (UINT16 i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER *s = &sec[i];
        if (s->SizeOfRawData == 0)
            continue;
        if ((UINT64)s->PointerToRawData + s->SizeOfRawData > file_size)
            continue;
        if ((UINT64)s->VirtualAddress + s->SizeOfRawData > image_size)
            continue;
        memcpy((void *)(base + s->VirtualAddress),
               bytes + s->PointerToRawData, s->SizeOfRawData);
    }

    if (!apply_relocations(base, (INT64)(base - opt->ImageBase),
                           (PIMAGE_NT_HEADERS64)(base + dos->e_lfanew))) {
        LdrpReleaseImagePages(base, image_size);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    *base_out = base;
    return STATUS_SUCCESS;
}

static void LdrpProtectImage(UINT64 base)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    UINT64 image_size = PAGE_ALIGN_UP(nt->OptionalHeader.SizeOfImage);

    set_range_perms(base, image_size, PTE_USER); /* read-only + user by default */

    PIMAGE_SECTION_HEADER sec = first_section(nt);
    for (UINT16 i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        UINT64 flags = PTE_USER;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)
            flags |= PTE_WRITE;
        set_range_perms(base + sec[i].VirtualAddress, sec[i].VirtualSize, flags);
    }
}
