/* Global memory objects. Movable handles retain their identity across resize
 * and discard; fixed allocations remain ordinary process-heap pointers. */
#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define GMEM_MOVEABLE 0x0002u
#define GMEM_ZEROINIT 0x0040u
#define GMEM_MODIFY 0x0080u
#define GMEM_DISCARDABLE 0x0100u
#define GMEM_DISCARDED 0x4000u
#define GMEM_INVALID_HANDLE 0x8000u
#define GLOBAL_HANDLE_TAG 4ull
#define GLOBAL_MOVE_MAGIC 0x474c4f42u
typedef unsigned int UINT;

typedef struct _GLOBAL_MOVEABLE {
    struct _GLOBAL_MOVEABLE *Next;
    void *Data;
    SIZE_T Size;
    DWORD Magic, Locks, Flags;
} GLOBAL_MOVEABLE;

__declspec(dllexport) void *GlobalAlloc(UINT, SIZE_T);
__declspec(dllexport) void *GlobalFree(void *);
__declspec(dllexport) SIZE_T GlobalSize(void *);
__declspec(dllexport) void *GlobalLock(void *);
__declspec(dllexport) BOOL GlobalUnlock(void *);
__declspec(dllexport) UINT GlobalFlags(void *);
__declspec(dllexport) void *GlobalHandle(const void *);
__declspec(dllexport) void *GlobalReAlloc(void *, SIZE_T, UINT);
__declspec(dllexport) void *GlobalDiscard(void *);

static GLOBAL_MOVEABLE *g_global_memory;

static GLOBAL_MOVEABLE *global_moveable(void *handle)
{
    ULONGLONG value = (ULONGLONG)handle;
    if ((value & 7) != GLOBAL_HANDLE_TAG) return NULL;
    GLOBAL_MOVEABLE *block = (GLOBAL_MOVEABLE *)(value & ~7ull);
    for (GLOBAL_MOVEABLE *entry = g_global_memory; entry; entry = entry->Next)
        if (entry == block && entry->Magic == GLOBAL_MOVE_MAGIC) return entry;
    return NULL;
}

static void *global_impl_Alloc(UINT flags, SIZE_T bytes)
{
    HANDLE heap = GetProcessHeap();
    DWORD allocation_flags = flags & GMEM_ZEROINIT ? HEAP_ZERO_MEMORY : 0;
    if (!(flags & GMEM_MOVEABLE))
        return HeapAlloc(heap, allocation_flags, bytes);
    GLOBAL_MOVEABLE *block = HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(*block));
    if (!block) { SetLastError(8); return NULL; }
    block->Data = bytes ? HeapAlloc(heap, allocation_flags, bytes) : NULL;
    if (bytes && !block->Data) {
        HeapFree(heap, 0, block);
        SetLastError(8);
        return NULL;
    }
    block->Magic = GLOBAL_MOVE_MAGIC;
    block->Flags = flags & (GMEM_MOVEABLE | GMEM_DISCARDABLE);
    block->Size = bytes;
    block->Next = g_global_memory;
    g_global_memory = block;
    return (void *)((ULONGLONG)block | GLOBAL_HANDLE_TAG);
}

static void *global_impl_Free(void *handle)
{
    if (!handle) return NULL;
    HANDLE heap = GetProcessHeap();
    GLOBAL_MOVEABLE *block = global_moveable(handle);
    if (!block) {
        if (HeapFree(heap, 0, handle)) return NULL;
        SetLastError(6);
        return handle;
    }
    GLOBAL_MOVEABLE **link = &g_global_memory;
    while (*link && *link != block) link = &(*link)->Next;
    *link = block->Next;
    if (block->Data) HeapFree(heap, 0, block->Data);
    block->Magic = 0;
    HeapFree(heap, 0, block);
    return NULL;
}

static SIZE_T global_impl_Size(void *handle)
{
    GLOBAL_MOVEABLE *block = global_moveable(handle);
    if (block) return block->Size;
    SIZE_T bytes = handle ? HeapSize(GetProcessHeap(), 0, handle) : (SIZE_T)-1;
    if (bytes == (SIZE_T)-1) { SetLastError(6); return 0; }
    return bytes;
}

static void *global_impl_Lock(void *handle)
{
    GLOBAL_MOVEABLE *block = global_moveable(handle);
    if (block) {
        if (!block->Data) { SetLastError(157); return NULL; }
        if (block->Locks == 0xffffffffUL) { SetLastError(212); return NULL; }
        block->Locks++;
        return block->Data;
    }
    if (handle && HeapValidate(GetProcessHeap(), 0, handle)) return handle;
    SetLastError(6);
    return NULL;
}

static BOOL global_impl_Unlock(void *handle)
{
    GLOBAL_MOVEABLE *block = global_moveable(handle);
    if (block && block->Locks) {
        if (--block->Locks) return TRUE;
        SetLastError(0);
        return FALSE;
    }
    if (!block && handle && HeapValidate(GetProcessHeap(), 0, handle)) return TRUE;
    SetLastError(block ? 158 : 6);
    return FALSE;
}

static UINT global_impl_Flags(void *handle)
{
    GLOBAL_MOVEABLE *block = global_moveable(handle);
    if (block)
        return (block->Locks > 255 ? 255 : block->Locks) | (block->Flags & GMEM_DISCARDABLE) |
               (!block->Data ? GMEM_DISCARDED : 0);
    if (handle && HeapValidate(GetProcessHeap(), 0, handle)) return 0;
    SetLastError(6);
    return GMEM_INVALID_HANDLE;
}

