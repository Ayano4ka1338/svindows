/* Independent executive-backed user heaps shared by the Rtl and Kernel32
 * APIs. Reservations commit on demand; all serialized heaps use blocking
 * critical sections. Lookaside and low-fragmentation modes own real caches. */
extern NTSTATUS NtFreeVirtualMemory(HANDLE, void **, ULONGLONG *, ULONG);
extern NTSTATUS NtTerminateProcess(HANDLE, NTSTATUS);
static void rtl_heap_raise(NTSTATUS status);

#define RTL_HEAP_NO_SERIALIZE 0x0001UL
#define RTL_HEAP_GROWABLE 0x0002UL
#define RTL_HEAP_GENERATE_EXCEPTIONS 0x0004UL
#define RTL_HEAP_ZERO_MEMORY 0x0008UL
#define RTL_HEAP_REALLOC_IN_PLACE_ONLY 0x0010UL
#define RTL_HEAP_CREATE_ENABLE_EXECUTE 0x00040000UL
#define RTL_HEAP_MAGIC 0x48504541504e5436ULL
#define RTL_BLOCK_BUSY 1UL
#define RTL_BLOCK_INTERNAL 2UL
#define RTL_BLOCK_LOOKASIDE 4UL
#define RTL_BLOCK_CACHED 8UL
#define RTL_BLOCK_SUBALLOC 16UL
#define RTL_HEAP_PAGE 4096ULL
#define RTL_HEAP_GUARD 16ULL

typedef struct _RTL_HEAP_BLOCK {
    SIZE_T Span, Requested;
    ULONG Magic, Flags;
    struct _RTL_HEAP_BLOCK *FreeNext;
} RTL_HEAP_BLOCK;
_Static_assert(sizeof(RTL_HEAP_BLOCK) == 32, "heap block alignment");
typedef struct _RTL_HEAP_SEGMENT {
    struct _RTL_HEAP_SEGMENT *Next;
    BYTE *Base, *First;
    SIZE_T Reserved, Committed;
    BOOL Owned;
} RTL_HEAP_SEGMENT;
typedef struct _RTL_HEAP_SLAB RTL_HEAP_SLAB;
typedef struct _RTL_HEAP_BUCKET {
    struct _RTL_HEAP_BUCKET *Next;
    SIZE_T Size;
    ULONG Mode;
    RTL_HEAP_BLOCK *Free;
    RTL_HEAP_SLAB *Slabs;
} RTL_HEAP_BUCKET;
struct _RTL_HEAP_SLAB {
    RTL_HEAP_SLAB *Next;
    RTL_HEAP_BUCKET *Bucket;
    void *Pool;
    BYTE *First;
    RTL_HEAP_BLOCK *Free;
    SIZE_T Stride;
    ULONG Count, Used;
};
typedef struct _RTL_HEAP_STATE {
    ULONGLONG Magic;
    struct _RTL_HEAP_STATE *Next;
    RTL_CRITICAL_SECTION_NT Lock;
    void *ExternalLock;
    RTL_HEAP_SEGMENT FirstSegment;
    RTL_HEAP_BUCKET *Buckets;
    SIZE_T NextReserve;
    ULONG Flags, Compatibility, Active, Cookie;
} RTL_HEAP_STATE;

static RTL_HEAP_STATE *rtl_heaps;
static RTL_CRITICAL_SECTION_NT rtl_heap_registry_lock = {.LockCount = -1};
static LONG rtl_heap_terminate_on_corruption;

static SIZE_T rtl_heap_align(SIZE_T value, SIZE_T alignment)
{
    if (value > ~(SIZE_T)0 - alignment + 1) return 0;
    return (value + alignment - 1) & ~(alignment - 1);
}

static void rtl_heap_error(ULONG error)
{ write_teb_dword(0x68, error); }

static void rtl_heap_corruption(void)
{
    rtl_heap_error(13);
    if (__atomic_load_n(&rtl_heap_terminate_on_corruption, __ATOMIC_ACQUIRE))
        NtTerminateProcess((HANDLE)-1LL, (NTSTATUS)0xc0000374L);
}

static void *rtl_heap_allocation_failure(ULONG flags)
{
    rtl_heap_error(8);
    if (flags & RTL_HEAP_GENERATE_EXCEPTIONS) rtl_heap_raise(STATUS_NO_MEMORY);
    return 0;
}

static ULONG rtl_heap_block_magic(RTL_HEAP_STATE *heap, const RTL_HEAP_BLOCK *block)
{ return heap->Cookie ^ (ULONG)(ULONGLONG)block ^ (ULONG)((ULONGLONG)block >> 32); }

static void rtl_heap_init_block(RTL_HEAP_STATE *heap, RTL_HEAP_BLOCK *block,
    SIZE_T span, ULONG flags, SIZE_T requested)
{
    block->Span = span; block->Requested = requested;
    block->Magic = rtl_heap_block_magic(heap, block); block->Flags = flags;
    block->FreeNext = 0;
    if (flags & RTL_BLOCK_BUSY) {
        BYTE *guard = (BYTE *)(block + 1) + span - RTL_HEAP_GUARD;
        for (SIZE_T i = 0; i < RTL_HEAP_GUARD; ++i) guard[i] = (BYTE)(0xa5u ^ (BYTE)i);
    }
}

