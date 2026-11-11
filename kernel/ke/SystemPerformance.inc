/* Windows 7 system-performance ABI. Unsupported NT cache/paging mechanisms
 * have no activity in this eager-allocation, synchronous-write kernel. */
typedef struct {
    UINT64 IdleProcessTime;
    UINT64 IoReadTransferCount;
    UINT64 IoWriteTransferCount;
    UINT64 IoOtherTransferCount;
    ULONG IoReadOperationCount;
    ULONG IoWriteOperationCount;
    ULONG IoOtherOperationCount;
    ULONG AvailablePages;
    ULONG CommittedPages;
    ULONG CommitLimit;
    ULONG PeakCommitment;
    ULONG PageFaultCount;
    ULONG CopyOnWriteCount;
    ULONG TransitionCount;
    ULONG CacheTransitionCount;
    ULONG DemandZeroCount;
    ULONG PageReadCount;
    ULONG PageReadIoCount;
    ULONG CacheReadCount;
    ULONG CacheIoCount;
    ULONG DirtyPagesWriteCount;
    ULONG DirtyWriteIoCount;
    ULONG MappedPagesWriteCount;
    ULONG MappedWriteIoCount;
    ULONG PagedPoolPages;
    ULONG NonPagedPoolPages;
    ULONG PagedPoolAllocs;
    ULONG PagedPoolFrees;
    ULONG NonPagedPoolAllocs;
    ULONG NonPagedPoolFrees;
    ULONG FreeSystemPtes;
    ULONG ResidentSystemCodePage;
    ULONG TotalSystemDriverPages;
    ULONG TotalSystemCodePages;
    ULONG NonPagedPoolLookasideHits;
    ULONG PagedPoolLookasideHits;
    ULONG AvailablePagedPoolPages;
    ULONG ResidentSystemCachePage;
    ULONG ResidentPagedPoolPage;
    ULONG ResidentSystemDriverPage;
    ULONG CcFastReadNoWait;
    ULONG CcFastReadWait;
    ULONG CcFastReadResourceMiss;
    ULONG CcFastReadNotPossible;
    ULONG CcFastMdlReadNoWait;
    ULONG CcFastMdlReadWait;
    ULONG CcFastMdlReadResourceMiss;
    ULONG CcFastMdlReadNotPossible;
    ULONG CcMapDataNoWait;
    ULONG CcMapDataWait;
    ULONG CcMapDataNoWaitMiss;
    ULONG CcMapDataWaitMiss;
    ULONG CcPinMappedDataCount;
    ULONG CcPinReadNoWait;
    ULONG CcPinReadWait;
    ULONG CcPinReadNoWaitMiss;
    ULONG CcPinReadWaitMiss;
    ULONG CcCopyReadNoWait;
    ULONG CcCopyReadWait;
    ULONG CcCopyReadNoWaitMiss;
    ULONG CcCopyReadWaitMiss;
    ULONG CcMdlReadNoWait;
    ULONG CcMdlReadWait;
    ULONG CcMdlReadNoWaitMiss;
    ULONG CcMdlReadWaitMiss;
    ULONG CcReadAheadIos;
    ULONG CcLazyWriteIos;
    ULONG CcLazyWritePages;
    ULONG CcDataFlushes;
    ULONG CcDataPages;
    ULONG ContextSwitches;
    ULONG FirstLevelTbFills;
    ULONG SecondLevelTbFills;
    ULONG SystemCalls;
    UINT64 CcTotalDirtyPages;
    UINT64 CcDirtyPageThreshold;
} KI_SYSTEM_PERFORMANCE;
_Static_assert(sizeof(KI_SYSTEM_PERFORMANCE)==328,"Win7 performance ABI");
_Static_assert(__builtin_offsetof(KI_SYSTEM_PERFORMANCE,AvailablePages)==44,"memory counter offset");
_Static_assert(__builtin_offsetof(KI_SYSTEM_PERFORMANCE,ContextSwitches)==296,"scheduler counter offset");
_Static_assert(__builtin_offsetof(KI_SYSTEM_PERFORMANCE,CcTotalDirtyPages)==312,"Win7 extension offset");
static NTSTATUS KiQuerySystemPerformance(void *buffer,ULONG length,ULONG *returned)
{
    const ULONG required=sizeof(KI_SYSTEM_PERFORMANCE);
    if(returned){
        if(!MmProbeForWrite((UINT64)returned,sizeof(*returned)))return STATUS_ACCESS_VIOLATION;
        *returned=required;
    }
    if(length<required)return STATUS_INFO_LENGTH_MISMATCH;
    if(!MmProbeForWrite((UINT64)buffer,required))return STATUS_ACCESS_VIOLATION;
    KI_SYSTEM_PERFORMANCE value={0};
    MM_MEMORY_USAGE memory;MmQueryMemoryUsage(&memory);
    EX_POOL_USAGE pool;ExQueryPoolUsage(&pool);
    IO_STATISTICS io;IoQueryStatistics(&io);
    PKPRCB cpu=KeGetProcessorBlock(0);
    value.IdleProcessTime=__atomic_load_n(&cpu->IdleTime,__ATOMIC_RELAXED);
    value.IoReadTransferCount=io.ReadBytes;
    value.IoWriteTransferCount=io.WriteBytes;
    value.IoReadOperationCount=(ULONG)io.ReadOperations;
    value.IoWriteOperationCount=(ULONG)io.WriteOperations;
    value.IoOtherOperationCount=(ULONG)__atomic_load_n(&cpu->OtherIoOperations,__ATOMIC_RELAXED);
    value.AvailablePages=(ULONG)memory.AvailablePages;
    value.CommittedPages=(ULONG)memory.CommittedPages;
    value.CommitLimit=(ULONG)memory.CommitLimit;
    value.PeakCommitment=(ULONG)memory.PeakCommitment;
    value.PageFaultCount=(ULONG)__atomic_load_n(&cpu->PageFaults,__ATOMIC_RELAXED);
    value.NonPagedPoolPages=(ULONG)pool.Pages;
    value.NonPagedPoolAllocs=(ULONG)pool.Allocations;
    value.NonPagedPoolFrees=(ULONG)pool.Frees;
    value.ContextSwitches=__atomic_load_n(&cpu->ContextSwitches,__ATOMIC_RELAXED);
    value.SystemCalls=(ULONG)__atomic_load_n(&cpu->SystemCalls,__ATOMIC_RELAXED);
    *(KI_SYSTEM_PERFORMANCE *)buffer=value;
    return STATUS_SUCCESS;
}

