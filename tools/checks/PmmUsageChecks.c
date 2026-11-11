#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <svindows/Mm.h>
#include <svindows/Ke.h>
UINT64 MmDirectMapBase;
UINT64 KiAcquireRawSpinLock(PKSPIN_LOCK lock){(void)lock;return 0;}
void KiReleaseRawSpinLock(PKSPIN_LOCK lock,UINT64 f){(void)lock;(void)f;}
#include "../../kernel/mm/Pmm.c"
int main(void)
{
    unsigned char *bitmap=malloc(512);assert(bitmap);MmDirectMapBase=(UINT64)bitmap;
    g_pmm=(PMM_STATE){.total_pages=4096,.used_pages=4096};memset(bitmap,255,512);
    free_range(0,4096*PAGE_SIZE);reserve_range(0,256*PAGE_SIZE);
    MM_MEMORY_USAGE u;MmQueryMemoryUsage(&u);
    assert(u.CommitLimit==3840&&u.AvailablePages==3840&&!u.CommittedPages&&!u.PeakCommitment);
    UINT64 a=MmAllocatePage(),b=MmAllocatePages(3),c=MmAllocatePageBelow4G();
    UINT64 d=MmAllocatePagesBelow4G(4),e=MmAllocatePagesBelow16M(2);
    assert(a!=MM_INVALID_PHYS&&b!=MM_INVALID_PHYS&&c!=MM_INVALID_PHYS&&d!=MM_INVALID_PHYS&&e!=MM_INVALID_PHYS);
    MmQueryMemoryUsage(&u);assert(u.CommittedPages==11&&u.PeakCommitment==11&&u.AvailablePages==3829);
    MmFreePages(b,3);MmQueryMemoryUsage(&u);assert(u.CommittedPages==8&&u.PeakCommitment==11);
    assert(MmAllocatePages(5000)==MM_INVALID_PHYS);MmQueryMemoryUsage(&u);assert(u.PeakCommitment==11);
    MmFreePages(a,1);MmFreePages(c,1);MmFreePages(d,4);MmFreePages(e,2);
    MmQueryMemoryUsage(&u);assert(!u.CommittedPages&&u.PeakCommitment==11&&u.AvailablePages==u.CommitLimit);
    reserve_range(256*PAGE_SIZE,8*PAGE_SIZE);MmQueryMemoryUsage(&u);
    assert(!u.CommittedPages&&u.AvailablePages==3832&&u.CommitLimit==3832);
    free(bitmap);puts("PASS real frame allocator usage, all allocation paths, peak retention and permanent reservation exclusion");
}