static BOOL rtl_heap_block_valid(RTL_HEAP_STATE *heap, RTL_HEAP_BLOCK *block,
    BYTE *limit)
{
    BYTE *address = (BYTE *)block;
    if (((ULONGLONG)address & 15) || address > limit ||
        (SIZE_T)(limit-address) < sizeof(*block) ||
        block->Magic != rtl_heap_block_magic(heap, block) || (block->Span & 15) ||
        block->Span > (SIZE_T)(limit-address) - sizeof(*block)) {
        rtl_heap_corruption(); return FALSE;
    }
    if (block->Flags & RTL_BLOCK_BUSY) {
        if (block->Span < RTL_HEAP_GUARD || block->Requested > block->Span-RTL_HEAP_GUARD) {
            rtl_heap_corruption(); return FALSE;
        }
        BYTE *guard = (BYTE *)(block+1) + block->Span-RTL_HEAP_GUARD;
        for (SIZE_T i=0;i<RTL_HEAP_GUARD;++i) if (guard[i] != (BYTE)(0xa5u ^ (BYTE)i)) {
            rtl_heap_corruption(); return FALSE;
        }
    }
    return TRUE;
}

static RTL_HEAP_STATE *rtl_heap_acquire(HANDLE handle, ULONG flags, BOOL lock)
{
    if (!NT_SUCCESS(RtlEnterCriticalSection(&rtl_heap_registry_lock))) {
        rtl_heap_error(8); return 0;
    }
    RTL_HEAP_STATE *heap;
    for (heap=rtl_heaps; heap && (HANDLE)heap!=handle; heap=heap->Next) {}
    if (heap) heap->Active++;
    RtlLeaveCriticalSection(&rtl_heap_registry_lock);
    if (!heap) { rtl_heap_error(6); return 0; }
    if (lock && !((flags|heap->Flags)&RTL_HEAP_NO_SERIALIZE) &&
        !NT_SUCCESS(RtlEnterCriticalSection(heap->ExternalLock ? heap->ExternalLock : &heap->Lock))) {
        RtlEnterCriticalSection(&rtl_heap_registry_lock); heap->Active--;
        RtlLeaveCriticalSection(&rtl_heap_registry_lock); rtl_heap_error(8); return 0;
    }
    return heap;
}

static void rtl_heap_release(RTL_HEAP_STATE *heap, ULONG flags, BOOL lock)
{
    if (lock && !((flags|heap->Flags)&RTL_HEAP_NO_SERIALIZE))
        RtlLeaveCriticalSection(heap->ExternalLock ? heap->ExternalLock : &heap->Lock);
    RtlEnterCriticalSection(&rtl_heap_registry_lock);
    heap->Active--;
    RtlLeaveCriticalSection(&rtl_heap_registry_lock);
}

static BOOL rtl_heap_commit(RTL_HEAP_STATE *heap, RTL_HEAP_SEGMENT *segment,
    SIZE_T wanted)
{
    SIZE_T committed = rtl_heap_align(wanted, RTL_HEAP_PAGE);
    if (!committed || committed > segment->Reserved) return FALSE;
    if (committed <= segment->Committed) return TRUE;
    BYTE *old_end=segment->Base+segment->Committed;
    RTL_HEAP_BLOCK *last=0;
    for (BYTE *position=segment->First; position<old_end;) {
        RTL_HEAP_BLOCK *block=(RTL_HEAP_BLOCK *)position;
        if (!rtl_heap_block_valid(heap,block,old_end)) return FALSE;
        last=block; position=(BYTE *)(block+1)+block->Span;
    }
    void *base=old_end;
    ULONGLONG bytes=committed-segment->Committed;
    ULONG protect=heap->Flags&RTL_HEAP_CREATE_ENABLE_EXECUTE ? 0x40UL : 4UL;
    if (!NT_SUCCESS(NtAllocateVirtualMemory((HANDLE)-1LL,&base,0,&bytes,0x1000UL,protect))) return FALSE;
    SIZE_T extra=committed-segment->Committed;
    segment->Committed=committed;
    if (last && !(last->Flags&(RTL_BLOCK_BUSY|RTL_BLOCK_CACHED))) last->Span+=extra;
    else rtl_heap_init_block(heap,(RTL_HEAP_BLOCK *)old_end,extra-sizeof(RTL_HEAP_BLOCK),0,0);
    return TRUE;
}