/* The Win7 Superfetch envelope also carries a read-only physical memory list
 * query. Svindows has one node and does not maintain standby/modified lists. */
typedef struct {
    ULONG Version, Magic, Class, Reserved;
    UINT64 Data;
    ULONG DataLength, Reserved2;
} KI_SUPERFETCH_REQUEST;

typedef struct {
    ULONG Version, Size, NodeCount, Reserved;
    UINT64 Node, StandbyLow, StandbyMedium, StandbyHigh, FreePages, ModifiedPages;
} KI_MEMORY_LIST;
_Static_assert(sizeof(KI_SUPERFETCH_REQUEST) == 32, "Win7 Superfetch request ABI");
_Static_assert(sizeof(KI_MEMORY_LIST) == 64, "Win7 memory list ABI");

static NTSTATUS KiQuerySuperfetchMemoryList(UINT64 buffer, ULONG length, ULONG *returned)
{
    if (length != sizeof(KI_SUPERFETCH_REQUEST)) return STATUS_INFO_LENGTH_MISMATCH;
    if (!MmProbeForRead(buffer, sizeof(KI_SUPERFETCH_REQUEST))) return STATUS_ACCESS_VIOLATION;
    KI_SUPERFETCH_REQUEST request;
    memcpy(&request, (const void *)buffer, sizeof(request));
    if (request.Version != 45 || request.Magic != 0x6b756843)
        return STATUS_INVALID_PARAMETER;
    if (request.Class != 16) return STATUS_NOT_SUPPORTED;

    const ULONG required = sizeof(KI_MEMORY_LIST);
    if (returned) {
        if (!MmProbeForWrite((UINT64)returned, sizeof(*returned))) return STATUS_ACCESS_VIOLATION;
        *returned = required;
    }
    if (request.DataLength < required) return STATUS_BUFFER_OVERFLOW;
    if (!MmProbeForWrite(request.Data, required)) return STATUS_ACCESS_VIOLATION;
    MM_MEMORY_USAGE memory;
    MmQueryMemoryUsage(&memory);
    KI_MEMORY_LIST result = {
        .Version = 1, .Size = required, .NodeCount = 1,
        .FreePages = memory.AvailablePages
    };
    memcpy((void *)request.Data, &result, sizeof(result));
    return STATUS_SUCCESS;
}
