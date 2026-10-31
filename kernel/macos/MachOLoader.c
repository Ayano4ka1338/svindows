#include "MachO.h"
#include <svindows/Mm.h>
#include <svindows/Rtl.h>

static void macho_rollback_segments(
    const MACHO_SEGMENT_64 segments[MACHO_MAX_SEGMENTS], UINT32 count)
{
    for (UINT32 i = 0; i < count; i++) {
        const MACHO_SEGMENT_64 *segment = &segments[i];
        if (!segment->VmSize || !segment->InitialProtection) continue;
        UINT64 first = PAGE_ALIGN(segment->VmAddress);
        UINT64 last = PAGE_ALIGN_UP(segment->VmAddress + segment->VmSize);
        for (UINT64 page = first; page < last; page += PAGE_SIZE) {
            UINT64 physical = MmGetPhysicalAddress(page);
            if (physical == MM_INVALID_PHYS) continue;
            if (MmUnmapPage(page)) MmFreePages(PAGE_ALIGN(physical), 1);
        }
    }
}

NTSTATUS MacOsLoadMachO64(const void *image, SIZE_T size,
                          MACOS_MACHO_INFO *info)
{
    if (!image || !info) return STATUS_INVALID_PARAMETER;
    const void *slice = NULL;
    SIZE_T slice_size = 0;
    MACHO_SEGMENT_64 segments[MACHO_MAX_SEGMENTS];
    NTSTATUS status = MacOsPrepareMachO64(image, size, &slice, &slice_size,
                                          segments, info);
    if (!NT_SUCCESS(status)) return status;
    image = slice;
    size = slice_size;
    UINT32 count = info->SegmentCount;
    /* Check every destination before the first allocation to avoid leaving a
     * half-mapped image when an address conflicts with an existing region. */
    for (UINT32 i = 0; i < count; i++) {
        const MACHO_SEGMENT_64 *segment = &segments[i];
        if (!segment->VmSize || !segment->InitialProtection) continue;
        UINT64 first = PAGE_ALIGN(segment->VmAddress);
        UINT64 last = PAGE_ALIGN_UP(segment->VmAddress + segment->VmSize);
        for (UINT64 page = first; page < last; page += PAGE_SIZE)
            if (MmGetPageFlags(page) & PTE_PRESENT)
                return STATUS_CONFLICTING_ADDRESSES;
    }

    for (UINT32 i = 0; i < count; i++) {
        MACHO_SEGMENT_64 *segment = &segments[i];
        if (!segment->VmSize || !segment->InitialProtection) continue;
        UINT64 first = PAGE_ALIGN(segment->VmAddress);
        UINT64 last = PAGE_ALIGN_UP(segment->VmAddress + segment->VmSize);
        for (UINT64 page = first; page < last; page += PAGE_SIZE) {
            UINT64 physical = MmAllocatePage();
            if (physical == MM_INVALID_PHYS) {
                macho_rollback_segments(segments, count);
                return STATUS_NO_MEMORY;
            }
            if (!MmMapPage(page, physical, PTE_USER | PTE_WRITE)) {
                MmFreePages(physical, 1);
                macho_rollback_segments(segments, count);
                return STATUS_NO_MEMORY;
            }
            memset((void *)(ULONG_PTR)page, 0, PAGE_SIZE);
        }
        memcpy((void *)(ULONG_PTR)segment->VmAddress,
               (const UINT8 *)image + segment->FileOffset,
               (SIZE_T)segment->FileSize);
    }
    for (UINT32 i = 0; i < count; i++) {
        MACHO_SEGMENT_64 *segment = &segments[i];
        if (!segment->VmSize || !segment->InitialProtection) continue;
        UINT64 protection = PTE_USER |
            ((segment->InitialProtection & MACHO_PROTECTION_WRITE) ?
             PTE_WRITE : 0) |
            ((segment->InitialProtection & MACHO_PROTECTION_EXECUTE) ?
             0 : PTE_NX);
        MmProtectRangeFlags(segment->VmAddress, segment->VmSize, protection);
    }
    return STATUS_SUCCESS;
}
