/* Bounded Linux brk and mmap arena; fbdev pages alias the physical display. */
#include "Internal.h"
#include <svindows/Io.h>
#include <svindows/Ob.h>
#include <svindows/Rtl.h>
#include <svindows/Hal.h>
#include <svindows/Console.h>

#define LINUX_BRK_LIMIT 0x60000000ULL
#define LINUX_MAP_START 0x100000000ULL
#define LINUX_MAP_END 0x200000000ULL
#define LINUX_MAP_MAX_BYTES (64ULL * 1024 * 1024)
#define LINUX_PROT_WRITE 2u
#define LINUX_PROT_EXEC 4u
#define LINUX_MAP_PRIVATE 2u
#define LINUX_MAP_SHARED 1u
#define LINUX_MAP_FIXED 0x10u
#define LINUX_MAP_DENYWRITE 0x800u /* ignored by Linux */
#define LINUX_MAP_ANONYMOUS 0x20u
#define LINUX_MAP_FIXED_NOREPLACE 0x100000u

static UINT64 linux_page_flags(UINT64 protection)
{
    /* Supervisor-only leaf mappings represent PROT_NONE. */
    if (!protection) return PTE_NX;
    return PTE_USER | ((protection & LINUX_PROT_WRITE) ? PTE_WRITE : 0) |
           ((protection & LINUX_PROT_EXEC) ? 0 : PTE_NX);
}

void LinuxReleaseFramebufferOwner(LINUX_MEMORY_CONTEXT *memory)
{
    if (!memory->FramebufferOwner) return;
    memory->FramebufferOwner = FALSE;
    if (HalConsoleReleaseFramebuffer()) ConRedraw();
}

static void linux_unmap_pages(UINT64 base, UINT64 end, BOOLEAN release)
{
    for (UINT64 page = base; page < end; page += PAGE_SIZE) {
        UINT64 physical = MmGetPhysicalAddress(page);
        if (physical != MM_INVALID_PHYS && MmUnmapPage(page) && release)
            MmFreePages(physical & ~PAGE_MASK, 1);
    }
}

static void linux_release_pages(UINT64 base, UINT64 end)
{
    linux_unmap_pages(base, end, TRUE);
}

NTSTATUS LinuxInitializeMemory(PKPROCESS process, UINT64 image_end)
{
    if (image_end < 0x10000 || image_end > LINUX_BRK_LIMIT ||
        (image_end & PAGE_MASK))
        return STATUS_INVALID_IMAGE_FORMAT;
    LINUX_MEMORY_CONTEXT *context = &process->LinuxContext->Memory;
    context->BrkBase = context->BrkCurrent = context->BrkMapped = image_end;
    context->NextMap = LINUX_MAP_START;
    return STATUS_SUCCESS;
}

UINT64 LinuxBrk(PKPROCESS process, UINT64 requested)
{
    LINUX_MEMORY_CONTEXT *context = &process->LinuxContext->Memory;
    if (!requested) return context->BrkCurrent;
    UINT64 previous = context->BrkCurrent;
    if (requested < context->BrkBase || requested > LINUX_BRK_LIMIT)
        return previous; /* Linux brk returns the old break on failure. */
    UINT64 end = PAGE_ALIGN_UP(requested);
    if (end > context->BrkMapped) {
        UINT64 page = context->BrkMapped;
        for (; page < end; page += PAGE_SIZE) {
            if (MmGetPageFlags(page) & PTE_PRESENT) break;
            UINT64 physical = MmAllocatePage();
            if (physical == MM_INVALID_PHYS) break;
            if (!MmMapPage(page, physical, PTE_USER | PTE_WRITE | PTE_NX)) {
                MmFreePages(physical, 1);
                break;
            }
            memset((void *)(ULONG_PTR)page, 0, PAGE_SIZE);
        }
        if (page < end) {
            linux_release_pages(context->BrkMapped, page);
            return previous;
        }
    } else if (end < context->BrkMapped) {
        linux_release_pages(end, context->BrkMapped);
    }
    context->BrkMapped = end;
    context->BrkCurrent = requested;
    return requested;
}