static RTL_HEAP_SEGMENT *rtl_heap_new_segment(RTL_HEAP_STATE *heap, SIZE_T needed)
{
    SIZE_T header=rtl_heap_align(sizeof(RTL_HEAP_SEGMENT),16);
    if (!header || needed > ~(SIZE_T)0-header-sizeof(RTL_HEAP_BLOCK)) return 0;
    needed+=header+sizeof(RTL_HEAP_BLOCK);
    SIZE_T reserve=needed>heap->NextReserve ? rtl_heap_align(needed,RTL_HEAP_PAGE) : heap->NextReserve;
    if (!reserve) return 0;
    void *base=0; ULONGLONG bytes=reserve;
    ULONG protect=heap->Flags&RTL_HEAP_CREATE_ENABLE_EXECUTE ? 0x40UL : 4UL;
    if (!NT_SUCCESS(NtAllocateVirtualMemory((HANDLE)-1LL,&base,0,&bytes,0x2000UL,protect))) return 0;
    SIZE_T committed=rtl_heap_align(needed>65536 ? needed : 65536,RTL_HEAP_PAGE);
    if (committed>reserve) committed=reserve;
    void *commit_base=base; ULONGLONG commit_bytes=committed;
    if (!NT_SUCCESS(NtAllocateVirtualMemory((HANDLE)-1LL,&commit_base,0,&commit_bytes,0x1000UL,protect))) {
        ULONGLONG zero=0; NtFreeVirtualMemory((HANDLE)-1LL,&base,&zero,0x8000UL); return 0;
    }
    RTL_HEAP_SEGMENT *segment=base;
    memset(segment,0,sizeof(*segment)); segment->Base=base; segment->Reserved=reserve;
    segment->Committed=committed; segment->First=(BYTE *)base+header; segment->Owned=TRUE;
    rtl_heap_init_block(heap,(RTL_HEAP_BLOCK *)segment->First,
        committed-header-sizeof(RTL_HEAP_BLOCK),0,0);
    RTL_HEAP_SEGMENT *last=&heap->FirstSegment; while(last->Next)last=last->Next;
    last->Next=segment;
    if (heap->NextReserve<8*1024*1024) heap->NextReserve*=2;
    return segment;
}

static void *rtl_heap_general_allocate(RTL_HEAP_STATE *heap, SIZE_T size, ULONG flags,
    ULONG block_flags)
{
    SIZE_T capacity=rtl_heap_align(size ? size : 1,16);
    if (!capacity || capacity>~(SIZE_T)0-RTL_HEAP_GUARD) return 0;
    SIZE_T span=capacity+RTL_HEAP_GUARD;
    for (RTL_HEAP_SEGMENT *segment=&heap->FirstSegment; segment; segment=segment->Next) {
retry_segment:;
        BYTE *end=segment->Base+segment->Committed;
        for (BYTE *position=segment->First;position<end;) {
            RTL_HEAP_BLOCK *block=(RTL_HEAP_BLOCK *)position;
            if (!rtl_heap_block_valid(heap,block,end)) return 0;
            if (!(block->Flags&(RTL_BLOCK_BUSY|RTL_BLOCK_CACHED)) && block->Span>=span) {
                SIZE_T available=block->Span;
                if (available>=span+sizeof(*block)+16) {
                    RTL_HEAP_BLOCK *tail=(RTL_HEAP_BLOCK *)((BYTE *)(block+1)+span);
                    rtl_heap_init_block(heap,tail,available-span-sizeof(*block),0,0);
                    available=span;
                }
                rtl_heap_init_block(heap,block,available,RTL_BLOCK_BUSY|block_flags,size);
                if (flags&RTL_HEAP_ZERO_MEMORY) memset(block+1,0,available-RTL_HEAP_GUARD);
                return block+1;
            }
            position=(BYTE *)(block+1)+block->Span;
        }
        if (segment->Committed<segment->Reserved) {
            SIZE_T growth=span+sizeof(RTL_HEAP_BLOCK);
            if (growth<segment->Committed) growth=segment->Committed;
            SIZE_T target=growth>segment->Reserved-segment->Committed ? segment->Reserved : segment->Committed+growth;
            if (rtl_heap_commit(heap,segment,target)) goto retry_segment;
        }
    }
    if (!(heap->Flags&RTL_HEAP_GROWABLE) || !rtl_heap_new_segment(heap,span)) return 0;
    return rtl_heap_general_allocate(heap,size,flags,block_flags);
}

static RTL_HEAP_BUCKET *rtl_heap_bucket(RTL_HEAP_STATE *heap, SIZE_T size, ULONG mode,
    BOOL create)
{
    for(RTL_HEAP_BUCKET *bucket=heap->Buckets;bucket;bucket=bucket->Next)
        if(bucket->Size==size && bucket->Mode==mode)return bucket;
    if(!create)return 0;
    RTL_HEAP_BUCKET *bucket=rtl_heap_general_allocate(heap,sizeof(*bucket),RTL_HEAP_ZERO_MEMORY,RTL_BLOCK_INTERNAL);
    if(!bucket)return 0;
    bucket->Size=size;bucket->Mode=mode;bucket->Next=heap->Buckets;heap->Buckets=bucket;
    return bucket;
}

