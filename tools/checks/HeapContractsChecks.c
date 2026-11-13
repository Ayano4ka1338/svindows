/* Exercise the same Rtl allocator, Kernel32 facade and Local/Global manager
 * used by guest programs, over host VM reservations and blocking semaphores. */
#include "UserRuntimeHost.h"
#include "../../user/w/ntdll/Rtl/CriticalSections.inc"
#include "../../user/w/ntdll/Rtl/Heap.inc"
#define NT_INVALID_HANDLE ((HANDLE)-1LL)
#define HEAP_ZERO_MEMORY 8u
#include "../../user/w/kernel32/HeapApi.inc"
#include "../../user/w/kernel32/GlobalMemory.inc"
#include "../../user/w/kernel32/LocalMemory.inc"

static _Atomic unsigned shared_allocations;
static void *allocate_worker(void *identifier)
{
    current_tid=(ULONG_PTR)identifier;
    HANDLE heap=GetProcessHeap();
    for(unsigned iteration=0;iteration<2500;++iteration) {
        SIZE_T bytes=17+(iteration%257);
        BYTE *memory=HeapAlloc(heap,HEAP_ZERO_MEMORY,bytes);assert(memory);
        assert(!((uintptr_t)memory&15));
        for(SIZE_T i=0;i<bytes;++i)assert(!memory[i]);
        memset(memory,(int)current_tid,bytes);
        BYTE *grown=RtlReAllocateHeap(heap,0,memory,bytes+512);assert(grown);
        for(SIZE_T i=0;i<bytes;++i)assert(grown[i]==(BYTE)current_tid);
        assert(HeapSize(heap,0,grown)>=bytes+512);
        assert(RtlFreeHeap(heap,0,grown));
        HANDLE movable=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,bytes);assert(movable);
        BYTE *data=LocalLock(movable);assert(data);memset(data,42,bytes);
        assert(!GlobalUnlock(movable) && host_last_error==0);
        assert(LocalSize(movable)==bytes && GlobalHandle(data)==movable);
        assert(!LocalFree(movable));
        atomic_fetch_add(&shared_allocations,1);
    }
    return 0;
}

static void check_independent_heaps(void)
{
    HANDLE first=HeapCreate(0,4096,16384),second=HeapCreate(0,4096,16384);
    assert(first && second && first!=second);
    BYTE *memory=HeapAlloc(first,HEAP_ZERO_MEMORY,65);assert(memory);
    assert(RtlValidateHeap(first,0,memory));
    assert(!HeapValidate(second,0,memory));
    assert(!RtlFreeHeap(second,0,memory));
    assert(!HeapFree(first,0,memory+1));
    assert(HeapValidate(first,0,memory));
    assert(RtlFreeHeap(first,0,memory));assert(!HeapFree(first,0,memory));
    assert(!HeapAlloc(first,0,65536));
    assert(HeapValidate(first,0,0));
    assert(HeapDestroy(first) && HeapDestroy(second));
    assert(!HeapAlloc(first,0,16));
    assert(!HeapDestroy(first));
    puts("PASS independent heaps, allocation ownership, invalid/free/stale handles and maximum size");
}

static void check_movable_contracts(void)
{
    HANDLE heap=GetProcessHeap();
    HANDLE memory=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,32);assert(memory);
    BYTE *data=GlobalLock(memory);assert(data && GlobalSize(memory)==32);
    for(unsigned i=0;i<32;++i)assert(!data[i]);
    memset(data,0x51,32);
    assert(LocalHandle(data)==memory && (GlobalFlags(memory)&255)==1);
    assert(!GlobalDiscard(memory) && host_last_error==212);
    HANDLE grown=GlobalReAlloc(memory,96,GMEM_ZEROINIT);assert(grown==memory);
    BYTE *same=GlobalLock(memory);assert(same==data);
    for(unsigned i=0;i<32;++i)assert(same[i]==0x51);
    for(unsigned i=32;i<96;++i)assert(!same[i]);
    assert(LocalUnlock(memory));assert(!GlobalUnlock(memory) && host_last_error==0);
    assert(GlobalDiscard(memory)==memory && (GlobalFlags(memory)&GMEM_DISCARDED));
    assert(!GlobalLock(memory) && host_last_error==157);
    assert(GlobalReAlloc(memory,100,GMEM_ZEROINIT)==memory);
    data=LocalLock(memory);assert(data);
    for(unsigned i=0;i<100;++i)assert(!data[i]);
    assert(!LocalUnlock(memory));
    assert(!GlobalFree(memory));assert(!GlobalLock(memory) && host_last_error==6);
    BYTE *fixed=LocalAlloc(0,48);assert(fixed);
    HANDLE converted=GlobalReAlloc(fixed,0,GMEM_MODIFY|GMEM_MOVEABLE);assert(converted && converted!=fixed);
    assert(GlobalLock(converted)==fixed);assert(!GlobalUnlock(converted));assert(!LocalFree(converted));
    assert(!HeapDestroy(heap));
    puts("PASS movable/fixed Local-Global interoperation, stable resize, locks, discard and stale handles");
}

