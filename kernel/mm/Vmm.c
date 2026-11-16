/*
 * mm/Vmm.c - the virtual memory manager: real 4-level page tables.
 *
 * Boot left us on the trampoline's tables (low 4 GiB identity + a 1 GiB
 * higher-half window). Here we build the kernel's own PML4 from scratch:
 *
 *   - the kernel image window at -2 GiB (0xFFFFFFFF80000000), and
 *   - a direct map of all physical RAM at 0xFFFF800000000000,
 *
 * then switch CR3 to it and abandon the identity map. From that point on
 * MmPhysToVirt resolves through the direct map. New fine-grained mappings (for
 * example the kernel heap) are added with MmMapPage.
 */
#include <svindows/Mm.h>
#include <svindows/Ke.h>
#include <svindows/Hal.h>
#include <svindows/Rtl.h>
#include <svindows/AddressSpace.h>

static UINT64 g_kernel_pml4_phys;
extern char __kernel_phys_end[];

UINT64 MmGetKernelAddressSpace(void)
{
    return g_kernel_pml4_phys;
}

#define ENTRIES_PER_TABLE 512
#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)

static ALWAYS_INLINE UINT64 *table_at(UINT64 phys)
{
    return (UINT64 *)MmPhysToVirt(phys);
}

static ALWAYS_INLINE void invlpg(UINT64 va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static ALWAYS_INLINE void load_cr3(UINT64 pml4_phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

static UINT64 alloc_zeroed_table(void)
{
    /* Before the CR3 switch MmPhysToVirt is an identity address and only
     * the low 4 GiB exists in the bootstrap tables. */
    UINT64 phys = MmDirectMapBase ? MmAllocatePage() : MmAllocatePageBelow4G();
    if (phys == MM_INVALID_PHYS)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "out of physical memory building page tables");
    memset(table_at(phys), 0, PAGE_SIZE);
    return phys;
}

/*
 * Return the phys of the next-level table under *entry, allocating it if absent.
 * Intermediate entries are marked writable and user-accessible; the actual
 * privilege of any page is decided by the leaf PTE (access requires U/S set at
 * every level, so a supervisor leaf stays kernel-only regardless).
 */
static UINT64 next_table(UINT64 *entry)
{
    if (!(*entry & PTE_PRESENT)) {
        UINT64 t = alloc_zeroed_table();
        *entry = t | PTE_PRESENT | PTE_WRITE | PTE_USER;
        return t;
    }
    /* Ensure an existing intermediate also permits user access below it. */
    *entry |= PTE_USER;
    return *entry & PTE_ADDR_MASK;
}

/* Convert an existing 2 MiB direct-map entry into a normal page table. This
 * is needed when a device range (notably the firmware framebuffer) must get
 * uncached 4 KiB mappings inside the otherwise write-back direct map. */
static UINT64 split_large_page(UINT64 *pde)
{
    UINT64 old = *pde;
    if (!(old & PTE_LARGE))
        return old & PTE_ADDR_MASK;

    UINT64 pt_phys = alloc_zeroed_table();
    UINT64 *pt = table_at(pt_phys);
    UINT64 base = old & PTE_ADDR_MASK & ~(LARGE_PAGE_SIZE - 1);
    UINT64 leaf_flags = old & ~PTE_ADDR_MASK;
    leaf_flags &= ~PTE_LARGE;
    for (UINT32 i = 0; i < ENTRIES_PER_TABLE; ++i)
        pt[i] = (base + (UINT64)i * PAGE_SIZE) | leaf_flags;

    /* Preserve effective permissions while moving the large-page mapping to
     * 4 KiB leaves. The PAT bit is not used by current large mappings. */
    *pde = pt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    return pt_phys;
}

/* Map a single 2 MiB page. Used only while building the initial address space. */
static void map_large(UINT64 pml4_phys, UINT64 va, UINT64 pa, UINT64 flags)
{
    UINT64 *pml4 = table_at(pml4_phys);
    UINT64 pdpt_phys = next_table(&pml4[PML4_INDEX(va)]);
    UINT64 *pdpt = table_at(pdpt_phys);
    UINT64 pd_phys = next_table(&pdpt[PDPT_INDEX(va)]);
    UINT64 *pd = table_at(pd_phys);

    pd[PD_INDEX(va)] = (pa & ~(LARGE_PAGE_SIZE - 1)) | flags | PTE_LARGE;
}

static BOOLEAN direct_map_ram_page(const MM_MEMORY_MAP *map, UINT64 phys,
                                   UINT64 *covered)
{
    for (UINT32 i = 0; i < map->count; ++i) {
        const MM_MEMORY_REGION *region = &map->regions[i];
        if (!region->length || region->base > ~(UINT64)0 - region->length)
            continue;
        UINT64 end = region->base + region->length;
        if (phys >= region->base && phys < end) {
            if (covered) *covered = end - phys;
            return region->type == MmRegionAvailable ||
                   region->type == MmRegionAcpiReclaimable ||
                   region->type == MmRegionAcpiNvs;
        }
    }
    if (covered) *covered = 0;
    return FALSE;
}

static void map_direct_chunk(UINT64 pml4_phys, const MM_MEMORY_MAP *map,
                             UINT64 phys)
{
    UINT64 covered = 0;
    BOOLEAN ram = direct_map_ram_page(map, phys, &covered);
    UINT64 cache_flags = ram ? 0 : PTE_PCD | PTE_PWT;
    UINT64 kernel_end = (UINT64)(ULONG_PTR)__kernel_phys_end;
    BOOLEAN kernel_overlap = phys < kernel_end &&
                             KERNEL_PHYS_BASE < phys + LARGE_PAGE_SIZE;
    if (covered >= LARGE_PAGE_SIZE && !(kernel_overlap && !ram)) {
        map_large(pml4_phys, MM_DIRECT_MAP_BASE + phys, phys,
                  PTE_PRESENT | PTE_WRITE | PTE_GLOBAL | cache_flags);
        return;
    }

    /* A 2 MiB chunk straddles firmware memory types. Keep RAM write-back and
     * device/reserved pages uncached instead of applying one cache policy to
     * the whole mixed region. */
    UINT64 *pml4 = table_at(pml4_phys);
    UINT64 pdpt_phys = next_table(&pml4[PML4_INDEX(MM_DIRECT_MAP_BASE + phys)]);
    UINT64 *pdpt = table_at(pdpt_phys);
    UINT64 pd_phys = next_table(&pdpt[PDPT_INDEX(MM_DIRECT_MAP_BASE + phys)]);
    UINT64 *pd = table_at(pd_phys);
    UINT64 pt_phys = alloc_zeroed_table();
    UINT64 *pt = table_at(pt_phys);
    for (UINT32 i = 0; i < ENTRIES_PER_TABLE; ++i) {
        UINT64 page = phys + (UINT64)i * PAGE_SIZE;
        UINT64 page_covered = 0;
        BOOLEAN page_ram = direct_map_ram_page(map, page, &page_covered) &&
                           page_covered >= PAGE_SIZE;
        /* The kernel's high-half image is already mapped write-back. Keep its
         * direct-map alias consistent even if firmware labels those pages
         * reserved. */
        if (page >= KERNEL_PHYS_BASE && page < (UINT64)(ULONG_PTR)__kernel_phys_end)
            page_ram = TRUE;
        UINT64 flags = PTE_PRESENT | PTE_WRITE | PTE_GLOBAL |
                       (page_ram ? 0 : PTE_PCD | PTE_PWT);
        pt[i] = page | flags;
    }
    pd[PD_INDEX(MM_DIRECT_MAP_BASE + phys)] =
        pt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
}

static void map_kernel_page(UINT64 pml4_phys, UINT64 virt, UINT64 phys)
{
    UINT64 *pml4 = table_at(pml4_phys);
    UINT64 pdpt_phys = next_table(&pml4[PML4_INDEX(virt)]);
    UINT64 *pdpt = table_at(pdpt_phys);
    UINT64 pd_phys = next_table(&pdpt[PDPT_INDEX(virt)]);
    UINT64 *pd = table_at(pd_phys);
    UINT64 pt_phys = next_table(&pd[PD_INDEX(virt)]);
    UINT64 *pt = table_at(pt_phys);
    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) |
                         PTE_PRESENT | PTE_WRITE | PTE_GLOBAL;
}