static void *rtl_heap_cached_allocate(RTL_HEAP_STATE *heap, SIZE_T bytes, ULONG flags)
{
    SIZE_T size=rtl_heap_align(bytes ? bytes : 1,16);
    ULONG mode=heap->Compatibility;
    if(!size || (mode==1 && size>1024) || (mode==2 && size>16384))
        return rtl_heap_general_allocate(heap,bytes,flags,0);
    RTL_HEAP_BUCKET *bucket=rtl_heap_bucket(heap,size,mode,TRUE);
    if(!bucket)return 0;
    if(mode==1) {
        RTL_HEAP_BLOCK *block=bucket->Free;
        if(block) {
            bucket->Free=block->FreeNext;
            rtl_heap_init_block(heap,block,block->Span,RTL_BLOCK_BUSY|RTL_BLOCK_LOOKASIDE,bytes);
            if(flags&RTL_HEAP_ZERO_MEMORY)memset(block+1,0,block->Span-RTL_HEAP_GUARD);
            return block+1;
        }
        return rtl_heap_general_allocate(heap,bytes,flags,RTL_BLOCK_LOOKASIDE);
    }
    RTL_HEAP_SLAB *slab=bucket->Slabs;
    while(slab && !slab->Free)slab=slab->Next;
    if(!slab) {
        SIZE_T stride=sizeof(RTL_HEAP_BLOCK)+size+RTL_HEAP_GUARD;
        ULONG count=(ULONG)(65536/stride);if(count<4)count=4;if(count>128)count=128;
        SIZE_T header=rtl_heap_align(sizeof(*slab),16);
        SIZE_T total=header+stride*count;
        slab=rtl_heap_general_allocate(heap,total,RTL_HEAP_ZERO_MEMORY,RTL_BLOCK_INTERNAL);
        if(!slab)return 0;
        slab->Pool=slab;slab->Bucket=bucket;slab->Stride=stride;slab->Count=count;
        slab->First=(BYTE *)slab+header;
        for(ULONG i=0;i<count;++i) {
            RTL_HEAP_BLOCK *block=(RTL_HEAP_BLOCK *)(slab->First+(SIZE_T)i*stride);
            rtl_heap_init_block(heap,block,size+RTL_HEAP_GUARD,RTL_BLOCK_SUBALLOC,0);
            block->FreeNext=slab->Free;slab->Free=block;
        }
        slab->Next=bucket->Slabs;bucket->Slabs=slab;
    }
    RTL_HEAP_BLOCK *block=slab->Free;slab->Free=block->FreeNext;slab->Used++;
    rtl_heap_init_block(heap,block,block->Span,RTL_BLOCK_BUSY|RTL_BLOCK_SUBALLOC,bytes);
    if(flags&RTL_HEAP_ZERO_MEMORY)memset(block+1,0,size);
    return block+1;
}

static RTL_HEAP_BLOCK *rtl_heap_find_block(RTL_HEAP_STATE *heap, const void *pointer,
    RTL_HEAP_SEGMENT **segment_out, RTL_HEAP_SLAB **slab_out)
{
    if(segment_out)*segment_out=0;if(slab_out)*slab_out=0;
    for(RTL_HEAP_SEGMENT *segment=&heap->FirstSegment;segment;segment=segment->Next) {
        BYTE *end=segment->Base+segment->Committed;
        for(BYTE *position=segment->First;position<end;) {
            RTL_HEAP_BLOCK *block=(RTL_HEAP_BLOCK *)position;
            if(!rtl_heap_block_valid(heap,block,end))return 0;
            if(pointer==(const void *)(block+1) && !(block->Flags&RTL_BLOCK_INTERNAL)) {
                if(segment_out)*segment_out=segment;return block;
            }
            position=(BYTE *)(block+1)+block->Span;
        }
    }
    for(RTL_HEAP_BUCKET *bucket=heap->Buckets;bucket;bucket=bucket->Next)
        for(RTL_HEAP_SLAB *slab=bucket->Slabs;slab;slab=slab->Next) {
            BYTE *address=(BYTE *)pointer;
            if(address<slab->First+sizeof(RTL_HEAP_BLOCK) ||
               address>=slab->First+(SIZE_T)slab->Count*slab->Stride)continue;
            SIZE_T offset=(SIZE_T)(address-slab->First-sizeof(RTL_HEAP_BLOCK));
            if(offset%slab->Stride)continue;
            RTL_HEAP_BLOCK *block=(RTL_HEAP_BLOCK *)(address-sizeof(RTL_HEAP_BLOCK));
            if(!rtl_heap_block_valid(heap,block,(BYTE *)block+slab->Stride))return 0;
            if(slab_out)*slab_out=slab;return block;
        }
    return 0;
}

