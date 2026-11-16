/*
 * mm/Pmm.c - the physical memory manager: a bitmap page-frame allocator.
 *
 * One bit per 4 KiB frame (1 = in use, 0 = free). The bitmap itself is parked
 * in physical RAM immediately above the kernel image. Everything is reached
 * through MmPhysToVirt, so the allocator keeps working across the switch from
 * the identity map to the high-half direct map.
 */
#include <svindows/Mm.h>
#include <svindows/Ke.h>
#include <svindows/Rtl.h>

/* Physical end of the loaded kernel image (from the linker script). */
extern char __kernel_phys_end[];

typedef struct _PMM_STATE {
    UINT64 bitmap_phys;   /* physical base of the bitmap                */
    UINT64 bitmap_bytes;  /* size of the bitmap in bytes                */
    UINT64 total_pages;   /* frames covered (highest_address / PAGE)    */
    UINT64 free_pages;
    UINT64 used_pages;
    UINT64 usable_pages; /* available RAM after permanent reservations */
    UINT64 next_hint;     /* frame index to start the next search from  */
    UINT64 peak_allocated;
} PMM_STATE;

static PMM_STATE g_pmm;
static KSPIN_LOCK g_pmm_lock;

/* Svindows commits eagerly and has no swap: each allocated frame is backed
 * immediately. Permanent boot reservations are excluded from usable_pages. */
static void pmm_record_commit(void)
{
    UINT64 committed=g_pmm.usable_pages-g_pmm.free_pages;
    if(committed>g_pmm.peak_allocated)g_pmm.peak_allocated=committed;
}

static ALWAYS_INLINE UINT8 *pmm_bitmap(void)
{
    return (UINT8 *)MmPhysToVirt(g_pmm.bitmap_phys);
}

static ALWAYS_INLINE void bit_set(UINT64 frame)   /* mark used */
{
    pmm_bitmap()[frame >> 3] |= (UINT8)(1u << (frame & 7));
}

static ALWAYS_INLINE void bit_clear(UINT64 frame) /* mark free */
{
    pmm_bitmap()[frame >> 3] &= (UINT8)~(1u << (frame & 7));
}

static ALWAYS_INLINE BOOLEAN bit_test(UINT64 frame) /* TRUE if used */
{
    return (BOOLEAN)((pmm_bitmap()[frame >> 3] >> (frame & 7)) & 1);
}

/* Reserve [base, base+length): mark frames used (idempotent for accounting). */
static void reserve_range(UINT64 base, UINT64 length)
{
    UINT64 first = base >> PAGE_SHIFT;
    UINT64 last = (base + length + PAGE_MASK) >> PAGE_SHIFT;
    for (UINT64 f = first; f < last && f < g_pmm.total_pages; f++) {
        if (!bit_test(f)) {
            bit_set(f);
            if (g_pmm.free_pages)
                g_pmm.free_pages--;
            if (g_pmm.usable_pages)
                g_pmm.usable_pages--;
            g_pmm.used_pages++;
        }
    }
}

/* Release an available region: mark frames free. */
static void free_range(UINT64 base, UINT64 length)
{
    UINT64 first = (base + PAGE_MASK) >> PAGE_SHIFT; /* don't free a partial head */
    UINT64 last = (base + length) >> PAGE_SHIFT;     /* nor a partial tail        */
    for (UINT64 f = first; f < last && f < g_pmm.total_pages; f++) {
        if (bit_test(f)) {
            bit_clear(f);
            g_pmm.free_pages++;
            g_pmm.usable_pages++;
            if (g_pmm.used_pages)
                g_pmm.used_pages--;
        }
    }
}

static BOOLEAN pmm_ranges_overlap(UINT64 a, UINT64 a_size,
                                 UINT64 b, UINT64 b_size)
{
    if (!a_size || !b_size || a > ~(UINT64)0 - a_size ||
        b > ~(UINT64)0 - b_size)
        return FALSE;
    return a < b + b_size && b < a + a_size;
}

