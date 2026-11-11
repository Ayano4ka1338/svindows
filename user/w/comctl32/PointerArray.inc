/* The count and pointer array are the public prefix read by the DPA macros. */
typedef struct {
    int Count;
    void **Pointers;
    HANDLE Heap;
    int Grow, Capacity;
} SV_DPA;

typedef int (*SV_DPA_COMPARE)(void *, void *, LPARAM);
__declspec(dllimport) void *HeapReAlloc(HANDLE, DWORD, void *, ULONG_PTR);

COMCTL32_API SV_DPA *DPA_CreateEx(int grow, HANDLE heap)
{
    if (grow < 8) grow = 8;
    if (!heap) heap = GetProcessHeap();
    SV_DPA *array = HeapAlloc(heap, 8, sizeof(*array));
    if (!array) return 0;
    array->Heap = heap;
    array->Grow = grow;
    return array;
}

COMCTL32_API SV_DPA *DPA_Create(int grow)
{
    return DPA_CreateEx(grow, 0);
}

COMCTL32_API void *DPA_GetPtr(SV_DPA *array, int index)
{
    return array && index >= 0 && index < array->Count ? array->Pointers[index] : 0;
}

COMCTL32_API void *DPA_DeletePtr(SV_DPA *array, int index)
{
    if (!array || index < 0 || index >= array->Count) return 0;
    void *previous = array->Pointers[index];
    for (int i = index + 1; i < array->Count; ++i)
        array->Pointers[i - 1] = array->Pointers[i];
    array->Pointers[--array->Count] = 0;
    return previous;
}

COMCTL32_API BOOL DPA_Destroy(SV_DPA *array)
{
    if (!array) return FALSE;
    HANDLE heap = array->Heap;
    if (array->Pointers) HeapFree(heap, 0, array->Pointers);
    return HeapFree(heap, 0, array);
}

COMCTL32_API BOOL DPA_DeleteAllPtrs(SV_DPA *array)
{
    if (!array) return FALSE;
    for (int i = 0; i < array->Count; i++) array->Pointers[i] = 0;
    array->Count = 0;
    return TRUE;
}

COMCTL32_API int DPA_InsertPtr(SV_DPA *array, int index, void *value)
{
    if (!array || index < 0 || array->Count == 0x7fffffff) return -1;
    if (index > array->Count) index = array->Count;
    if (array->Count == array->Capacity) {
        int grow = array->Capacity / 2;
        if (grow < array->Grow) grow = array->Grow;
        if (grow > 0x7fffffff - array->Capacity) return -1;
        int capacity = array->Capacity + grow;
        ULONG_PTR bytes = (ULONG_PTR)capacity * sizeof(void *);
        void **pointers = array->Pointers
            ? HeapReAlloc(array->Heap, 0, array->Pointers, bytes)
            : HeapAlloc(array->Heap, 0, bytes);
        if (!pointers) return -1;
        array->Pointers = pointers;
        array->Capacity = capacity;
    }
    for (int i = array->Count; i > index; i--)
        array->Pointers[i] = array->Pointers[i - 1];
    array->Pointers[index] = value;
    array->Count++;
    return index;
}

static void dpa_merge(void **items, void **scratch, int first, int last,
                      SV_DPA_COMPARE compare, LPARAM context)
{
    if (last - first < 2) return;
    int middle = first + (last - first) / 2;
    dpa_merge(items, scratch, first, middle, compare, context);
    dpa_merge(items, scratch, middle, last, compare, context);

    int left = first, right = middle;
    for (int i = first; i < last; i++) {
        if (right == last || (left < middle && compare(items[left], items[right], context) <= 0))
            scratch[i] = items[left++];
        else
            scratch[i] = items[right++];
    }
    for (int i = first; i < last; i++) items[i] = scratch[i];
}

COMCTL32_API BOOL DPA_Sort(SV_DPA *array, SV_DPA_COMPARE compare, LPARAM context)
{
    if (!array || !compare) return FALSE;
    if (array->Count < 2) return TRUE;
    void **scratch = HeapAlloc(array->Heap, 0, (ULONG_PTR)array->Count * sizeof(void *));
    if (!scratch) return FALSE;
    dpa_merge(array->Pointers, scratch, 0, array->Count, compare, context);
    HeapFree(array->Heap, 0, scratch);
    return TRUE;
}

COMCTL32_API BOOL Str_SetPtrW(WCHAR **slot, const WCHAR *text)
{
    if (!slot) return FALSE;
    WCHAR *copy = 0;
    if (text) {
        ULONG_PTR size = 1;
        while (text[size - 1]) size++;
        copy = SvComctlAlloc(size * sizeof(*copy));
        if (!copy) return FALSE;
        for (ULONG_PTR i = 0; i < size; i++) copy[i] = text[i];
    }
    SvComctlFree(*slot);
    *slot = copy;
    return TRUE;
}