static void rtl_heap_coalesce(RTL_HEAP_STATE *heap, RTL_HEAP_SEGMENT *segment)
{
    BYTE *end=segment->Base+segment->Committed;
    for(BYTE *position=segment->First;position<end;) {
        RTL_HEAP_BLOCK *block=(RTL_HEAP_BLOCK *)position;
        if(!rtl_heap_block_valid(heap,block,end))return;
        BYTE *next=(BYTE *)(block+1)+block->Span;
        if(!(block->Flags&(RTL_BLOCK_BUSY|RTL_BLOCK_CACHED)) && next<end) {
            RTL_HEAP_BLOCK *other=(RTL_HEAP_BLOCK *)next;
            if(!rtl_heap_block_valid(heap,other,end))return;
            if(!(other->Flags&(RTL_BLOCK_BUSY|RTL_BLOCK_CACHED))) {
                block->Span+=sizeof(*block)+other->Span;continue;
            }
        }
        position=next;
    }
}

static BOOL rtl_heap_free_block(RTL_HEAP_STATE *heap, void *pointer)
{
    RTL_HEAP_SEGMENT *segment;RTL_HEAP_SLAB *slab;
    RTL_HEAP_BLOCK *block=rtl_heap_find_block(heap,pointer,&segment,&slab);
    if(!block || !(block->Flags&RTL_BLOCK_BUSY)) { rtl_heap_error(87);return FALSE; }
    if(slab) {
        rtl_heap_init_block(heap,block,block->Span,RTL_BLOCK_SUBALLOC,0);
        block->FreeNext=slab->Free;slab->Free=block;slab->Used--;return TRUE;
    }
    if((block->Flags&RTL_BLOCK_LOOKASIDE) && heap->Compatibility==1) {
        RTL_HEAP_BUCKET *bucket=rtl_heap_bucket(heap,block->Span-RTL_HEAP_GUARD,1,FALSE);
        if(bucket) {
            rtl_heap_init_block(heap,block,block->Span,RTL_BLOCK_CACHED,0);
            block->FreeNext=bucket->Free;bucket->Free=block;return TRUE;
        }
    }
    rtl_heap_init_block(heap,block,block->Span,0,0);
    rtl_heap_coalesce(heap,segment);return TRUE;
}

HANDLE RtlCreateHeap(ULONG flags, void *base, SIZE_T reserve, SIZE_T commit,
    void *lock, void *parameters)
{
    (void)parameters;
    SIZE_T header=rtl_heap_align(sizeof(RTL_HEAP_STATE),16);
    SIZE_T minimum=header+sizeof(RTL_HEAP_BLOCK)+16;
    SIZE_T reservation=rtl_heap_align(reserve ? reserve : 1024*1024,RTL_HEAP_PAGE);
    SIZE_T committed=rtl_heap_align(commit ? commit : 65536,RTL_HEAP_PAGE);
    if(!reservation || !committed || reservation<minimum) {rtl_heap_error(87);return 0;}
    if(committed>reservation)committed=reservation;
    if(committed<minimum)committed=rtl_heap_align(minimum,RTL_HEAP_PAGE);
    BOOL owned=!base;
    ULONG protect=flags&RTL_HEAP_CREATE_ENABLE_EXECUTE ? 0x40UL : 4UL;
    if(owned) {
        ULONGLONG bytes=reservation;
        if(!NT_SUCCESS(NtAllocateVirtualMemory((HANDLE)-1LL,&base,0,&bytes,0x2000UL,protect)))
            return rtl_heap_allocation_failure(flags);
    }
    void *commit_base=base;ULONGLONG commit_bytes=committed;
    if(!NT_SUCCESS(NtAllocateVirtualMemory((HANDLE)-1LL,&commit_base,0,&commit_bytes,0x1000UL,protect))) {
        if(owned){ULONGLONG zero=0;NtFreeVirtualMemory((HANDLE)-1LL,&base,&zero,0x8000UL);}
        return rtl_heap_allocation_failure(flags);
    }
    RTL_HEAP_STATE *heap=base;memset(heap,0,sizeof(*heap));
    heap->Magic=RTL_HEAP_MAGIC;heap->Flags=flags;heap->ExternalLock=lock;
    heap->Lock.LockCount=-1;heap->NextReserve=reservation<8*1024*1024 ? reservation*2 : reservation;
    heap->Cookie=(ULONG)(ULONGLONG)heap^(ULONG)((ULONGLONG)heap>>32)^0x68456150UL;
    heap->FirstSegment=(RTL_HEAP_SEGMENT){.Base=base,.First=(BYTE *)base+header,
        .Reserved=reservation,.Committed=committed,.Owned=owned};
    rtl_heap_init_block(heap,(RTL_HEAP_BLOCK *)heap->FirstSegment.First,
        committed-header-sizeof(RTL_HEAP_BLOCK),0,0);
    if(!NT_SUCCESS(RtlEnterCriticalSection(&rtl_heap_registry_lock))) {
        if(owned){ULONGLONG zero=0;NtFreeVirtualMemory((HANDLE)-1LL,&base,&zero,0x8000UL);}
        return rtl_heap_allocation_failure(flags);
    }
    heap->Next=rtl_heaps;rtl_heaps=heap;
    RtlLeaveCriticalSection(&rtl_heap_registry_lock);return heap;
}