/* The bootstrap page tables identity-map only the low 4 GiB. Keep the bitmap
 * there and inside a firmware-reported RAM range, and never place it over a
 * loaded module or the Multiboot information block. Limine is free to put any
 * of those objects immediately after the kernel image. */
static UINT64 pmm_find_bitmap(const MM_MEMORY_MAP *map, UINT64 kernel_end,
                             UINT64 bitmap_size,
                             const MM_PHYSICAL_RESERVATION *reservations,
                             UINT32 reservation_count)
{
    const UINT64 low_identity_limit = 0x100000000ULL;
    UINT64 bitmap_span = PAGE_ALIGN_UP(bitmap_size);
    for (UINT32 i = 0; i < map->count; ++i) {
        const MM_MEMORY_REGION *region = &map->regions[i];
        if (region->type != MmRegionAvailable || !region->length ||
            region->base > ~(UINT64)0 - region->length)
            continue;
        UINT64 region_start = PAGE_ALIGN_UP(region->base);
        UINT64 region_end = PAGE_ALIGN(region->base + region->length);
        if (region_start < kernel_end)
            region_start = PAGE_ALIGN_UP(kernel_end);
        if (region_end > low_identity_limit)
            region_end = low_identity_limit;

        for (UINT64 candidate = region_start;
             candidate <= region_end && bitmap_span <= region_end - candidate;) {
            UINT64 next = candidate + bitmap_span;
            BOOLEAN conflict = pmm_ranges_overlap(candidate, bitmap_span,
                                                   KERNEL_PHYS_BASE,
                                                   kernel_end - KERNEL_PHYS_BASE);
            for (UINT32 r = 0; !conflict && r < reservation_count; ++r) {
                if (pmm_ranges_overlap(candidate, bitmap_span,
                                       reservations[r].base,
                                       reservations[r].length)) {
                    conflict = TRUE;
                    if (reservations[r].base <= ~(UINT64)0 - reservations[r].length) {
                        UINT64 after = PAGE_ALIGN_UP(reservations[r].base +
                                                     reservations[r].length);
                        if (after > next) next = after;
                    }
                }
            }
            if (!conflict)
                return candidate;
            if (next <= candidate)
                break;
            candidate = next;
        }
    }
    return MM_INVALID_PHYS;
}

void MmInitializePhysicalMemory(const MM_MEMORY_MAP *map,
                                const MM_PHYSICAL_RESERVATION *reservations,
                                UINT32 reservation_count)
{
    KeInitializeSpinLock(&g_pmm_lock);
    g_pmm.total_pages = map->highest_address >> PAGE_SHIFT;
    g_pmm.bitmap_bytes = (g_pmm.total_pages + 7) / 8;

    /* Do not assume Limine leaves free RAM directly after the kernel image. */
    UINT64 kernel_end = (UINT64)(ULONG_PTR)__kernel_phys_end;
    g_pmm.bitmap_phys = pmm_find_bitmap(map, kernel_end,
                                        g_pmm.bitmap_bytes,
                                        reservations, reservation_count);
    if (g_pmm.bitmap_phys == MM_INVALID_PHYS)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "no low RAM available for physical-memory bitmap");
    UINT64 bitmap_pages = BYTES_TO_PAGES(g_pmm.bitmap_bytes);

    /* Start with everything marked used, then free the available regions. */
    memset(pmm_bitmap(), 0xFF, g_pmm.bitmap_bytes);
    g_pmm.free_pages = 0;
    g_pmm.used_pages = g_pmm.total_pages;
    g_pmm.usable_pages = 0;
    g_pmm.peak_allocated = 0;

    for (UINT32 i = 0; i < map->count; i++) {
        const MM_MEMORY_REGION *r = &map->regions[i];
        if (r->type == MmRegionAvailable)
            free_range(r->base, r->length);
    }

    /* Reserve what must never be handed out. */
    reserve_range(0, 0x100000);                                  /* low 1 MiB      */
    reserve_range(KERNEL_PHYS_BASE, kernel_end - KERNEL_PHYS_BASE); /* kernel image */
    reserve_range(g_pmm.bitmap_phys, bitmap_pages << PAGE_SHIFT);   /* the bitmap   */
    for (UINT32 i = 0; i < reservation_count; ++i)
        reserve_range(reservations[i].base, reservations[i].length);

    g_pmm.next_hint = 0x100000 >> PAGE_SHIFT;

    KeLog("[mm]   pmm: %lu MiB usable, %lu frames free (bitmap @ 0x%lx, %lu KiB)\n",
          (unsigned long)((g_pmm.free_pages << PAGE_SHIFT) >> 20),
          (unsigned long)g_pmm.free_pages,
          (unsigned long)g_pmm.bitmap_phys,
          (unsigned long)(g_pmm.bitmap_bytes >> 10));
}

