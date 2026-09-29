/* Memory, heap, critical-section and SRW-lock runtime support.
 * Included by user/ntdll/Ntdllrtl.c. */
/* ------------------------------------------------------------------ */
/* Memory functions (real binaries import these from ntdll)            */
/* ------------------------------------------------------------------ */

void *memset(void *dst, int v, SIZE_T n)
{
    BYTE *p = (BYTE *)dst;
    while (n--)
        *p++ = (BYTE)v;
    return dst;
}

void *memcpy(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, SIZE_T n)
{
    const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return (int)*x - (int)*y;
    return 0;
}

/* ------------------------------------------------------------------ */
/* The Rtl heap: a coalescing free-list allocator served from          */
/* NtAllocateVirtualMemory blocks. Every heap handle maps to the same  */
/* process-wide heap, matching the single-process model.               */
/* ------------------------------------------------------------------ */

typedef struct _RTL_BLOCK {
    SIZE_T Size;                 /* payload bytes */
    struct _RTL_BLOCK *Next;     /* free-list link when not in use */
} RTL_BLOCK;

#define RTL_HEAP_MAGIC 0xFFEEFFEEULL

typedef struct _RTL_HEAP {
    ULONGLONG Magic;
    RTL_BLOCK *FreeHead;
    BYTE *Cursor;                /* bump cursor inside the current segment */
    BYTE *SegmentEnd;
} RTL_HEAP;

static RTL_HEAP g_process_heap;
static LONG g_heap_lock;

static void heap_lock(void)
{
    while (__sync_lock_test_and_set(&g_heap_lock, 1))
        ; /* brief busy wait: allocations never hold the lock long */
}

static void heap_unlock(void)
{
    __sync_lock_release(&g_heap_lock);
}

static BOOLEAN heap_grow(RTL_HEAP *heap, SIZE_T bytes)
{
    void *base = 0;
    ULONGLONG size = (bytes + 0xFFFFULL) & ~0xFFFFULL;
    if (size < 0x10000)
        size = 0x10000;
    if (!NT_SUCCESS(NtAllocateVirtualMemory((HANDLE)-1LL, &base, 0, &size,
                                            0x3000 /* MEM_COMMIT|RESERVE */,
                                            0x04 /* PAGE_READWRITE */)))
        return FALSE;
    heap->Cursor = (BYTE *)base;
    heap->SegmentEnd = (BYTE *)base + size;
    return TRUE;
}

HANDLE RtlCreateHeap(ULONG flags, void *base, SIZE_T reserve, SIZE_T commit,
                     void *lock, void *params)
{
    (void)flags; (void)base; (void)reserve; (void)commit; (void)lock;
    (void)params;
    if (!g_process_heap.Magic) {
        g_process_heap.Magic = RTL_HEAP_MAGIC;
        g_process_heap.FreeHead = 0;
        g_process_heap.Cursor = 0;
        g_process_heap.SegmentEnd = 0;
    }
    return (HANDLE)&g_process_heap;
}

HANDLE RtlDestroyHeap(HANDLE heap)
{
    (void)heap;
    return 0;
}

void *RtlAllocateHeap(HANDLE heap, ULONG flags, SIZE_T size)
{
    (void)heap;
    if (!size)
        size = 1;
    size = (size + 15UL) & ~15UL;

    heap_lock();
    RTL_HEAP *h = &g_process_heap;
    if (!h->Magic) {
        h->Magic = RTL_HEAP_MAGIC;
        h->FreeHead = 0;
        h->Cursor = 0;
        h->SegmentEnd = 0;
    }

    /* Best fit from the free list (first fit keeps it simple). */
    RTL_BLOCK **link = &h->FreeHead;
    for (RTL_BLOCK *b = h->FreeHead; b; b = b->Next) {
        if (b->Size >= size) {
            *link = b->Next;
            heap_unlock();
            if (flags & 0x8 /* HEAP_ZERO_MEMORY */)
                memset(b + 1, 0, size);
            return b + 1;
        }
        link = &b->Next;
    }

    if ((SIZE_T)(h->SegmentEnd - h->Cursor) < size + sizeof(RTL_BLOCK)) {
        if (!heap_grow(h, size + sizeof(RTL_BLOCK))) {
            heap_unlock();
            return 0;
        }
    }
    RTL_BLOCK *block = (RTL_BLOCK *)h->Cursor;
    h->Cursor += sizeof(RTL_BLOCK) + size;
    block->Size = size;
    heap_unlock();
    if (flags & 0x8)
        memset(block + 1, 0, size);
    return block + 1;
}

BOOL RtlFreeHeap(HANDLE heap, ULONG flags, void *ptr)
{
    (void)heap; (void)flags;
    if (!ptr)
        return 1;
    RTL_BLOCK *block = (RTL_BLOCK *)ptr - 1;
    heap_lock();
    RTL_HEAP *h = &g_process_heap;
    block->Next = h->FreeHead;
    h->FreeHead = block;
    heap_unlock();
    return 1;
}

void *RtlReAllocateHeap(HANDLE heap, ULONG flags, void *ptr, SIZE_T size)
{
    if (!ptr)
        return 0;
    RTL_BLOCK *old = (RTL_BLOCK *)ptr - 1;
    void *fresh = RtlAllocateHeap(heap, flags, size);
    if (!fresh)
        return 0;
    memcpy(fresh, ptr, old->Size < size ? old->Size : size);
    RtlFreeHeap(heap, flags, ptr);
    return fresh;
}

SIZE_T RtlSizeHeap(HANDLE heap, ULONG flags, void *ptr)
{
    (void)heap; (void)flags;
    return ptr ? ((RTL_BLOCK *)ptr - 1)->Size : 0;
}

void RtlFlushHeaps(void)
{
}

BOOLEAN RtlValidateHeap(HANDLE heap, ULONG flags, void *ptr)
{
    (void)heap; (void)flags; (void)ptr;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Critical sections: recursive spin locks keyed by the thread id.     */
/* ------------------------------------------------------------------ */

#include "CriticalSections.inc"

/* ------------------------------------------------------------------ */
/* SRW locks: exclusive bit + shared counter in one pointer.           */
/* ------------------------------------------------------------------ */

void RtlInitializeSRWLock(void *srwlock)
{
    *(ULONGLONG *)srwlock = 0;
}

void RtlAcquireSRWLockExclusive(void *srwlock)
{
    while (__sync_lock_test_and_set((unsigned int *)srwlock, 1))
        __asm__ volatile("pause");
}

void RtlReleaseSRWLockExclusive(void *srwlock)
{
    __sync_lock_release((unsigned int *)srwlock);
}

void RtlAcquireSRWLockShared(void *srwlock)
{
    unsigned int v;
    for (;;) {
        v = *(volatile unsigned int *)srwlock;
        if (!(v & 1u) &&
            __sync_bool_compare_and_swap((unsigned int *)srwlock,
                                         v, v + 2))
            return;
        __asm__ volatile("pause");
    }
}

void RtlReleaseSRWLockShared(void *srwlock)
{
    __sync_fetch_and_sub((unsigned int *)srwlock, 2);
}

/* Slim resources map onto the same primitives. */
void RtlInitializeResource(void *r)      { memset(r, 0, 24); }
void RtlDeleteResource(void *r)          { (void)r; }
void RtlAcquireResourceExclusive(void *r, BOOLEAN wait)
{ (void)wait; RtlAcquireSRWLockExclusive(r); }
void RtlReleaseResource(void *r)         { RtlReleaseSRWLockExclusive(r); }
ULONG RtlQueryResourcePolicy(void *r)    { (void)r; return 0; }