void MmInitializeVirtualMemory(const MM_MEMORY_MAP *map)
{
    g_kernel_pml4_phys = alloc_zeroed_table();

    /* Map only the loaded kernel image in the -2 GiB window. Mapping the
     * entire first GiB write-back creates cache-type aliases for legacy
     * devices such as VGA and PCI MMIO. */
    UINT64 kernel_end = (UINT64)(ULONG_PTR)__kernel_phys_end;
    for (UINT64 phys = KERNEL_PHYS_BASE; phys < kernel_end;) {
        UINT64 remaining = kernel_end - phys;
        UINT64 virt = KERNEL_VIRT_BASE + phys;
        if (!(phys & (LARGE_PAGE_SIZE - 1)) && remaining >= LARGE_PAGE_SIZE) {
            map_large(g_kernel_pml4_phys, virt, phys,
                      PTE_PRESENT | PTE_WRITE | PTE_GLOBAL);
            phys += LARGE_PAGE_SIZE;
        } else {
            map_kernel_page(g_kernel_pml4_phys, virt, phys);
            phys += PAGE_SIZE;
        }
    }

    /* Direct map: cover all physical RAM at MM_DIRECT_MAP_BASE with 2 MiB pages. */
    UINT64 top = (map->highest_address + LARGE_PAGE_SIZE - 1) & ~(LARGE_PAGE_SIZE - 1);
    if (top < 0x40000000ULL)
        top = 0x40000000ULL; /* always map at least the low 1 GiB */
    for (UINT64 pa = 0; pa < top; pa += LARGE_PAGE_SIZE)
        map_direct_chunk(g_kernel_pml4_phys, map, pa);

    /* Commit: switch to the new tables and start resolving physical addresses
     * through the direct map. These two lines must stay adjacent - after the
     * CR3 load the identity map is gone, so MmDirectMapBase must be updated
     * before any further MmPhysToVirt call. */
    load_cr3(g_kernel_pml4_phys);
    MmDirectMapBase = MM_DIRECT_MAP_BASE;

    /* The VGA framebuffer pointer was computed against the identity map; move
     * it onto the direct map. */
    HalVgaRelocate();

    KeLog("[mm]   vmm: kernel PML4 @ 0x%lx, direct map covers %lu MiB\n",
          (unsigned long)g_kernel_pml4_phys, (unsigned long)(top >> 20));
    KeLog("[mm]   vmm: identity map dropped; direct map base 0x%lx\n",
          (unsigned long)MM_DIRECT_MAP_BASE);
}