static void check_real_heap_modes(void)
{
    HANDLE heap=HeapCreate(0,4096,0);assert(heap);
    DWORD mode=1,got=0;SIZE_T needed=0;
    assert(HeapSetInformation(heap,0,&mode,sizeof(mode)));
    BYTE *first=HeapAlloc(heap,0,48);assert(first);assert(HeapFree(heap,0,first));
    BYTE *again=HeapAlloc(heap,0,48);assert(again==first);assert(HeapFree(heap,0,again));
    mode=2;assert(HeapSetInformation(heap,0,&mode,sizeof(mode)));
    assert(HeapQueryInformation(heap,0,&got,sizeof(got),&needed) && got==2 && needed==4);
    void *slots[70];
    for(unsigned i=0;i<70;++i){slots[i]=HeapAlloc(heap,HEAP_ZERO_MEMORY,48);assert(slots[i]);}
    RTL_HEAP_STATE *state=heap;RTL_HEAP_BUCKET *bucket=rtl_heap_bucket(state,48,2,FALSE);
    assert(bucket && bucket->Slabs && bucket->Slabs->Used>=70);
    assert((BYTE *)slots[0]-(BYTE *)slots[1]==(ptrdiff_t)bucket->Slabs->Stride);
    for(unsigned i=0;i<70;++i)assert(HeapFree(heap,0,slots[i]));
    assert(HeapCompact(heap,0)>0 && !bucket->Slabs);
    assert(HeapDestroy(heap));
    HANDLE unserialized=HeapCreate(RTL_HEAP_NO_SERIALIZE,4096,0);assert(unserialized);
    assert(!HeapSetInformation(unserialized,0,&mode,sizeof(mode)));
    assert(HeapLock(unserialized));BYTE *memory=HeapAlloc(unserialized,0,16);assert(memory);
    assert(HeapFree(unserialized,0,memory));assert(HeapUnlock(unserialized));assert(HeapDestroy(unserialized));
    puts("PASS actual lookaside reuse, LFH slabs, compaction and explicit locking of unserialized heaps");
}

static void check_growth_and_failure(void)
{
    HANDLE heap=HeapCreate(0,4096,0);assert(heap);
    BYTE *memory=HeapAlloc(heap,0,200000);assert(memory);memset(memory,0x7c,200000);
    BYTE *grown=HeapReAlloc(heap,HEAP_ZERO_MEMORY,memory,1000000);assert(grown);
    for(unsigned i=0;i<200000;++i)assert(grown[i]==0x7c);
    for(unsigned i=200000;i<1000000;++i)assert(!grown[i]);
    assert(!HeapAlloc(heap,0,(SIZE_T)-1));
    host_fail_vm=-1;
    assert(!HeapAlloc(heap,RTL_HEAP_GENERATE_EXCEPTIONS,16000000));
    host_fail_vm=0;
    assert(atomic_load(&host_raised_exceptions)==1);
    assert(HeapValidate(heap,0,grown));
    assert(HeapFree(heap,0,grown));assert(HeapDestroy(heap));
    puts("PASS reservation/commit growth, zero-preserving reallocation, overflow and allocation-failure rollback");
}

int main(void)
{
    check_independent_heaps();check_movable_contracts();check_real_heap_modes();check_growth_and_failure();
    pthread_t workers[4];
    for(ULONG_PTR i=0;i<4;++i)assert(!pthread_create(&workers[i],0,allocate_worker,(void *)(i+20)));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(workers[i],0));
    assert(atomic_load(&shared_allocations)==10000 && atomic_load(&host_wait_calls)>0);
    HANDLE heap=GetProcessHeap();assert(HeapValidate(heap,0,0));
    HANDLE heaps[16];assert(GetProcessHeaps(16,heaps)==1 && heaps[0]==heap);
    assert(!g_global_memory);
    assert(!RtlDestroyHeap(heap));host_peb.ProcessHeap=0;
    host_runtime_cleanup();
    puts("PASS concurrent Heap/Rtl/Local/Global allocation and complete native-resource release");
    return 0;
}