static void *global_impl_Handle(const void *data)
{
    for (GLOBAL_MOVEABLE *block = g_global_memory; block; block = block->Next)
        if (data && (block->Data == data ||
            (const void *)((ULONGLONG)block | GLOBAL_HANDLE_TAG) == data))
            return (void *)((ULONGLONG)block | GLOBAL_HANDLE_TAG);
    if (data && HeapValidate(GetProcessHeap(), 0, data)) return (void *)data;
    SetLastError(6);
    return NULL;
}

static void *global_impl_ReAlloc(void *handle, SIZE_T bytes, UINT flags)
{
    if (!handle) { SetLastError(6); return NULL; }
    GLOBAL_MOVEABLE *block = global_moveable(handle);
    HANDLE heap = GetProcessHeap();
    if (flags & GMEM_MODIFY) {
        if (block) {
            block->Flags = GMEM_MOVEABLE | (flags & GMEM_DISCARDABLE);
            return handle;
        }
        if (!HeapValidate(heap, 0, handle)) { SetLastError(6); return NULL; }
        if (!(flags & GMEM_MOVEABLE)) return handle;
        block = HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(*block));
        if (!block) { SetLastError(8); return NULL; }
        block->Data = handle;
        block->Size = HeapSize(heap, 0, handle);
        block->Magic = GLOBAL_MOVE_MAGIC;
        block->Flags = GMEM_MOVEABLE | (flags & GMEM_DISCARDABLE);
        block->Next = g_global_memory;
        g_global_memory = block;
        return (void *)((ULONGLONG)block | GLOBAL_HANDLE_TAG);
    }
    DWORD allocation_flags = flags & GMEM_ZEROINIT ? HEAP_ZERO_MEMORY : 0;
    if (!block) return HeapReAlloc(heap, allocation_flags, handle, bytes);
    if (block->Locks && !bytes) {
        SetLastError(212);
        return NULL;
    }
    if (!bytes) {
        if (block->Data) HeapFree(heap, 0, block->Data);
        block->Data = NULL;
        block->Size = 0;
        return handle;
    }
    if (block->Locks) allocation_flags |= 0x10u; /* preserve every locked data pointer */
    void *data = block->Data ? HeapReAlloc(heap, allocation_flags, block->Data, bytes)
                            : HeapAlloc(heap, allocation_flags, bytes);
    if (!data) { SetLastError(8); return NULL; }
    block->Data = data;
    block->Size = bytes;
    return handle;
}

static void *global_impl_Discard(void *handle)
{
    return GlobalReAlloc(handle, 0, GMEM_MOVEABLE);
}

__declspec(dllexport) void *GlobalAlloc(UINT flags, SIZE_T bytes)
{
    HANDLE heap=GetProcessHeap();
    if (!heap || !HeapLock(heap)) return NULL;
    void *result=global_impl_Alloc(flags, bytes);
    HeapUnlock(heap);
    return result;
}

__declspec(dllexport) void *GlobalFree(void *handle)
{
    if (!handle) return NULL;
    HANDLE heap=GetProcessHeap();
    if (!heap || !HeapLock(heap)) return handle;
    void *result=global_impl_Free(handle);
    HeapUnlock(heap);
    return result;
}

__declspec(dllexport) SIZE_T GlobalSize(void *handle)
{
    if (!handle) { SetLastError(6); return 0; }
    HANDLE heap=GetProcessHeap();
    if (!heap || !HeapLock(heap)) return 0;
    SIZE_T result=global_impl_Size(handle);
    HeapUnlock(heap);
    return result;
}

__declspec(dllexport) void *GlobalLock(void *handle)
{
    if (!handle) { SetLastError(6); return NULL; }
    HANDLE heap=GetProcessHeap();
    if (!heap || !HeapLock(heap)) return NULL;
    void *result=global_impl_Lock(handle);
    HeapUnlock(heap);
    return result;
}

__declspec(dllexport) BOOL GlobalUnlock(void *handle)
{
    if (!handle) { SetLastError(6); return FALSE; }
    HANDLE heap=GetProcessHeap();
    if (!heap || !HeapLock(heap)) return FALSE;
    BOOL result=global_impl_Unlock(handle);
    HeapUnlock(heap);
    return result;
}

__declspec(dllexport) UINT GlobalFlags(void *handle)
{
    if (!handle) { SetLastError(6); return GMEM_INVALID_HANDLE; }
    HANDLE heap=GetProcessHeap();
    if (!heap || !HeapLock(heap)) return GMEM_INVALID_HANDLE;
    UINT result=global_impl_Flags(handle);
    HeapUnlock(heap);
    return result;
}

__declspec(dllexport) void *GlobalHandle(const void *data)
{
    if (!data) { SetLastError(6); return NULL; }
    HANDLE heap=GetProcessHeap();
    if (!heap || !HeapLock(heap)) return NULL;
    void *result=global_impl_Handle(data);
    HeapUnlock(heap);
    return result;
}

__declspec(dllexport) void *GlobalReAlloc(void *handle, SIZE_T bytes, UINT flags)
{
    if (!handle) { SetLastError(6); return NULL; }
    HANDLE heap=GetProcessHeap();
    if (!heap || !HeapLock(heap)) return NULL;
    void *result=global_impl_ReAlloc(handle, bytes, flags);
    HeapUnlock(heap);
    return result;
}

__declspec(dllexport) void *GlobalDiscard(void *handle)
{
    if (!handle) { SetLastError(6); return NULL; }
    HANDLE heap=GetProcessHeap();
    if (!heap || !HeapLock(heap)) return NULL;
    void *result=global_impl_Discard(handle);
    HeapUnlock(heap);
    return result;
}
