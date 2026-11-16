/*
 * mm/MmInit.c - Memory Manager bring-up.
 *
 * Owns the direct-map base (0 while the identity map is live, MM_DIRECT_MAP_BASE
 * afterwards) and sequences the three stages: parse the firmware memory map,
 * start the physical allocator, then install the kernel's own page tables.
 */
#include <svindows/Mm.h>
#include <svindows/Ke.h>
#include <svindows/Multiboot2.h>
#include <svindows/Gfx.h>

/* 0 => identity map (early boot); becomes MM_DIRECT_MAP_BASE once VMM is up. */
UINT64 MmDirectMapBase = 0;

static MM_MEMORY_MAP g_memory_map;

void MmInitialize(UINT64 mb_info_phys)
{
    KeLog("[mm]   parsing Multiboot2 memory map...\n");
    if (!MmParseMultibootMemoryMap(mb_info_phys, &g_memory_map))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "bootloader supplied no usable memory map");

    for (UINT32 i = 0; i < g_memory_map.count; i++) {
        const MM_MEMORY_REGION *r = &g_memory_map.regions[i];
        KeLog("[mm]   e820[%2u] 0x%016lx..0x%016lx  %s\n",
              i, (unsigned long)r->base,
              (unsigned long)(r->base + r->length),
              MmRegionTypeName(r->type));
    }
    KeLog("[mm]   %lu MiB available, top of RAM at 0x%lx\n",
          (unsigned long)(g_memory_map.total_available >> 20),
          (unsigned long)g_memory_map.highest_address);

    MM_PHYSICAL_RESERVATION reservations[3];
    UINT32 reservation_count = 0;
    UINT64 initrd_base = 0, initrd_size = 0;
    BOOLEAN has_initrd = MmFindMultibootModule(mb_info_phys, "initrd.fat",
                                               &initrd_base, &initrd_size);
    if (has_initrd && initrd_size) {
        reservations[reservation_count++] = (MM_PHYSICAL_RESERVATION){
            initrd_base, initrd_size
        };
    }

    /* The boot information block and framebuffer can also occupy RAM that
     * Limine reports as available. Reserve them before the PMM chooses and
     * clears its bitmap; otherwise bitmap initialization can corrupt either. */
    MB_INFO_HEADER *boot_info = (MB_INFO_HEADER *)MmPhysToVirt(mb_info_phys);
    if (boot_info->total_size) {
        reservations[reservation_count++] = (MM_PHYSICAL_RESERVATION){
            mb_info_phys, boot_info->total_size
        };
    }
    if (GfxFramebuffer.Present && GfxFramebuffer.Pitch &&
        GfxFramebuffer.Height &&
        (UINT64)GfxFramebuffer.Pitch <=
            ~(UINT64)0 / GfxFramebuffer.Height) {
        UINT64 fb_size = (UINT64)GfxFramebuffer.Pitch * GfxFramebuffer.Height;
        if (fb_size && GfxFramebuffer.PhysAddr <= ~(UINT64)0 - fb_size)
            reservations[reservation_count++] = (MM_PHYSICAL_RESERVATION){
                GfxFramebuffer.PhysAddr, fb_size
            };
    }

    MmInitializePhysicalMemory(&g_memory_map, reservations,
                               reservation_count);
    if (has_initrd) {
        KeLog("[mm]   reserved initrd.fat physical range 0x%lx..0x%lx\n",
              (unsigned long)initrd_base,
              (unsigned long)(initrd_base + initrd_size));
    }
    MmInitializeVirtualMemory(&g_memory_map);
}