static BOOLEAN linux_owned_page(const LINUX_MEMORY_CONTEXT *context,
                                UINT64 page)
{
    for (UINT32 i = 0; i < LINUX_MAP_REGIONS; i++) {
        UINT64 base = context->Regions[i].Base;
        if (base && page >= base && page - base < context->Regions[i].Size)
            return TRUE;
    }
    return FALSE;
}

static BOOLEAN linux_range_free(const LINUX_MEMORY_CONTEXT *context,
                                UINT64 base, UINT64 size,
                                UINT64 *next_candidate)
{
    UINT64 end = base + size;
    for (UINT32 i = 0; i < LINUX_MAP_REGIONS; i++) {
        UINT64 other = context->Regions[i].Base;
        UINT64 other_end = other + context->Regions[i].Size;
        if (other && base < other_end && other < end) {
            *next_candidate = other_end;
            return FALSE;
        }
    }
    for (UINT64 page = base; page < end; page += PAGE_SIZE) {
        if (MmGetPageFlags(page) & PTE_PRESENT) {
            *next_candidate = page + PAGE_SIZE;
            return FALSE;
        }
    }
    return TRUE;
}

static UINT64 linux_find_space(LINUX_MEMORY_CONTEXT *context, UINT64 hint,
                               UINT64 size, BOOLEAN fixed)
{
    UINT64 next;
    if (hint >= LINUX_MAP_START && hint <= LINUX_MAP_END - size &&
        linux_range_free(context, hint, size, &next))
        return hint;
    if (fixed) return 0;
    UINT64 starts[2] = {context->NextMap, LINUX_MAP_START};
    UINT64 ends[2] = {LINUX_MAP_END, context->NextMap};
    for (unsigned pass = 0; pass < 2; pass++) {
        UINT64 candidate = starts[pass];
        while (candidate <= ends[pass] - size) {
            if (linux_range_free(context, candidate, size, &next))
                return candidate;
            candidate = PAGE_ALIGN_UP(next);
        }
    }
    return 0;
}

static UINT64 linux_map_framebuffer(LINUX_MEMORY_CONTEXT *context,
                                    UINT64 hint, UINT64 size,
                                    UINT64 protection, UINT64 flags,
                                    UINT64 offset)
{
    if (flags & ~(UINT64)(LINUX_MAP_SHARED | LINUX_MAP_FIXED_NOREPLACE))
        return LinuxError(LINUX_EOPNOTSUPP);
    UINT64 bytes = LinuxFramebufferSize();
    UINT64 physical = LinuxFramebufferPhysicalAddress();
    if (!bytes || (physical & PAGE_MASK) ||
        offset > PAGE_ALIGN_UP(bytes) ||
        size > PAGE_ALIGN_UP(bytes) - offset)
        return LinuxError(LINUX_EINVAL);
    BOOLEAN fixed = !!(flags & LINUX_MAP_FIXED_NOREPLACE);
    if (fixed && (hint & PAGE_MASK)) return LinuxError(LINUX_EINVAL);
    if (!fixed) hint = PAGE_ALIGN(hint);
    if (fixed && (hint < LINUX_MAP_START ||
                  hint > LINUX_MAP_END - size))
        return LinuxError(LINUX_EINVAL);
    UINT32 slot = 0;
    while (slot < LINUX_MAP_REGIONS && context->Regions[slot].Base) slot++;
    if (slot == LINUX_MAP_REGIONS) return LinuxError(LINUX_ENOMEM);
    UINT64 base = linux_find_space(context, hint, size, fixed);
    if (!base) return LinuxError(fixed ? LINUX_EEXIST : LINUX_ENOMEM);
    UINT64 page = base;
    for (; page < base + size; page += PAGE_SIZE)
        if (!MmMapPage(page, physical + offset + page - base,
                       linux_page_flags(protection) | PTE_PCD | PTE_PWT | PTE_SHARED))
            break;
    if (page < base + size) {
        linux_unmap_pages(base, page, FALSE);
        return LinuxError(LINUX_ENOMEM);
    }
    context->Regions[slot] = (LINUX_MAP_REGION){base, size,
                                               LINUX_MAP_FRAMEBUFFER};
    if (!context->FramebufferOwner) {
        context->FramebufferOwner = TRUE;
        HalConsoleAcquireFramebuffer();
    }
    context->NextMap = base + size;
    if (context->NextMap >= LINUX_MAP_END)
        context->NextMap = LINUX_MAP_START;
    return base;
}