BOOLEAN MmMapPage(UINT64 virt, UINT64 phys, UINT64 flags)
{
    UINT64 *pml4 = table_at(MmGetCurrentAddressSpace());
    UINT64 pdpt_phys = next_table(&pml4[PML4_INDEX(virt)]);
    UINT64 *pdpt = table_at(pdpt_phys);
    UINT64 pd_phys = next_table(&pdpt[PDPT_INDEX(virt)]);
    UINT64 *pd = table_at(pd_phys);

    UINT64 pt_phys;
    if (pd[PD_INDEX(virt)] & PTE_LARGE)
        pt_phys = split_large_page(&pd[PD_INDEX(virt)]);
    else
        pt_phys = next_table(&pd[PD_INDEX(virt)]);
    UINT64 *pt = table_at(pt_phys);

    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    invlpg(virt);
    return TRUE;
}

BOOLEAN MmUnmapPage(UINT64 virt)
{
    return MmUnmapPageInAddressSpace(MmGetCurrentAddressSpace(), virt);
}

BOOLEAN MmUnmapPageInAddressSpace(UINT64 root, UINT64 virt)
{
    if (!root) return FALSE;
    UINT64 *pml4 = table_at(root);
    if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT))
        return FALSE;
    UINT64 *pdpt = table_at(pml4[PML4_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT))
        return FALSE;
    UINT64 *pd = table_at(pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pd[PD_INDEX(virt)] & PTE_PRESENT) || (pd[PD_INDEX(virt)] & PTE_LARGE))
        return FALSE;
    UINT64 *pt = table_at(pd[PD_INDEX(virt)] & PTE_ADDR_MASK);

    pt[PT_INDEX(virt)] = 0;
    if (root == MmGetCurrentAddressSpace()) invlpg(virt);
    return TRUE;
}

