/* Process heap and KUSER_SHARED_DATA time APIs.
 * Included by the owning translation unit. */
/* ------------------------------------------------------------------ */
/* Process heap: a first-fit free list over NtAllocateVirtualMemory   */
/* ------------------------------------------------------------------ */

/* Every allocation is preceded by one of these headers; blocks form a single
 * address-ordered list spanning the heap region, which makes coalescing easy. */
typedef struct _BLOCK {
    ULONGLONG      size; /* payload bytes */
    struct _BLOCK *next; /* next block by address (NULL at the end)  */
    int            free;
    int            pad;
} BLOCK;

typedef struct _HEAP {
    BLOCK *first;
} HEAP;

static HEAP *g_process_heap;

static ULONGLONG align_up8(ULONGLONG n) { return (n + 7) & ~7ULL; }

static BLOCK *heap_grow(HEAP *heap, ULONGLONG wanted)
{
    ULONGLONG payload = wanted < 0x10000 ? 0x10000 : align_up8(wanted);
    BLOCK *block = (BLOCK *)nt_alloc(sizeof(BLOCK) + payload);
    if (!block)
        return 0;
    block->size = payload;
    block->next = 0;
    block->free = 1;
    block->pad = 0;

    BLOCK *last = heap->first;
    while (last->next)
        last = last->next;
    last->next = block;
    return block;
}

__declspec(dllexport) HANDLE HeapCreate(DWORD flags, SIZE_T initial, SIZE_T max)
{
    (void)flags; (void)max;
    ULONGLONG region = initial ? align_up8(initial) : 0x10000; /* >=64 KiB */
    region += sizeof(HEAP) + sizeof(BLOCK);

    void *mem = nt_alloc(region);
    if (!mem)
        return 0;

    HEAP *h = (HEAP *)mem;
    BLOCK *b = (BLOCK *)(h + 1);
    b->size = region - sizeof(HEAP) - sizeof(BLOCK);
    b->next = 0;
    b->free = 1;
    h->first = b;
    return (HANDLE)h;
}

__declspec(dllexport) HANDLE GetProcessHeap(void)
{
    if (!g_process_heap) {
        g_process_heap = (HEAP *)HeapCreate(0, 0x10000, 0);
        NtCurrentPeb()->ProcessHeap = g_process_heap;
    }
    return (HANDLE)g_process_heap;
}

__declspec(dllexport) LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    HEAP *h = (HEAP *)heap;
    if (!h)
        return 0;
    ULONGLONG size = align_up8(bytes ? bytes : 1);

    for (BLOCK *b = h->first; b; b = b->next) {
        if (!b->free || b->size < size)
            continue;

        /* Split off the remainder if there's room for another block. */
        if (b->size >= size + sizeof(BLOCK) + 16) {
            BLOCK *nb = (BLOCK *)((char *)(b + 1) + size);
            nb->size = b->size - size - sizeof(BLOCK);
            nb->next = b->next;
            nb->free = 1;
            b->next = nb;
            b->size = size;
        }
        b->free = 0;

        void *p = (void *)(b + 1);
        if (flags & HEAP_ZERO_MEMORY)
            memset(p, 0, size);
        return p;
    }
    /* Windows process heaps grow on demand.  Keep segments in the same block
     * list; they need not be virtually adjacent. */
    BLOCK *b = heap_grow(h, size);
    if (!b)
        return 0;
    if (b->size >= size + sizeof(BLOCK) + 16) {
        BLOCK *tail = (BLOCK *)((char *)(b + 1) + size);
        tail->size = b->size - size - sizeof(BLOCK);
        tail->next = b->next;
        tail->free = 1;
        tail->pad = 0;
        b->next = tail;
        b->size = size;
    }
    b->free = 0;
    if (flags & HEAP_ZERO_MEMORY)
        memset(b + 1, 0, size);
    return b + 1;
}

__declspec(dllexport) BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID ptr)
{
    (void)flags;
    HEAP *h = (HEAP *)heap;
    if (!h || !ptr)
        return 1;

    BLOCK *b = (BLOCK *)ptr - 1;
    b->free = 1;

    /* Coalesce adjacent free blocks in one forward pass. */
    for (BLOCK *c = h->first; c && c->next; ) {
        BOOL adjacent = (char *)(c + 1) + c->size == (char *)c->next;
        if (c->free && c->next->free && adjacent) {
            c->size += sizeof(BLOCK) + c->next->size;
            c->next = c->next->next;
        } else {
            c = c->next;
        }
    }
    return 1;
}