HANDLE RtlDestroyHeap(HANDLE handle)
{
    if(!NT_SUCCESS(RtlEnterCriticalSection(&rtl_heap_registry_lock)))return handle;
    RTL_HEAP_STATE **link=&rtl_heaps;while(*link && (HANDLE)*link!=handle)link=&(*link)->Next;
    if(!*link){RtlLeaveCriticalSection(&rtl_heap_registry_lock);rtl_heap_error(6);return handle;}
    RTL_HEAP_STATE *heap=*link;
    if(heap->Active){RtlLeaveCriticalSection(&rtl_heap_registry_lock);rtl_heap_error(170);return handle;}
    *link=heap->Next;RtlLeaveCriticalSection(&rtl_heap_registry_lock);
    RtlDeleteCriticalSection(&heap->Lock);
    RTL_HEAP_SEGMENT first=heap->FirstSegment;
    for(RTL_HEAP_SEGMENT *segment=first.Next;segment;) {
        RTL_HEAP_SEGMENT *next=segment->Next;void *base=segment->Base;ULONGLONG zero=0;
        if(segment->Owned)NtFreeVirtualMemory((HANDLE)-1LL,&base,&zero,0x8000UL);
        segment=next;
    }
    heap->Magic=0;
    if(first.Owned){void *base=first.Base;ULONGLONG zero=0;NtFreeVirtualMemory((HANDLE)-1LL,&base,&zero,0x8000UL);}
    return 0;
}

void *RtlAllocateHeap(HANDLE handle, ULONG flags, SIZE_T bytes)
{
    RTL_HEAP_STATE *heap=rtl_heap_acquire(handle,flags,TRUE);if(!heap)return 0;
    void *memory=heap->Compatibility ? rtl_heap_cached_allocate(heap,bytes,flags) :
                                      rtl_heap_general_allocate(heap,bytes,flags,0);
    ULONG allocation_flags=flags|heap->Flags;
    rtl_heap_release(heap,flags,TRUE);
    return memory ? memory : rtl_heap_allocation_failure(allocation_flags);
}

BOOL RtlFreeHeap(HANDLE handle, ULONG flags, void *memory)
{
    RTL_HEAP_STATE *heap=rtl_heap_acquire(handle,flags,TRUE);if(!heap)return FALSE;
    BOOL freed=!memory || rtl_heap_free_block(heap,memory);
    rtl_heap_release(heap,flags,TRUE);return freed;
}

SIZE_T RtlSizeHeap(HANDLE handle, ULONG flags, void *memory)
{
    RTL_HEAP_STATE *heap=rtl_heap_acquire(handle,flags,TRUE);if(!heap)return (SIZE_T)-1;
    RTL_HEAP_BLOCK *block=memory ? rtl_heap_find_block(heap,memory,0,0) : 0;
    SIZE_T size=block && (block->Flags&RTL_BLOCK_BUSY) ? block->Span-RTL_HEAP_GUARD : (SIZE_T)-1;
    if(size==(SIZE_T)-1)rtl_heap_error(87);
    rtl_heap_release(heap,flags,TRUE);return size;
}

void *RtlReAllocateHeap(HANDLE handle, ULONG flags, void *memory, SIZE_T bytes)
{
    if(!memory){rtl_heap_error(87);return 0;}
    RTL_HEAP_STATE *heap=rtl_heap_acquire(handle,flags,TRUE);if(!heap)return 0;
    RTL_HEAP_SEGMENT *segment;RTL_HEAP_SLAB *slab;
    RTL_HEAP_BLOCK *block=rtl_heap_find_block(heap,memory,&segment,&slab);
    if(!block || !(block->Flags&RTL_BLOCK_BUSY)) {
        rtl_heap_release(heap,flags,TRUE);rtl_heap_error(87);return 0;
    }
    SIZE_T capacity=rtl_heap_align(bytes ? bytes : 1,16),old=block->Requested;
    if(!capacity || capacity>~(SIZE_T)0-RTL_HEAP_GUARD) {
        rtl_heap_release(heap,flags,TRUE);return rtl_heap_allocation_failure(flags);
    }
    SIZE_T span=capacity+RTL_HEAP_GUARD;
    if(!slab && span>block->Span) {
        BYTE *end=segment->Base+segment->Committed;
        BYTE *next=(BYTE *)(block+1)+block->Span;
        if(next==end && segment->Committed<segment->Reserved) {
            SIZE_T target=segment->Committed+span-block->Span+sizeof(*block);
            if(target>segment->Reserved)target=segment->Reserved;
            rtl_heap_commit(heap,segment,target);end=segment->Base+segment->Committed;
        }
        if(next<end) {
            RTL_HEAP_BLOCK *free=(RTL_HEAP_BLOCK *)next;
            if(rtl_heap_block_valid(heap,free,end) && !(free->Flags&(RTL_BLOCK_BUSY|RTL_BLOCK_CACHED)) &&
               block->Span+sizeof(*block)+free->Span>=span)block->Span+=sizeof(*block)+free->Span;
        }
    }
    if(span<=block->Span) {
        ULONG block_flags=block->Flags;
        SIZE_T available=block->Span;
        if(!slab && !(block_flags&RTL_BLOCK_LOOKASIDE) && available>=span+sizeof(*block)+16) {
            rtl_heap_init_block(heap,(RTL_HEAP_BLOCK *)((BYTE *)(block+1)+span),
                available-span-sizeof(*block),0,0);available=span;
        }
        rtl_heap_init_block(heap,block,available,block_flags,bytes);
        if((flags&RTL_HEAP_ZERO_MEMORY) && bytes>old)memset((BYTE *)memory+old,0,bytes-old);
        if(segment)rtl_heap_coalesce(heap,segment);
        rtl_heap_release(heap,flags,TRUE);return memory;
    }
    if(flags&RTL_HEAP_REALLOC_IN_PLACE_ONLY) {
        rtl_heap_release(heap,flags,TRUE);return rtl_heap_allocation_failure(flags);
    }
    void *fresh=heap->Compatibility ? rtl_heap_cached_allocate(heap,bytes,flags) :
                                     rtl_heap_general_allocate(heap,bytes,flags,0);
    if(fresh){memcpy(fresh,memory,old<bytes ? old : bytes);rtl_heap_free_block(heap,memory);}
    rtl_heap_release(heap,flags,TRUE);return fresh ? fresh : rtl_heap_allocation_failure(flags);
}