/* Release thread-owned mappings even when another process is current. */
void MmReleaseUserPagesInAddressSpace(UINT64 root, UINT64 base, SIZE_T pages)
{
    if (!root || !base || (base & PAGE_MASK) || base >= MM_USER_MAX ||
        pages > (MM_USER_MAX - base) / PAGE_SIZE)
        return;
    for (SIZE_T i = 0; i < pages; ++i) {
        UINT64 va = base + i * PAGE_SIZE;
        UINT64 pa = MmGetPhysicalAddressInAddressSpace(root, va);
        if (pa != MM_INVALID_PHYS && MmUnmapPageInAddressSpace(root, va))
            MmFreePages(PAGE_ALIGN(pa), 1);
    }
}

UINT64 MmGetPhysicalAddress(UINT64 virt)
{
    return MmGetPhysicalAddressInAddressSpace(MmGetCurrentAddressSpace(), virt);
}

UINT64 MmGetPhysicalAddressInAddressSpace(UINT64 root, UINT64 virt)
{
    if (!root) return MM_INVALID_PHYS;
    UINT64 *pml4 = table_at(root);
    if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT))
        return MM_INVALID_PHYS;
    UINT64 *pdpt = table_at(pml4[PML4_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT))
        return MM_INVALID_PHYS;
    UINT64 *pd = table_at(pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK);
    UINT64 pde = pd[PD_INDEX(virt)];
    if (!(pde & PTE_PRESENT))
        return MM_INVALID_PHYS;
    if (pde & PTE_LARGE)
        return (pde & PTE_ADDR_MASK & ~(LARGE_PAGE_SIZE - 1)) |
               (virt & (LARGE_PAGE_SIZE - 1));

    UINT64 *pt = table_at(pde & PTE_ADDR_MASK);
    UINT64 pte = pt[PT_INDEX(virt)];
    if (!(pte & PTE_PRESENT))
        return MM_INVALID_PHYS;
    return (pte & PTE_ADDR_MASK) | (virt & PAGE_MASK);
}

UINT64 MmGetPageFlags(UINT64 virt)
{
    UINT64 *pml4 = table_at(MmGetCurrentAddressSpace());
    UINT64 top = pml4[PML4_INDEX(virt)];
    if (!(top & PTE_PRESENT)) return 0;
    UINT64 *pdpt = table_at(top & PTE_ADDR_MASK);
    UINT64 middle = pdpt[PDPT_INDEX(virt)];
    if (!(middle & PTE_PRESENT)) return 0;
    UINT64 *pd = table_at(middle & PTE_ADDR_MASK);
    UINT64 pde = pd[PD_INDEX(virt)];
    if (!(pde & PTE_PRESENT)) return 0;
    UINT64 leaf = pde;
    if (!(pde & PTE_LARGE)) {
        UINT64 *pt = table_at(pde & PTE_ADDR_MASK);
        leaf = pt[PT_INDEX(virt)];
        if (!(leaf & PTE_PRESENT)) return 0;
    }
    /* R/W and U/S must be allowed at every level; NX at any level prevents
     * execution. A leaf alone does not describe the effective protection. */
    UINT64 allowed = top & middle & pde & leaf;
    return (leaf & ~(PTE_WRITE | PTE_USER | PTE_NX)) |
           (allowed & (PTE_WRITE | PTE_USER)) |
           ((top | middle | pde | leaf) & PTE_NX);
}

