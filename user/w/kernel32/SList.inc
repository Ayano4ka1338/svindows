/* x64 SLIST_HEADER has a 16-byte representation. Header16 stores the depth
 * and sequence in Alignment, and the 16-byte-aligned head in Region; Header8
 * packs the head into Alignment. A process-wide lock serializes updates. */
typedef struct _SLIST_ENTRY_K { struct _SLIST_ENTRY_K *Next; } SLIST_ENTRY_K;
typedef struct __attribute__((aligned(16))) _SLIST_HEADER_K {
    volatile ULONGLONG Alignment, Region;
} SLIST_HEADER_K;
static volatile DWORD g_slist_lock;

static void slist_lock(void)
{
    while (__atomic_exchange_n(&g_slist_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static void slist_unlock(void)
{ __atomic_store_n(&g_slist_lock, 0, __ATOMIC_RELEASE); }

static SLIST_ENTRY_K *slist_first(const SLIST_HEADER_K *head)
{
    if (head->Region & 1)
        return (SLIST_ENTRY_K *)(ULONGLONG)(head->Region & ~15ULL);
    ULONGLONG pointer = ((head->Alignment >> 25) << 4) |
                        (((head->Region >> 3) & 0x1f) << 43);
    return (SLIST_ENTRY_K *)(ULONGLONG)pointer;
}

static void slist_store(SLIST_HEADER_K *head, SLIST_ENTRY_K *first,
                        unsigned depth)
{
    ULONGLONG address = (ULONGLONG)first;
    if (head->Region & 1 || (!head->Alignment && !head->Region)) {
        ULONGLONG sequence = (head->Alignment >> 16) + 1;
        head->Alignment = (sequence << 16) | (depth & 0xffffu);
        head->Region = (address & ~15ULL) | 3;
    } else {
        ULONGLONG sequence = ((head->Alignment >> 16) + 1) & 0x1ffu;
        head->Alignment = ((address >> 4) << 25) |
                          (sequence << 16) | (depth & 0xffffu);
        head->Region = (head->Region & ~0xf8ULL) |
                       (((address >> 43) & 0x1f) << 3) | 2;
    }
}

__declspec(dllexport) void InitializeSListHead(SLIST_HEADER_K *head)
{
    if (!head) return;
    slist_lock();
    head->Alignment = 0;
    head->Region = 3; /* Header16, initialized, empty */
    slist_unlock();
}

__declspec(dllexport) SLIST_ENTRY_K *InterlockedPushEntrySList(
    SLIST_HEADER_K *head, SLIST_ENTRY_K *entry)
{
    if (!head || !entry) return 0;
    slist_lock();
    SLIST_ENTRY_K *previous = slist_first(head);
    entry->Next = previous;
    slist_store(head, entry, (unsigned)(head->Alignment & 0xffffu) + 1);
    slist_unlock();
    return previous;
}

__declspec(dllexport) SLIST_ENTRY_K *InterlockedPopEntrySList(
    SLIST_HEADER_K *head)
{
    if (!head) return 0;
    slist_lock();
    SLIST_ENTRY_K *entry = slist_first(head);
    if (entry) {
        unsigned depth = (unsigned)(head->Alignment & 0xffffu);
        slist_store(head, entry->Next, depth ? depth - 1 : 0);
    }
    slist_unlock();
    return entry;
}

__declspec(dllexport) SLIST_ENTRY_K *InterlockedFlushSList(
    SLIST_HEADER_K *head)
{
    if (!head) return 0;
    slist_lock();
    SLIST_ENTRY_K *entry = slist_first(head);
    if (entry) slist_store(head, 0, 0);
    slist_unlock();
    return entry;
}

__declspec(dllexport) unsigned short QueryDepthSList(
    const SLIST_HEADER_K *head)
{
    if (!head) return 0;
    slist_lock();
    unsigned short depth = (unsigned short)head->Alignment;
    slist_unlock();
    return depth;
}