UINT64 LinuxMmap(PKPROCESS process, const UINT64 args[6])
{
    LINUX_MEMORY_CONTEXT *context = &process->LinuxContext->Memory;
    UINT64 hint = args[0], length = args[1], protection = args[2];
    UINT64 flags = args[3], fd = args[4], offset = args[5];
    if (!length || length > LINUX_MAP_MAX_BYTES ||
        protection & ~7ULL || offset & PAGE_MASK)
        return LinuxError(LINUX_EINVAL);
    UINT64 size = PAGE_ALIGN_UP(length);
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_FRAMEBUFFER) {
        if (!(flags & LINUX_MAP_SHARED))
            return LinuxError(LINUX_EOPNOTSUPP);
        return linux_map_framebuffer(context, hint, size, protection,
                                     flags, offset);
    }
    if (flags & ~(UINT64)(LINUX_MAP_PRIVATE | LINUX_MAP_SHARED |
                          LINUX_MAP_ANONYMOUS | LINUX_MAP_DENYWRITE |
                          LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE))
        return LinuxError(LINUX_EOPNOTSUPP);
    UINT64 map_type = flags & (LINUX_MAP_PRIVATE | LINUX_MAP_SHARED);
    if (map_type != LINUX_MAP_PRIVATE && map_type != LINUX_MAP_SHARED)
        return LinuxError(LINUX_EINVAL);
    /* Read-only MAP_SHARED file mappings are used by Imlib2 to mmap image
     * contents.  Since FAT file objects are immutable snapshots while open,
     * a private physical copy is equivalent for PROT_READ.  Writable shared
     * mappings would require writeback/page-cache coherence and are refused. */
    if (map_type == LINUX_MAP_SHARED && (protection & LINUX_PROT_WRITE))
        return LinuxError(LINUX_EOPNOTSUPP);
    if ((flags & LINUX_MAP_ANONYMOUS) && map_type == LINUX_MAP_SHARED)
        return LinuxError(LINUX_EOPNOTSUPP);
    BOOLEAN replace = !!(flags & LINUX_MAP_FIXED);
    BOOLEAN fixed = !!(flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE));
    if (fixed && (hint & PAGE_MASK)) return LinuxError(LINUX_EINVAL);
    if (!fixed) hint = PAGE_ALIGN(hint);
    if (fixed && (hint < LINUX_MAP_START ||
                  hint > LINUX_MAP_END - size))
        return LinuxError(LINUX_EINVAL);
    POBJECT file_object = NULL;
    const UINT8 *data = NULL;
    SIZE_T data_size = 0;
    if (!(flags & LINUX_MAP_ANONYMOUS)) {
        HANDLE handle;
        if (!LinuxFileHandle(process, fd, FALSE, &handle))
            return LinuxError(LINUX_EBADF);
        NTSTATUS status = IoReferenceFileData(handle, &file_object,
                                               &data, &data_size);
        if (!NT_SUCCESS(status)) return LinuxError(LINUX_ENODEV);
    }

    if (replace) {
        UINT64 end = hint + size;
        for (UINT32 i = 0; i < LINUX_MAP_REGIONS; i++) {
            LINUX_MAP_REGION *region = &context->Regions[i];
            UINT64 region_end = region->Base + region->Size;
            if (region->Base && hint < region_end && region->Base < end &&
                region->Kind != LINUX_MAP_OWNED) {
                if (file_object) ObDereferenceObject(file_object);
                return LinuxError(LINUX_EOPNOTSUPP);
            }
        }
        UINT64 unmap = LinuxMunmap(process, hint, size);
        if (unmap) {
            if (file_object) ObDereferenceObject(file_object);
            return unmap;
        }
    }
    UINT32 slot = 0;
    while (slot < LINUX_MAP_REGIONS && context->Regions[slot].Base) slot++;
    if (slot == LINUX_MAP_REGIONS) {
        KeLog("[lx] mmap region table exhausted (max %u)\n",
              (unsigned)LINUX_MAP_REGIONS);
        if (file_object) ObDereferenceObject(file_object);
        return LinuxError(LINUX_ENOMEM);
    }

    UINT64 base = linux_find_space(context, hint, size, fixed);
    if (!base) {
        if (file_object) ObDereferenceObject(file_object);
        return LinuxError(fixed ? LINUX_EEXIST : LINUX_ENOMEM);
    }
    UINT64 page = base;
    for (; page < base + size; page += PAGE_SIZE) {
        UINT64 physical = MmAllocatePage();
        if (physical == MM_INVALID_PHYS) break;
        if (!MmMapPage(page, physical, PTE_USER | PTE_WRITE | PTE_NX)) {
            MmFreePages(physical, 1);
            break;
        }
        memset((void *)(ULONG_PTR)page, 0, PAGE_SIZE);
        UINT64 relative = page - base;
        if (file_object && offset < data_size &&
            relative < data_size - offset) {
            UINT64 available = data_size - offset - relative;
            UINT64 copied = available < PAGE_SIZE ? available : PAGE_SIZE;
            memcpy((void *)(ULONG_PTR)page,
                   data + offset + relative, (SIZE_T)copied);
        }
    }
    if (file_object) ObDereferenceObject(file_object);
    if (page < base + size) {
        linux_release_pages(base, page);
        return LinuxError(LINUX_ENOMEM);
    }
    MmProtectRangeFlags(base, size, linux_page_flags(protection));
    context->Regions[slot] = (LINUX_MAP_REGION){base, size, LINUX_MAP_OWNED};
    context->NextMap = base + size;
    if (context->NextMap >= LINUX_MAP_END)
        context->NextMap = LINUX_MAP_START;
    return base;
}