static BLOCK *heap_find_block(HEAP *heap, const void *ptr)
{
    if (!heap || !ptr)
        return 0;
    for (BLOCK *b = heap->first; b; b = b->next)
        if ((const void *)(b + 1) == ptr)
            return b;
    return 0;
}

__declspec(dllexport) SIZE_T HeapSize(HANDLE heap, DWORD flags,
                                      const void *ptr)
{
    (void)flags;
    BLOCK *b = heap_find_block((HEAP *)heap, ptr);
    if (!b || b->free) {
        SetLastError(6); /* ERROR_INVALID_HANDLE / invalid heap block */
        return (SIZE_T)-1;
    }
    return (SIZE_T)b->size;
}

__declspec(dllexport) BOOL HeapValidate(HANDLE heap, DWORD flags,
                                        const void *ptr)
{
    (void)flags;
    if (!heap)
        return 0;
    if (!ptr)
        return 1;
    BLOCK *b = heap_find_block((HEAP *)heap, ptr);
    return b && !b->free;
}

__declspec(dllexport) LPVOID HeapReAlloc(HANDLE heap, DWORD flags,
                                         LPVOID ptr, SIZE_T bytes)
{
    if (!ptr)
        return HeapAlloc(heap, flags, bytes);
    if (!bytes) {
        HeapFree(heap, 0, ptr);
        return 0;
    }

    HEAP *h = (HEAP *)heap;
    BLOCK *b = heap_find_block(h, ptr);
    if (!b || b->free) {
        SetLastError(6);
        return 0;
    }
    ULONGLONG wanted = align_up8(bytes);
    ULONGLONG old_size = b->size;
    if (wanted <= old_size) {
        if (old_size >= wanted + sizeof(BLOCK) + 16) {
            BLOCK *tail = (BLOCK *)((char *)(b + 1) + wanted);
            tail->size = old_size - wanted - sizeof(BLOCK);
            tail->next = b->next;
            tail->free = 1;
            b->next = tail;
            b->size = wanted;
        }
        return ptr;
    }

    if (b->next && b->next->free &&
        old_size + sizeof(BLOCK) + b->next->size >= wanted) {
        b->size += sizeof(BLOCK) + b->next->size;
        b->next = b->next->next;
        if (flags & HEAP_ZERO_MEMORY)
            memset((char *)ptr + old_size, 0, wanted - old_size);
        return ptr;
    }

    LPVOID replacement = HeapAlloc(heap, flags, bytes);
    if (!replacement)
        return 0;
    memcpy(replacement, ptr, old_size < bytes ? old_size : bytes);
    HeapFree(heap, 0, ptr);
    return replacement;
}

__declspec(dllexport) BOOL HeapSetInformation(HANDLE heap, int info_class,
                                              LPVOID info, SIZE_T info_size)
{
    (void)heap; (void)info_class; (void)info; (void)info_size;
    return 1;
}

__declspec(dllexport) LPVOID LocalAlloc(DWORD flags, SIZE_T bytes)
{
    return HeapAlloc(GetProcessHeap(),
                     (flags & 0x0040) ? HEAP_ZERO_MEMORY : 0, bytes);
}

