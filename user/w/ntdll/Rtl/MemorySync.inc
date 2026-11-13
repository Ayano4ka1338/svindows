/* Memory, heap, critical-section and SRW-lock runtime support.
 * Included by user/w/ntdll/Ntdllrtl.c. */
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

SIZE_T RtlCompareMemory(const void *a, const void *b, SIZE_T length)
{
    const BYTE *left = (const BYTE *)a, *right = (const BYTE *)b;
    SIZE_T equal = 0;
    while (equal < length && left[equal] == right[equal]) equal++;
    return equal;
}

#include "CriticalSections.inc"
#include "Heap.inc"

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
