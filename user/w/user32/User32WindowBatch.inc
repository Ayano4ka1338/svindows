/* Defer window operations until the caller commits the batch. Each operation
 * goes through SetWindowPos so win32k owns the geometry and notifications. */
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(HANDLE heap, DWORD flags, ULONG_PTR size);
__declspec(dllimport) void *HeapReAlloc(HANDLE heap, DWORD flags, void *ptr, ULONG_PTR size);
__declspec(dllimport) ULONG_PTR HeapSize(HANDLE heap, DWORD flags, const void *ptr);
__declspec(dllimport) BOOL HeapFree(HANDLE heap, DWORD flags, void *ptr);

typedef struct {
    HWND Window, After;
    int X, Y, Width, Height;
    UINT Flags;
} SV_WINDOW_MOVE;

#define SV_WINDOW_BATCH_TAG 0x42505744u

typedef struct {
    UINT Tag, Capacity, Count;
    HWND Parent;
    BOOL Open;
    SV_WINDOW_MOVE Moves[];
} SV_WINDOW_BATCH;

static ULONG_PTR batch_bytes(UINT capacity)
{
    return sizeof(SV_WINDOW_BATCH) + (ULONG_PTR)capacity * sizeof(SV_WINDOW_MOVE);
}

static SV_WINDOW_BATCH *open_window_batch(HANDLE handle)
{
    if (!handle) return 0;
    ULONG_PTR size = HeapSize(GetProcessHeap(), 0, handle);
    if (size == (ULONG_PTR)-1 || size < sizeof(SV_WINDOW_BATCH)) return 0;
    SV_WINDOW_BATCH *batch = handle;
    if (batch->Tag != SV_WINDOW_BATCH_TAG || !batch->Open ||
        batch->Count > batch->Capacity || batch_bytes(batch->Capacity) > size)
        return 0;
    return batch;
}

USER32_API HANDLE BeginDeferWindowPos(int count)
{
    if (count < 0) return 0;
    UINT capacity = count ? (UINT)count : 8;
    SV_WINDOW_BATCH *batch = HeapAlloc(GetProcessHeap(), 0, batch_bytes(capacity));
    if (!batch) return 0;
    batch->Tag = SV_WINDOW_BATCH_TAG;
    batch->Capacity = capacity;
    batch->Count = 0;
    batch->Parent = 0;
    batch->Open = TRUE;
    return batch;
}

USER32_API HANDLE DeferWindowPos(HANDLE handle, HWND hwnd, HWND after,
    int x, int y, int width, int height, UINT flags)
{
    SV_WINDOW_BATCH *batch = open_window_batch(handle);
    if (!batch) return 0;
    RECT rect;
    if (!hwnd || !GetWindowRect(hwnd, &rect)) {
        /* A rejected window does not consume the open batch. */
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return 0;
    }
    HWND parent = GetAncestor(hwnd, 1 /* GA_PARENT */);
    if (batch->Count && parent != batch->Parent) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    if (batch->Count == batch->Capacity) {
        UINT capacity = batch->Capacity < 0x40000000u ? batch->Capacity * 2 : 0;
        SV_WINDOW_BATCH *grown = capacity ?
            HeapReAlloc(GetProcessHeap(), 0, batch, batch_bytes(capacity)) : 0;
        if (!grown) {
            HeapFree(GetProcessHeap(), 0, batch);
            return 0;
        }
        batch = grown;
        batch->Capacity = capacity;
    }
    batch->Parent = parent;
    batch->Moves[batch->Count++] = (SV_WINDOW_MOVE){hwnd, after, x, y, width, height, flags};
    return batch;
}

USER32_API BOOL EndDeferWindowPos(HANDLE handle)
{
    SV_WINDOW_BATCH *batch = open_window_batch(handle);
    if (!batch) return FALSE;
    batch->Open = FALSE;

    BOOL ok = TRUE;
    for (UINT i = 0; ok && i < batch->Count; i++) {
        RECT rect;
        HWND hwnd = batch->Moves[i].Window;
        ok = GetWindowRect(hwnd, &rect) &&
             GetAncestor(hwnd, 1 /* GA_PARENT */) == batch->Parent;
    }
    for (UINT i = 0; ok && i < batch->Count; i++) {
        const SV_WINDOW_MOVE *move = &batch->Moves[i];
        ok = SetWindowPos(move->Window, move->After, move->X, move->Y,
                          move->Width, move->Height, move->Flags);
    }
    HeapFree(GetProcessHeap(), 0, batch);
    return ok;
}