__declspec(dllexport) LPVOID LocalFree(LPVOID ptr)
{
    HeapFree(GetProcessHeap(), 0, ptr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Time: KUSER_SHARED_DATA (the read-only page at 0x7FFE0000)          */
/* ------------------------------------------------------------------ */

#define KUSD ((volatile unsigned char *)(ULONGLONG)0x7FFE0000ULL)

/* Read a KSYSTEM_TIME (Low, High1, High2) lock-free from the shared page. */
static ULONGLONG read_ksystem_time(unsigned off)
{
    for (;;) {
        int high1 = *(volatile int *)(KUSD + off + 0x4);
        unsigned low = *(volatile unsigned *)(KUSD + off + 0x0);
        int high2 = *(volatile int *)(KUSD + off + 0x8);
        if (high1 == high2)
            return ((ULONGLONG)(unsigned)high1 << 32) | low;
    }
}

__declspec(dllexport) ULONGLONG GetTickCount64(void)
{
    ULONGLONG ticks = read_ksystem_time(0x320);          /* TickCount     */
    unsigned mult = *(volatile unsigned *)(KUSD + 0x004); /* Multiplier    */
    return (ticks * mult) >> 24;                          /* -> milliseconds */
}

__declspec(dllexport) DWORD GetTickCount(void)
{
    return (DWORD)GetTickCount64();
}

__declspec(dllexport) void GetSystemTimeAsFileTime(void *lpFileTime)
{
    ULONGLONG t = read_ksystem_time(0x014); /* SystemTime, 100 ns units */
    if (lpFileTime) {
        ((DWORD *)lpFileTime)[0] = (DWORD)t;
        ((DWORD *)lpFileTime)[1] = (DWORD)(t >> 32);
    }
}

__declspec(dllexport) BOOL FileTimeToSystemTime(const FILETIME *file_time,
                                                SYSTEMTIME *system_time)
{
    static const BYTE month_days[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!file_time || !system_time) {
        SetLastError(87);
        return 0;
    }
    ULONGLONG ticks = ((ULONGLONG)file_time->High << 32) | file_time->Low;
    ULONGLONG total_seconds = ticks / 10000000ULL;
    ULONGLONG days = total_seconds / 86400ULL;
    DWORD seconds = (DWORD)(total_seconds % 86400ULL);
    DWORD day_of_week = (DWORD)((days + 1) % 7); /* 1601-01-01 was Monday */

    DWORD year = 1601;
    while (year < 30828) {
        DWORD count = leap_year(year) ? 366 : 365;
        if (days < count) break;
        days -= count;
        year++;
    }
    if (year == 30828) {
        SetLastError(87);
        return 0;
    }
    DWORD month = 1;
    while (month <= 12) {
        DWORD count = month_days[month - 1];
        if (month == 2 && leap_year(year)) count++;
        if (days < count) break;
        days -= count;
        month++;
    }
    memset(system_time, 0, sizeof(*system_time));
    system_time->Year = (WORD)year;
    system_time->Month = (WORD)month;
    system_time->Day = (WORD)(days + 1);
    system_time->DayOfWeek = (WORD)day_of_week;
    system_time->Hour = (WORD)(seconds / 3600);
    system_time->Minute = (WORD)((seconds / 60) % 60);
    system_time->Second = (WORD)(seconds % 60);
    system_time->Milliseconds = (WORD)((ticks % 10000000ULL) / 10000ULL);
    return 1;
}

__declspec(dllexport) BOOL FileTimeToLocalFileTime(const FILETIME *utc,
                                                    FILETIME *local)
{
    if (!utc || !local) {
        SetLastError(87);
        return 0;
    }
    /* SVINDOWS has no configured time-zone database yet, so local time is UTC. */
    *local = *utc;
    return 1;
}

static void put_dec_w(WCHAR *out, DWORD value, DWORD digits)
{
    while (digits) {
        out[--digits] = (WCHAR)('0' + value % 10);
        value /= 10;
    }
}

static int copy_formatted_time(const WCHAR *value, int length,
                               WCHAR *out, int capacity)
{
    int required = length + 1;
    if (!out || capacity == 0)
        return required;
    if (capacity < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    for (int i = 0; i <= length; i++) out[i] = value[i];
    return required;
}

__declspec(dllexport) int GetDateFormatW(DWORD locale, DWORD flags,
                                         const SYSTEMTIME *time,
                                         const WCHAR *format, WCHAR *out,
                                         int capacity)
{
    WCHAR value[11];
    (void)locale; (void)flags; (void)format;
    if (!time) {
        SetLastError(87);
        return 0;
    }
    put_dec_w(value, time->Year, 4);
    value[4] = '-';
    put_dec_w(value + 5, time->Month, 2);
    value[7] = '-';
    put_dec_w(value + 8, time->Day, 2);
    value[10] = 0;
    return copy_formatted_time(value, 10, out, capacity);
}

__declspec(dllexport) int GetTimeFormatW(DWORD locale, DWORD flags,
                                         const SYSTEMTIME *time,
                                         const WCHAR *format, WCHAR *out,
                                         int capacity)
{
    WCHAR value[9];
    (void)locale; (void)flags; (void)format;
    if (!time) {
        SetLastError(87);
        return 0;
    }
    put_dec_w(value, time->Hour, 2);
    value[2] = ':';
    put_dec_w(value + 3, time->Minute, 2);
    value[5] = ':';
    put_dec_w(value + 6, time->Second, 2);
    value[8] = 0;
    return copy_formatted_time(value, 8, out, capacity);
}

__declspec(dllexport) void Sleep(DWORD ms)
{
    /* Relative delay: negative 100 ns units. */
    long long interval = -(long long)ms * 10000;
    NtDelayExecution(0, &interval);
}

__declspec(dllexport) DWORD SleepEx(DWORD ms, BOOL alertable)
{
    (void)alertable;
    Sleep(ms);
    return 0;
}