void MmReservePhysicalRange(UINT64 base, UINT64 length)
{
    if (!g_pmm.total_pages || !length || base > ~(UINT64)0 - length)
        return;
    UINT64 interrupt_state = KiAcquireRawSpinLock(&g_pmm_lock);
    reserve_range(base, length);
    KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
}

UINT64 MmAllocatePage(void)
{
    UINT64 interrupt_state = KiAcquireRawSpinLock(&g_pmm_lock);
    for (UINT64 scan = 0; scan < g_pmm.total_pages; scan++) {
        UINT64 f = g_pmm.next_hint + scan;
        if (f >= g_pmm.total_pages)
            f -= g_pmm.total_pages; /* wrap */

        if (!bit_test(f)) {
            bit_set(f);
            g_pmm.free_pages--;
            g_pmm.used_pages++;
            pmm_record_commit();
            g_pmm.next_hint = f + 1;
            UINT64 result = f << PAGE_SHIFT;
            KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
            return result;
        }
    }
    KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
    return MM_INVALID_PHYS;
}

UINT64 MmAllocatePages(SIZE_T count)
{
    if (count == 0)
        return MM_INVALID_PHYS;
    if (count == 1)
        return MmAllocatePage();

    /* First-fit search for `count` contiguous free frames. */
    UINT64 interrupt_state = KiAcquireRawSpinLock(&g_pmm_lock);
    UINT64 run = 0, start = 0;
    for (UINT64 f = 0; f < g_pmm.total_pages; f++) {
        if (!bit_test(f)) {
            if (run == 0)
                start = f;
            if (++run == count) {
                for (UINT64 k = start; k < start + count; k++)
                    bit_set(k);
                g_pmm.free_pages -= count;
                g_pmm.used_pages += count;
                pmm_record_commit();
                g_pmm.next_hint = start + count;
                UINT64 result = start << PAGE_SHIFT;
                KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
                return result;
            }
        } else {
            run = 0;
        }
    }
    KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
    return MM_INVALID_PHYS;
}


/* Legacy PCI bus masters (including transitional VirtIO) use 32-bit DMA
 * addresses. Keep this constraint in the physical allocator instead of
 * allocating arbitrary RAM and rejecting it later in each device driver. */
#define PMM_DMA32_PAGE_LIMIT (0x100000000ULL >> PAGE_SHIFT)

UINT64 MmAllocatePageBelow4G(void)
{
    UINT64 interrupt_state = KiAcquireRawSpinLock(&g_pmm_lock);
    UINT64 limit = g_pmm.total_pages < PMM_DMA32_PAGE_LIMIT ?
                   g_pmm.total_pages : PMM_DMA32_PAGE_LIMIT;
    UINT64 start = g_pmm.next_hint < limit ? g_pmm.next_hint : 0;
    for (UINT64 scan = 0; scan < limit; ++scan) {
        UINT64 frame = start + scan;
        if (frame >= limit) frame -= limit;
        if (!bit_test(frame)) {
            bit_set(frame);
            g_pmm.free_pages--;
            g_pmm.used_pages++;
            pmm_record_commit();
            g_pmm.next_hint = frame + 1;
            UINT64 result = frame << PAGE_SHIFT;
            KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
            return result;
        }
    }
    KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
    return MM_INVALID_PHYS;
}