UINT64 LinuxMremap(PKPROCESS process, const UINT64 args[6])
{
    const UINT64 old_address = args[0];
    const UINT64 old_length = args[1];
    const UINT64 new_length = args[2];
    const UINT64 flags = args[3];
    const UINT64 MAYMOVE = 1u;
    const UINT64 FIXED = 2u;
    const UINT64 DONTUNMAP = 4u;

    if (!old_length || !new_length || old_length > LINUX_MAP_MAX_BYTES ||
        new_length > LINUX_MAP_MAX_BYTES || (old_address & PAGE_MASK) ||
        (flags & ~(MAYMOVE | FIXED | DONTUNMAP)) ||
        (flags & (FIXED | DONTUNMAP)))
        return LinuxError(LINUX_EINVAL);
    UINT64 old_size = PAGE_ALIGN_UP(old_length);
    UINT64 new_size = PAGE_ALIGN_UP(new_length);
    if (new_size > LINUX_MAP_MAX_BYTES)
        return LinuxError(LINUX_ENOMEM);

    LINUX_MEMORY_CONTEXT *context = &process->LinuxContext->Memory;
    LINUX_MAP_REGION *region = NULL;
    for (UINT32 i = 0; i < LINUX_MAP_REGIONS; i++) {
        if (context->Regions[i].Base == old_address) {
            region = &context->Regions[i];
            break;
        }
    }
    if (!region || region->Kind != LINUX_MAP_OWNED ||
        region->Size != old_size)
        return LinuxError(LINUX_EINVAL);
    if (new_size == old_size) return old_address;

    if (new_size < old_size) {
        UINT64 status = LinuxMunmap(process, old_address + new_size,
                                    old_size - new_size);
        return status ? status : old_address;
    }
    if (!(flags & MAYMOVE)) return LinuxError(LINUX_ENOMEM);

    UINT64 page_flags = MmGetPageFlags(old_address);
    UINT64 protection = 1u; /* Linux PROT_READ */
    if (page_flags & PTE_WRITE) protection |= 2u;
    if (!(page_flags & PTE_NX)) protection |= 4u;
    UINT64 mmap_args[6] = {
        0, new_size, protection, 2u | 0x20u,
        (UINT64)(INT64)-1, 0
    };
    UINT64 new_address = LinuxMmap(process, mmap_args);
    if ((INT64)new_address < 0) return new_address;
    memcpy((void *)(ULONG_PTR)new_address,
           (const void *)(ULONG_PTR)old_address,
           (SIZE_T)(old_size < new_size ? old_size : new_size));
    UINT64 status = LinuxMunmap(process, old_address, old_size);
    if (status) {
        LinuxMunmap(process, new_address, new_size);
        return status;
    }
    return new_address;
}