/* ------------------------------------------------------------------ */
/* User-pointer validation                                            */
/* ------------------------------------------------------------------ */

BOOLEAN MmIsUserAddress(UINT64 va)
{
    return (BOOLEAN)(va < MM_USER_MAX);
}

BOOLEAN MmProbeForRead(UINT64 va, UINT64 len)
{
    if (len == 0)
        return TRUE;
    if (va + len < va)                 /* wrap-around */
        return FALSE;
    if (!MmIsUserAddress(va) || !MmIsUserAddress(va + len - 1))
        return FALSE;
    for (UINT64 p = PAGE_ALIGN(va); p < va + len; p += PAGE_SIZE)
        if ((MmGetPageFlags(p) & (PTE_PRESENT | PTE_USER)) !=
            (PTE_PRESENT | PTE_USER))
            return FALSE;
    return TRUE;
}

BOOLEAN MmProbeForWrite(UINT64 va, UINT64 len)
{
    if (!MmProbeForRead(va, len))
        return FALSE;
    for (UINT64 p = PAGE_ALIGN(va); p < va + len; p += PAGE_SIZE) {
        UINT64 flags = MmGetPageFlags(p);
        if (!(flags & PTE_USER) || !(flags & PTE_WRITE))
            return FALSE;
    }
    return TRUE;
}

BOOLEAN MmCaptureUnicodeName(UINT64 ustr_va, char *out, SIZE_T out_size)
{
    if (out_size == 0)
        return FALSE;
    out[0] = 0;
    if (!MmProbeForRead(ustr_va, 16)) /* UNICODE_STRING is 16 bytes on x64 */
        return FALSE;

    UINT16 length = *(volatile UINT16 *)ustr_va;        /* bytes  */
    UINT64 buffer = *(volatile UINT64 *)(ustr_va + 8);  /* PWSTR  */
    if (length & 1)
        return FALSE;
    UINT32 chars = length / 2;
    if (chars && !MmProbeForRead(buffer, length))
        return FALSE;

    SIZE_T used = 0;
    for (UINT32 i = 0; i < chars; i++) {
        UINT32 cp = ((volatile UINT16 *)buffer)[i];
        if (!cp)
            return FALSE;
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (++i >= chars)
                return FALSE;
            UINT32 low = ((volatile UINT16 *)buffer)[i];
            if (low < 0xdc00 || low > 0xdfff)
                return FALSE;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return FALSE;
        }

        SIZE_T bytes = cp < 0x80 ? 1 : cp < 0x800 ? 2 :
                       cp < 0x10000 ? 3 : 4;
        if (used + bytes >= out_size)
            return FALSE;
        if (bytes == 1) {
            out[used++] = (char)cp;
        } else if (bytes == 2) {
            out[used++] = (char)(0xc0 | (cp >> 6));
            out[used++] = (char)(0x80 | (cp & 0x3f));
        } else if (bytes == 3) {
            out[used++] = (char)(0xe0 | (cp >> 12));
            out[used++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[used++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[used++] = (char)(0xf0 | (cp >> 18));
            out[used++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            out[used++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[used++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    out[used] = 0;
    return TRUE;
}

void MmProtectRange(UINT64 va, UINT64 size, BOOLEAN writable)
{
    MmProtectRangeFlags(va, size, PTE_USER | (writable ? PTE_WRITE : 0));
}

void MmProtectRangeFlags(UINT64 va, UINT64 size, UINT64 flags)
{
    UINT64 first = PAGE_ALIGN(va);
    UINT64 last = PAGE_ALIGN_UP(va + size);
    for (UINT64 p = first; p < last; p += PAGE_SIZE) {
        UINT64 pa = MmGetPhysicalAddress(p);
        if (pa != MM_INVALID_PHYS)
            MmMapPage(p, pa & ~PAGE_MASK, flags | (MmGetPageFlags(p) & PTE_SHARED));
    }
}
