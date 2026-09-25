/*
 * address_space.c - creation and activation of process page tables.
 *
 * Every process receives an empty lower half and shares the kernel half.  The
 * shared KUSER page is copied explicitly; executable images, PEB/TEB and stacks
 * are populated later while the new CR3 is active.
 */
#include <svindows/address_space.h>
#include <svindows/mm.h>
#include <svindows/rtl.h>

#define PT_ENTRIES 512
#define PML4_INDEX(va) (((va) >> 39) & 0x1ff)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1ff)
#define PD_INDEX(va)   (((va) >> 21) & 0x1ff)
#define PT_INDEX(va)   (((va) >> 12) & 0x1ff)

static UINT64 *table(UINT64 phys)
{
    return (UINT64 *)MmPhysToVirt(phys & PTE_ADDR_MASK);
}

static UINT64 next_table(UINT64 *entry)
{
    if (!(*entry & PTE_PRESENT)) {
        UINT64 phys = MmAllocatePage();
        if (phys == MM_INVALID_PHYS)
            return MM_INVALID_PHYS;
        memset(table(phys), 0, PAGE_SIZE);
        *entry = phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else {
        *entry |= PTE_USER;
    }
    return *entry & PTE_ADDR_MASK;
}

UINT64 MmGetCurrentAddressSpace(void)
{
    UINT64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & PTE_ADDR_MASK;
}

void MmSwitchAddressSpace(UINT64 directory_table_base)
{
    if (directory_table_base &&
        directory_table_base != MmGetCurrentAddressSpace())
        __asm__ volatile("mov %0, %%cr3" : : "r"(directory_table_base)
                         : "memory");
}

BOOLEAN MmMapPageInAddressSpace(UINT64 root, UINT64 va, UINT64 pa,
                                UINT64 flags)
{
    UINT64 *pml4 = table(root);
    UINT64 pdpt_phys = next_table(&pml4[PML4_INDEX(va)]);
    if (pdpt_phys == MM_INVALID_PHYS) return FALSE;
    UINT64 *pdpt = table(pdpt_phys);
    UINT64 pd_phys = next_table(&pdpt[PDPT_INDEX(va)]);
    if (pd_phys == MM_INVALID_PHYS) return FALSE;
    UINT64 *pd = table(pd_phys);
    if (pd[PD_INDEX(va)] & PTE_LARGE) return FALSE;
    UINT64 pt_phys = next_table(&pd[PD_INDEX(va)]);
    if (pt_phys == MM_INVALID_PHYS) return FALSE;
    table(pt_phys)[PT_INDEX(va)] = (pa & PTE_ADDR_MASK) |
                                   flags | PTE_PRESENT;
    if (root == MmGetCurrentAddressSpace())
        __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    return TRUE;
}

UINT64 MmCreateAddressSpace(void)
{
    UINT64 parent = MmGetCurrentAddressSpace();
    UINT64 root = MmAllocatePage();
    if (root == MM_INVALID_PHYS)
        return MM_INVALID_PHYS;
    UINT64 *dst = table(root);
    UINT64 *src = table(parent);
    memset(dst, 0, PAGE_SIZE);

    /* PML4 slots 256..511 are the shared higher-half kernel. */
    for (UINT32 i = PT_ENTRIES / 2; i < PT_ENTRIES; i++)
        dst[i] = src[i];

    /* KUSER_SHARED_DATA is read-only user memory shared by all processes. */
    UINT64 shared = MmGetPhysicalAddress(0x000000007ffe0000ULL);
    if (shared != MM_INVALID_PHYS &&
        !MmMapPageInAddressSpace(root, 0x000000007ffe0000ULL,
                                 shared & ~PAGE_MASK, PTE_USER))
        return MM_INVALID_PHYS;
    return root;
}

void MmDestroyAddressSpace(UINT64 root)
{
    root &= PTE_ADDR_MASK;
    if (!root || root == MmGetCurrentAddressSpace())
        return;

    UINT64 shared = MmGetPhysicalAddress(0x000000007ffe0000ULL);
    shared &= PTE_ADDR_MASK;
    UINT64 *pml4 = table(root);
    for (UINT32 i = 0; i < PT_ENTRIES / 2; i++) {
        if (!(pml4[i] & PTE_PRESENT))
            continue;
        UINT64 pdpt_phys = pml4[i] & PTE_ADDR_MASK;
        UINT64 *pdpt = table(pdpt_phys);
        for (UINT32 j = 0; j < PT_ENTRIES; j++) {
            if (!(pdpt[j] & PTE_PRESENT))
                continue;
            UINT64 pd_phys = pdpt[j] & PTE_ADDR_MASK;
            UINT64 *pd = table(pd_phys);
            for (UINT32 k = 0; k < PT_ENTRIES; k++) {
                if (!(pd[k] & PTE_PRESENT))
                    continue;
                if (pd[k] & PTE_LARGE)
                    continue; /* process mappings never use large pages */
                UINT64 pt_phys = pd[k] & PTE_ADDR_MASK;
                UINT64 *pt = table(pt_phys);
                for (UINT32 n = 0; n < PT_ENTRIES; n++) {
                    if (!(pt[n] & PTE_PRESENT))
                        continue;
                    UINT64 page = pt[n] & PTE_ADDR_MASK;
                    if (page != shared)
                        MmFreePages(page, 1);
                }
                MmFreePages(pt_phys, 1);
            }
            MmFreePages(pd_phys, 1);
        }
        MmFreePages(pdpt_phys, 1);
    }
    MmFreePages(root, 1);
}
