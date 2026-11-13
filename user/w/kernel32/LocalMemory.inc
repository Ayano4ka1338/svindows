/* Win32 Local and Global movable allocations share one process-heap
 * handle manager. Each API preserves its documented lock/return semantics. */
#define LMEM_MOVEABLE 0x0002u
#define LMEM_ZEROINIT 0x0040u

__declspec(dllexport) void *LocalAlloc(DWORD flags, SIZE_T bytes)
{ return GlobalAlloc(flags, bytes); }
__declspec(dllexport) void *LocalFree(void *memory)
{ return GlobalFree(memory); }
__declspec(dllexport) void *LocalLock(void *memory)
{ return GlobalLock(memory); }
__declspec(dllexport) BOOL LocalUnlock(void *memory)
{
    HANDLE heap=GetProcessHeap();
    if (!HeapLock(heap)) return 0;
    GLOBAL_MOVEABLE *block=global_moveable(memory);
    if (!block) {
        BOOL valid=memory && HeapValidate(heap,0,memory);
        HeapUnlock(heap);
        SetLastError(valid ? 158 : 6);
        return 0;
    }
    BOOL result=GlobalUnlock(memory);
    HeapUnlock(heap);
    return result;
}
__declspec(dllexport) SIZE_T LocalSize(void *memory)
{ return GlobalSize(memory); }
__declspec(dllexport) void *LocalHandle(const void *memory)
{ return GlobalHandle(memory); }
__declspec(dllexport) UINT LocalFlags(void *memory)
{ return GlobalFlags(memory); }
__declspec(dllexport) void *LocalReAlloc(void *memory, SIZE_T bytes, DWORD flags)
{ return GlobalReAlloc(memory,bytes,flags); }