UINT64 LinuxMunmap(PKPROCESS process, UINT64 address, UINT64 length)
{
    LINUX_MEMORY_CONTEXT *context = &process->LinuxContext->Memory;
    if (!length || (address & PAGE_MASK) ||
        address < LINUX_MAP_START || address >= LINUX_MAP_END ||
        length > LINUX_MAP_END - address)
        return LinuxError(LINUX_EINVAL);
    UINT64 end = PAGE_ALIGN_UP(address + length);
    if (end > LINUX_MAP_END) return LinuxError(LINUX_EINVAL);
    UINT32 splits = 0, free_slots = 0;
    for (UINT32 i = 0; i < LINUX_MAP_REGIONS; i++) {
        UINT64 base = context->Regions[i].Base;
        if (!base) { free_slots++; continue; }
        UINT64 last = base + context->Regions[i].Size;
        if (base < address && last > end) splits++;
    }
    if (splits > free_slots) return LinuxError(LINUX_ENOMEM);

    for (UINT32 i = 0; i < LINUX_MAP_REGIONS; i++) {
        UINT64 base = context->Regions[i].Base;
        if (!base) continue;
        UINT64 last = base + context->Regions[i].Size;
        UINT64 first_cut = base > address ? base : address;
        UINT64 last_cut = last < end ? last : end;
        if (first_cut >= last_cut) continue;
        linux_unmap_pages(first_cut, last_cut,
                          context->Regions[i].Kind == LINUX_MAP_OWNED);
        if (first_cut == base && last_cut == last) {
            context->Regions[i] = (LINUX_MAP_REGION){0};
        } else if (first_cut == base) {
            context->Regions[i].Base = last_cut;
            context->Regions[i].Size = last - last_cut;
        } else if (last_cut == last) {
            context->Regions[i].Size = first_cut - base;
        } else {
            context->Regions[i].Size = first_cut - base;
            UINT32 free_slot = 0;
            while (free_slot < LINUX_MAP_REGIONS &&
                   context->Regions[free_slot].Base) free_slot++;
            context->Regions[free_slot] = (LINUX_MAP_REGION){
                last_cut, last - last_cut, context->Regions[i].Kind};
        }
    }
    if (context->FramebufferOwner) {
        BOOLEAN found = FALSE;
        for (UINT32 i = 0; i < LINUX_MAP_REGIONS; i++)
            if (context->Regions[i].Base &&
                context->Regions[i].Kind == LINUX_MAP_FRAMEBUFFER) {
                found = TRUE;
                break;
            }
        if (!found) LinuxReleaseFramebufferOwner(context);
    }
    return 0;
}

UINT64 LinuxMprotect(PKPROCESS process, UINT64 address, UINT64 length,
                     UINT64 protection)
{
    LINUX_MEMORY_CONTEXT *context = &process->LinuxContext->Memory;
    if (!context || !length || (address & PAGE_MASK) ||
        protection & ~7ULL || address < 0x10000 ||
        address >= LINUX_MAP_END ||
        length > LINUX_MAP_END - address)
        return LinuxError(LINUX_EINVAL);
    UINT64 end = PAGE_ALIGN_UP(address + length);
    BOOLEAN image_or_brk = address < LINUX_BRK_LIMIT;
    if (image_or_brk) {
        if (end > context->BrkMapped) return LinuxError(LINUX_ENOMEM);
        for (UINT64 page = address; page < end; page += PAGE_SIZE)
            if (!(MmGetPageFlags(page) & PTE_PRESENT))
                return LinuxError(LINUX_ENOMEM);
    } else {
        if (address < LINUX_MAP_START || end > LINUX_MAP_END)
            return LinuxError(LINUX_ENOMEM);
        for (UINT64 page = address; page < end; page += PAGE_SIZE)
            if (!linux_owned_page(context, page))
                return LinuxError(LINUX_ENOMEM);
    }
    MmProtectRangeFlags(address, end - address, linux_page_flags(protection));
    return 0;
}
