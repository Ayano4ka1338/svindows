/* Win7 x64 SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION. The shared scheduler
 * currently executes on the BSP only (matching SystemBasicInformation's one
 * active processor); secondary processors are parked, not fake CPU samples. */
typedef struct {
    UINT64 IdleTime, KernelTime, UserTime, DpcTime, InterruptTime;
    ULONG InterruptCount, Spare;
} KI_PROCESSOR_PERFORMANCE;
_Static_assert(sizeof(KI_PROCESSOR_PERFORMANCE)==48,"processor performance ABI");
static NTSTATUS KiQueryProcessorPerformance(void *buffer,ULONG length,ULONG *returned)
{
    ULONG required=sizeof(KI_PROCESSOR_PERFORMANCE);
    if(returned){
        if(!MmProbeForWrite((UINT64)returned,sizeof(*returned)))return STATUS_ACCESS_VIOLATION;
        *returned=required;
    }
    if(length<required)return STATUS_INFO_LENGTH_MISMATCH;
    if(!MmProbeForWrite((UINT64)buffer,required))return STATUS_ACCESS_VIOLATION;
    PKPRCB cpu=KeGetProcessorBlock(0);
    KI_PROCESSOR_PERFORMANCE value={0};
    value.IdleTime=__atomic_load_n(&cpu->IdleTime,__ATOMIC_RELAXED);
    value.KernelTime=__atomic_load_n(&cpu->KernelTime,__ATOMIC_RELAXED);
    value.UserTime=__atomic_load_n(&cpu->UserTime,__ATOMIC_RELAXED);
    value.DpcTime=__atomic_load_n(&cpu->DpcTime,__ATOMIC_RELAXED);
    value.InterruptCount=__atomic_load_n(&cpu->InterruptCount,__ATOMIC_RELAXED);
    /* IRQ handlers run with IF clear and cannot receive an IRQ0 sample. */
    *(KI_PROCESSOR_PERFORMANCE *)buffer=value;
    return STATUS_SUCCESS;
}
