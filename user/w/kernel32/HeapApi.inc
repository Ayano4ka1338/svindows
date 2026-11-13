/* Kernel32 heap APIs use ntdll's allocator, so Heap* and Rtl* may operate
 * on the same opaque heap and allocations without incompatible headers. */
extern HANDLE RtlCreateHeap(DWORD, void *, SIZE_T, SIZE_T, void *, void *);
extern HANDLE RtlDestroyHeap(HANDLE);
extern void *RtlAllocateHeap(HANDLE, DWORD, SIZE_T);
extern BOOL RtlFreeHeap(HANDLE, DWORD, void *);
extern void *RtlReAllocateHeap(HANDLE, DWORD, void *, SIZE_T);
extern SIZE_T RtlSizeHeap(HANDLE, DWORD, void *);
extern BOOL RtlValidateHeap(HANDLE, DWORD, void *);
extern BOOL RtlLockHeap(HANDLE);
extern BOOL RtlUnlockHeap(HANDLE);
extern SIZE_T RtlCompactHeap(HANDLE, DWORD);
extern DWORD RtlGetProcessHeaps(DWORD, HANDLE *);
extern NTSTATUS RtlSetHeapInformation(HANDLE, DWORD, void *, SIZE_T);
extern NTSTATUS RtlQueryHeapInformation(HANDLE, DWORD, void *, SIZE_T, SIZE_T *);
extern DWORD RtlNtStatusToDosError(NTSTATUS);

__declspec(dllexport) HANDLE HeapCreate(DWORD flags, SIZE_T initial, SIZE_T maximum)
{
    if ((flags & ~0x00040005UL) || (maximum && initial > maximum)) {
        SetLastError(87); return 0;
    }
    return RtlCreateHeap(flags | (maximum ? 0 : 2), 0, maximum, initial, 0, 0);
}

__declspec(dllexport) BOOL HeapDestroy(HANDLE heap)
{
    if (!heap || heap == NtCurrentPeb()->ProcessHeap) { SetLastError(87); return 0; }
    return RtlDestroyHeap(heap) == 0;
}

__declspec(dllexport) HANDLE GetProcessHeap(void)
{
    void **slot = &NtCurrentPeb()->ProcessHeap;
    HANDLE heap = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (heap) return heap;
    HANDLE fresh = RtlCreateHeap(2, 0, 0, 0, 0, 0);
    if (!fresh) return 0;
    HANDLE expected = 0;
    if (__atomic_compare_exchange_n(slot, &expected, fresh, 0,
            __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) return fresh;
    RtlDestroyHeap(fresh);
    return expected;
}

__declspec(dllexport) LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{ return RtlAllocateHeap(heap, flags, bytes); }
__declspec(dllexport) BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID memory)
{ return RtlFreeHeap(heap, flags, memory); }
__declspec(dllexport) LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID memory, SIZE_T bytes)
{ return RtlReAllocateHeap(heap, flags, memory, bytes); }
__declspec(dllexport) SIZE_T HeapSize(HANDLE heap, DWORD flags, const void *memory)
{ return RtlSizeHeap(heap, flags, (void *)memory); }
__declspec(dllexport) BOOL HeapValidate(HANDLE heap, DWORD flags, const void *memory)
{ return RtlValidateHeap(heap, flags, (void *)memory); }
__declspec(dllexport) BOOL HeapLock(HANDLE heap)
{ return RtlLockHeap(heap); }
__declspec(dllexport) BOOL HeapUnlock(HANDLE heap)
{ return RtlUnlockHeap(heap); }
__declspec(dllexport) SIZE_T HeapCompact(HANDLE heap, DWORD flags)
{ return RtlCompactHeap(heap, flags); }
__declspec(dllexport) DWORD GetProcessHeaps(DWORD capacity, HANDLE *heaps)
{ return RtlGetProcessHeaps(capacity, heaps); }

__declspec(dllexport) BOOL HeapSetInformation(HANDLE heap, int info_class,
    LPVOID info, SIZE_T bytes)
{
    NTSTATUS status=RtlSetHeapInformation(heap,(DWORD)info_class,info,bytes);
    if (!NT_SUCCESS(status)) SetLastError(ntstatus_to_win32(status));
    return NT_SUCCESS(status);
}
__declspec(dllexport) BOOL HeapQueryInformation(HANDLE heap, int info_class,
    LPVOID info, SIZE_T bytes, SIZE_T *needed)
{
    NTSTATUS status=RtlQueryHeapInformation(heap,(DWORD)info_class,info,bytes,needed);
    if (!NT_SUCCESS(status)) SetLastError(ntstatus_to_win32(status));
    return NT_SUCCESS(status);
}
