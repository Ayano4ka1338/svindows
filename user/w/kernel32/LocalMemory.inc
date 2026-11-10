/* Fixed blocks are heap pointers. Movable blocks carry a tagged descriptor
 * so a successful resize can replace the data without changing the HLOCAL. */
#define LMEM_MOVEABLE 0x0002u
#define LMEM_ZEROINIT 0x0040u
#define LOCAL_HANDLE_TAG 2ull
#define LOCAL_MOVE_MAGIC 0x4c4f434du

typedef struct {
    void *Data;
    DWORD Magic, Locks;
} LOCAL_MOVEABLE;

static LOCAL_MOVEABLE *local_moveable(void *handle)
{
    ULONGLONG value = (ULONGLONG)handle;
    if ((value & 7) != LOCAL_HANDLE_TAG) return 0;
    LOCAL_MOVEABLE *block = (LOCAL_MOVEABLE *)(value & ~7ull);
    BLOCK *heap_block = heap_find_block(GetProcessHeap(), block);
    if (!heap_block || heap_block->free || heap_block->size < sizeof(*block) ||
        block->Magic != LOCAL_MOVE_MAGIC)
        return 0;
    return block;
}

__declspec(dllexport) void *LocalAlloc(DWORD flags, SIZE_T bytes)
{
    HANDLE heap = GetProcessHeap();
    DWORD heap_flags = flags & LMEM_ZEROINIT ? HEAP_ZERO_MEMORY : 0;
    if (!(flags & LMEM_MOVEABLE)) return HeapAlloc(heap, heap_flags, bytes);
    LOCAL_MOVEABLE *block = HeapAlloc(heap, 0, sizeof(*block));
    if (!block) return 0;
    block->Data = bytes ? HeapAlloc(heap, heap_flags, bytes) : 0;
    if (bytes && !block->Data) {
        HeapFree(heap, 0, block);
        return 0;
    }
    block->Magic = LOCAL_MOVE_MAGIC;
    block->Locks = 0;
    return (void *)((ULONGLONG)block | LOCAL_HANDLE_TAG);
}

__declspec(dllexport) void *LocalFree(void *handle)
{
    if (!handle) return 0;
    HANDLE heap = GetProcessHeap();
    LOCAL_MOVEABLE *block = local_moveable(handle);
    if (!block)
        return HeapFree(heap, 0, handle) ? 0 : handle;
    if (block->Data) HeapFree(heap, 0, block->Data);
    block->Magic = 0;
    HeapFree(heap, 0, block);
    return 0;
}

__declspec(dllexport) void *LocalLock(void *handle)
{
    LOCAL_MOVEABLE *block = local_moveable(handle);
    if (block) {
        if (!block->Data) {
            SetLastError(157); /* ERROR_DISCARDED */
            return 0;
        }
        if (block->Locks < 255) block->Locks++;
        return block->Data;
    }
    if (!handle || !HeapValidate(GetProcessHeap(), 0, handle)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }
    return handle;
}

__declspec(dllexport) BOOL LocalUnlock(void *handle)
{
    LOCAL_MOVEABLE *block = local_moveable(handle);
    if (block && block->Locks) {
        if (--block->Locks) return 1;
        SetLastError(0);
        return 0;
    }
    SetLastError(block || (handle && HeapValidate(GetProcessHeap(), 0, handle))
        ? 158 /* ERROR_NOT_LOCKED */ : 6 /* ERROR_INVALID_HANDLE */);
    return 0;
}

__declspec(dllexport) SIZE_T LocalSize(void *handle)
{
    LOCAL_MOVEABLE *block = local_moveable(handle);
    if (block && !block->Data) return 0;
    SIZE_T size = HeapSize(GetProcessHeap(), 0, block ? block->Data : handle);
    return size == (SIZE_T)-1 ? 0 : size;
}

__declspec(dllexport) void *LocalReAlloc(void *handle, SIZE_T bytes, DWORD flags)
{
    if (!handle) return 0;
    HANDLE heap = GetProcessHeap();
    LOCAL_MOVEABLE *block = local_moveable(handle);
    DWORD heap_flags = flags & LMEM_ZEROINIT ? HEAP_ZERO_MEMORY : 0;
    if (!block)
        return HeapReAlloc(heap, heap_flags, handle, bytes ? bytes : 1);
    if (block->Locks && (!bytes || bytes > LocalSize(handle))) {
        SetLastError(212); /* ERROR_LOCKED */
        return 0;
    }
    if (!bytes) {
        if (block->Data) HeapFree(heap, 0, block->Data);
        block->Data = 0;
        return handle;
    }
    void *data = HeapReAlloc(heap, heap_flags, block->Data, bytes);
    if (!data) return 0;
    block->Data = data;
    return handle;
}