BOOLEAN RtlValidateHeap(HANDLE handle, ULONG flags, void *memory)
{
    RTL_HEAP_STATE *heap=rtl_heap_acquire(handle,flags,TRUE);if(!heap)return FALSE;
    BOOL valid=TRUE;
    if(memory){RTL_HEAP_BLOCK *block=rtl_heap_find_block(heap,memory,0,0);valid=block && (block->Flags&RTL_BLOCK_BUSY);}
    else {
        for(RTL_HEAP_SEGMENT *segment=&heap->FirstSegment;valid && segment;segment=segment->Next)
            for(BYTE *position=segment->First;position<segment->Base+segment->Committed;) {
                RTL_HEAP_BLOCK *block=(RTL_HEAP_BLOCK *)position;
                if(!rtl_heap_block_valid(heap,block,segment->Base+segment->Committed)){valid=FALSE;break;}
                position=(BYTE *)(block+1)+block->Span;
            }
        for(RTL_HEAP_BUCKET *bucket=heap->Buckets;valid && bucket;bucket=bucket->Next)
            for(RTL_HEAP_SLAB *slab=bucket->Slabs;valid && slab;slab=slab->Next)
                for(ULONG i=0;i<slab->Count;++i) {
                    RTL_HEAP_BLOCK *block=(RTL_HEAP_BLOCK *)(slab->First+(SIZE_T)i*slab->Stride);
                    if(!rtl_heap_block_valid(heap,block,(BYTE *)block+slab->Stride)){valid=FALSE;break;}
                }
    }
    rtl_heap_release(heap,flags,TRUE);return valid;
}

BOOLEAN RtlLockHeap(HANDLE handle)
{
    RTL_HEAP_STATE *heap=rtl_heap_acquire(handle,0,FALSE);if(!heap)return FALSE;
    if(NT_SUCCESS(RtlEnterCriticalSection(heap->ExternalLock ? heap->ExternalLock : &heap->Lock)))return TRUE;
    rtl_heap_release(heap,0,FALSE);return FALSE;
}

BOOLEAN RtlUnlockHeap(HANDLE handle)
{
    RTL_HEAP_STATE *heap=rtl_heap_acquire(handle,0,FALSE);if(!heap)return FALSE;
    NTSTATUS status=RtlLeaveCriticalSection(heap->ExternalLock ? heap->ExternalLock : &heap->Lock);
    rtl_heap_release(heap,0,FALSE);
    if(!NT_SUCCESS(status)){rtl_heap_error(288);return FALSE;}
    rtl_heap_release(heap,0,FALSE);return TRUE;
}

static void rtl_heap_drain_lookaside(RTL_HEAP_STATE *heap)
{
    for(RTL_HEAP_BUCKET *bucket=heap->Buckets;bucket;bucket=bucket->Next) {
        while(bucket->Free) {
            RTL_HEAP_BLOCK *block=bucket->Free;bucket->Free=block->FreeNext;
            rtl_heap_init_block(heap,block,block->Span,0,0);
        }
    }
    for(RTL_HEAP_SEGMENT *segment=&heap->FirstSegment;segment;segment=segment->Next)
        rtl_heap_coalesce(heap,segment);
}

