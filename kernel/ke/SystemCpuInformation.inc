/* Win7 x64 SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION. The BSP's global clock
 * sends a scheduling/accounting IPI to each online AP. */
typedef struct {
    UINT64 IdleTime, KernelTime, UserTime, DpcTime, InterruptTime;
    ULONG InterruptCount, Spare;
} KI_PROCESSOR_PERFORMANCE;
_Static_assert(sizeof(KI_PROCESSOR_PERFORMANCE)==48,"processor performance ABI");
static ULONG KiCountOnlineProcessors(void)
{
    ULONG count = 1; /* the bootstrap processor */
    UINT32 discovered = HalGetProcessorCount();
    if (!discovered) discovered = 1;
    if (discovered > KE_MAXIMUM_PROCESSORS)
        discovered = KE_MAXIMUM_PROCESSORS;
    for (UINT32 i = 1; i < discovered; ++i) {
        PKPRCB cpu = KeGetProcessorBlock(i);
        if (cpu && cpu->StartupState == 2 &&
            __atomic_load_n(&cpu->StartupRelease, __ATOMIC_ACQUIRE))
            count++;
    }
    return count;
}
static NTSTATUS KiQueryProcessorPerformance(void *buffer,ULONG length,ULONG *returned)
{
    UINT32 discovered = HalGetProcessorCount();
    if (!discovered) discovered = 1;
    if (discovered > KE_MAXIMUM_PROCESSORS)
        discovered = KE_MAXIMUM_PROCESSORS;
    ULONG count = KiCountOnlineProcessors();
    ULONG required = count * sizeof(KI_PROCESSOR_PERFORMANCE);
    if(returned){
        if(!MmProbeForWrite((UINT64)returned,sizeof(*returned)))return STATUS_ACCESS_VIOLATION;
        *returned=required;
    }
    if(length<required)return STATUS_INFO_LENGTH_MISMATCH;
    if(!MmProbeForWrite((UINT64)buffer,required))return STATUS_ACCESS_VIOLATION;
    KI_PROCESSOR_PERFORMANCE *out = buffer;
    ULONG row = 0;
    for (UINT32 i = 0; i < discovered; ++i) {
        PKPRCB cpu = KeGetProcessorBlock(i);
        if (!cpu || (i && (cpu->StartupState != 2 ||
            !__atomic_load_n(&cpu->StartupRelease, __ATOMIC_ACQUIRE))))
            continue;
        KI_PROCESSOR_PERFORMANCE value = {0};
        value.IdleTime = __atomic_load_n(&cpu->IdleTime,__ATOMIC_RELAXED);
        value.KernelTime = __atomic_load_n(&cpu->KernelTime,__ATOMIC_RELAXED);
        value.UserTime = __atomic_load_n(&cpu->UserTime,__ATOMIC_RELAXED);
        value.DpcTime = __atomic_load_n(&cpu->DpcTime,__ATOMIC_RELAXED);
        value.InterruptCount = __atomic_load_n(&cpu->InterruptCount,__ATOMIC_RELAXED);
        out[row++] = value;
    }
    return STATUS_SUCCESS;
}