UINT64 MmAllocatePagesBelow4G(SIZE_T count)
{
    if (!count) return MM_INVALID_PHYS;
    if (count == 1) return MmAllocatePageBelow4G();
    UINT64 interrupt_state = KiAcquireRawSpinLock(&g_pmm_lock);
    UINT64 limit = g_pmm.total_pages < PMM_DMA32_PAGE_LIMIT ?
                   g_pmm.total_pages : PMM_DMA32_PAGE_LIMIT;
    if ((UINT64)count > limit) {
        KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
        return MM_INVALID_PHYS;
    }
    UINT64 run = 0, start = 0;
    for (UINT64 frame = 0; frame < limit; ++frame) {
        if (!bit_test(frame)) {
            if (!run) start = frame;
            if (++run == count) {
                for (UINT64 page = start; page < start + count; ++page)
                    bit_set(page);
                g_pmm.free_pages -= count;
                g_pmm.used_pages += count;
                pmm_record_commit();
                g_pmm.next_hint = start + count;
                UINT64 result = start << PAGE_SHIFT;
                KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
                return result;
            }
        } else {
            run = 0;
        }
    }
    KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
    return MM_INVALID_PHYS;
}

UINT64 MmAllocatePagesBelow16M(SIZE_T count)
{
    if (!count) return MM_INVALID_PHYS;
    UINT64 interrupt_state = KiAcquireRawSpinLock(&g_pmm_lock);
    UINT64 limit = g_pmm.total_pages < (0x01000000ULL >> PAGE_SHIFT) ?
                   g_pmm.total_pages : (0x01000000ULL >> PAGE_SHIFT);
    if ((UINT64)count > limit) {
        KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
        return MM_INVALID_PHYS;
    }
    for (UINT64 start = 0; start + count <= limit; ++start) {
        UINT64 i;
        for (i = 0; i < count && !bit_test(start + i); ++i) {}
        if (i == count) {
            for (i = 0; i < count; ++i) bit_set(start + i);
            g_pmm.free_pages -= count;
            g_pmm.used_pages += count;
            pmm_record_commit();
            g_pmm.next_hint = start + count;
            KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
            return start << PAGE_SHIFT;
        }
        start += i;
    }
    KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
    return MM_INVALID_PHYS;
}

void MmFreePages(UINT64 phys, SIZE_T count)
{
    UINT64 interrupt_state = KiAcquireRawSpinLock(&g_pmm_lock);
    UINT64 first = phys >> PAGE_SHIFT;
    for (UINT64 f = first; f < first + count && f < g_pmm.total_pages; f++) {
        if (bit_test(f)) {
            bit_clear(f);
            g_pmm.free_pages++;
            g_pmm.used_pages--;
        }
    }
    if (first < g_pmm.next_hint)
        g_pmm.next_hint = first;
    KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
}

UINT64 MmTotalPages(void) { return g_pmm.total_pages; }
UINT64 MmFreePageCount(void)
{
    UINT64 interrupt_state = KiAcquireRawSpinLock(&g_pmm_lock);
    UINT64 value = g_pmm.free_pages;
    KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
    return value;
}
UINT64 MmUsablePageCount(void)
{
    UINT64 interrupt_state = KiAcquireRawSpinLock(&g_pmm_lock);
    UINT64 value = g_pmm.usable_pages;
    KiReleaseRawSpinLock(&g_pmm_lock, interrupt_state);
    return value;
}

void MmQueryMemoryUsage(MM_MEMORY_USAGE *usage)
{
    UINT64 flags=KiAcquireRawSpinLock(&g_pmm_lock);
    usage->AvailablePages=g_pmm.free_pages;
    usage->CommitLimit=g_pmm.usable_pages;
    usage->CommittedPages=g_pmm.usable_pages-g_pmm.free_pages;
    usage->PeakCommitment=g_pmm.peak_allocated;
    KiReleaseRawSpinLock(&g_pmm_lock,flags);
}