SIZE_T RtlCompactHeap(HANDLE handle, ULONG flags)
{
    RTL_HEAP_STATE *heap=rtl_heap_acquire(handle,flags,TRUE);if(!heap)return 0;
    rtl_heap_drain_lookaside(heap);
    for(RTL_HEAP_BUCKET *bucket=heap->Buckets;bucket;bucket=bucket->Next) {
        RTL_HEAP_SLAB **link=&bucket->Slabs;
        while(*link) {
            RTL_HEAP_SLAB *slab=*link;
            if(slab->Used){link=&slab->Next;continue;}
            *link=slab->Next;RTL_HEAP_BLOCK *pool=(RTL_HEAP_BLOCK *)slab->Pool-1;
            pool->Flags&=~RTL_BLOCK_INTERNAL;rtl_heap_free_block(heap,slab->Pool);
        }
    }
    SIZE_T largest=0;
    for(RTL_HEAP_SEGMENT *segment=&heap->FirstSegment;segment;segment=segment->Next)
        for(BYTE *position=segment->First;position<segment->Base+segment->Committed;) {
            RTL_HEAP_BLOCK *block=(RTL_HEAP_BLOCK *)position;
            if(!rtl_heap_block_valid(heap,block,segment->Base+segment->Committed))break;
            if(!(block->Flags&(RTL_BLOCK_BUSY|RTL_BLOCK_CACHED)) && block->Span>RTL_HEAP_GUARD &&
                block->Span-RTL_HEAP_GUARD>largest)largest=block->Span-RTL_HEAP_GUARD;
            position=(BYTE *)(block+1)+block->Span;
        }
    rtl_heap_release(heap,flags,TRUE);return largest;
}

NTSTATUS RtlSetHeapInformation(HANDLE handle, ULONG info_class, void *data, SIZE_T bytes)
{
    if(info_class==1) {
        if(data || bytes)return STATUS_INVALID_PARAMETER;
        __atomic_store_n(&rtl_heap_terminate_on_corruption,1,__ATOMIC_RELEASE);return STATUS_SUCCESS;
    }
    if(info_class!=0)return (NTSTATUS)0xc0000003L;
    if(!data || bytes<sizeof(ULONG))return STATUS_INVALID_PARAMETER;
    ULONG mode=*(ULONG *)data;
    RTL_HEAP_STATE *heap=rtl_heap_acquire(handle,0,TRUE);if(!heap)return (NTSTATUS)0xc0000008L;
    NTSTATUS status=STATUS_SUCCESS;
    if(mode>2 || (mode==2 && (heap->Flags&RTL_HEAP_NO_SERIALIZE)))status=STATUS_INVALID_PARAMETER;
    else {if(mode!=1)rtl_heap_drain_lookaside(heap);heap->Compatibility=mode;}
    rtl_heap_release(heap,0,TRUE);return status;
}

NTSTATUS RtlQueryHeapInformation(HANDLE handle, ULONG info_class, void *data,
    SIZE_T bytes, SIZE_T *needed)
{
    if(info_class!=0)return (NTSTATUS)0xc0000003L;
    if(needed)*needed=sizeof(ULONG);
    if(!data || bytes<sizeof(ULONG))return (NTSTATUS)0xc0000023L;
    RTL_HEAP_STATE *heap=rtl_heap_acquire(handle,0,TRUE);if(!heap)return (NTSTATUS)0xc0000008L;
    *(ULONG *)data=heap->Compatibility;rtl_heap_release(heap,0,TRUE);return STATUS_SUCCESS;
}

ULONG RtlGetProcessHeaps(ULONG capacity, HANDLE *heaps)
{
    if(capacity && !heaps){rtl_heap_error(87);return 0;}
    if(!NT_SUCCESS(RtlEnterCriticalSection(&rtl_heap_registry_lock)))return 0;
    ULONG count=0;for(RTL_HEAP_STATE *heap=rtl_heaps;heap;heap=heap->Next)count++;
    if(capacity>=count){ULONG i=0;for(RTL_HEAP_STATE *heap=rtl_heaps;heap;heap=heap->Next)heaps[i++]=heap;}
    RtlLeaveCriticalSection(&rtl_heap_registry_lock);return count;
}

static HANDLE rtl_process_heap(void)
{
    void **slot=(void **)(read_teb_qword(0x60)+0x30);
    HANDLE heap=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
    if(heap)return heap;
    HANDLE fresh=RtlCreateHeap(RTL_HEAP_GROWABLE,0,0,0,0,0);if(!fresh)return 0;
    HANDLE expected=0;
    if(__atomic_compare_exchange_n(slot,&expected,fresh,0,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE))return fresh;
    RtlDestroyHeap(fresh);return expected;
}

void RtlFlushHeaps(void)
{
    RtlEnterCriticalSection(&rtl_heap_registry_lock);
    for(RTL_HEAP_STATE *heap=rtl_heaps;heap;heap=heap->Next) {
        if(RtlTryEnterCriticalSection(heap->ExternalLock ? heap->ExternalLock : &heap->Lock)) {
            rtl_heap_drain_lookaside(heap);
            RtlLeaveCriticalSection(heap->ExternalLock ? heap->ExternalLock : &heap->Lock);
        }
    }
    RtlLeaveCriticalSection(&rtl_heap_registry_lock);
}
