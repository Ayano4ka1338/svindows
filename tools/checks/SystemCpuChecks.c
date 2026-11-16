#include <assert.h>
#include <stdio.h>
#include <svindows/Ke.h>
#include <svindows/Hal.h>
#include <nt/Ntstatus.h>

static KPRCB cpus[4];
static UINT32 discovered_cpus = 1;
PKPRCB KeGetProcessorBlock(ULONG i)
{
    assert(i < 4);
    return &cpus[i];
}
UINT32 HalGetProcessorCount(void) { return discovered_cpus; }
BOOLEAN MmProbeForWrite(UINT64 p,SIZE_T n){return p>=4096&&n!=0;}
#include "../../kernel/ke/SystemCpuInformation.inc"

int main(void)
{
    struct { KI_PROCESSOR_PERFORMANCE sample; UINT64 guard; } out={0};
    out.guard=0x123456789abcdef0ULL;
    ULONG size=0;
    assert(KiQueryProcessorPerformance(&out,0,&size)==STATUS_INFO_LENGTH_MISMATCH&&size==48);
    assert(KiQueryProcessorPerformance(0,48,0)==STATUS_ACCESS_VIOLATION);
    assert(KiQueryProcessorPerformance(&out,48,(ULONG *)1)==STATUS_ACCESS_VIOLATION);

    cpus[0].IdleTime=300000; cpus[0].KernelTime=900000;
    cpus[0].UserTime=500000; cpus[0].DpcTime=100000; cpus[0].InterruptCount=121;
    assert(KiQueryProcessorPerformance(&out,48,&size)==STATUS_SUCCESS&&size==48);
    assert(out.sample.IdleTime==300000&&out.sample.KernelTime==900000&&out.sample.UserTime==500000);
    assert(out.sample.DpcTime==100000&&out.sample.InterruptCount==121&&!out.sample.Spare);
    assert(out.guard==0x123456789abcdef0ULL);
    cpus[0].UserTime+=100000;
    assert(KiQueryProcessorPerformance(&out,48,0)==STATUS_SUCCESS&&out.sample.UserTime==600000);

    /* Online APs are included; CPUs still booting or not released are not. */
    discovered_cpus=4;
    cpus[1].StartupState=2; cpus[1].StartupRelease=1;
    cpus[2].StartupState=1; cpus[2].StartupRelease=0;
    cpus[3].StartupState=2; cpus[3].StartupRelease=1;
    cpus[1].KernelTime=200000; cpus[3].IdleTime=400000;
    KI_PROCESSOR_PERFORMANCE samples[3]={{0}};
    assert(KiQueryProcessorPerformance(samples,sizeof(samples),&size)==STATUS_SUCCESS);
    assert(size==sizeof(samples));
    assert(samples[0].UserTime==600000);
    assert(samples[1].KernelTime==200000);
    assert(samples[2].IdleTime==400000);

    discovered_cpus=0;
    assert(KiQueryProcessorPerformance(&out,48,&size)==STATUS_SUCCESS&&size==48);
    puts("PASS processor performance ABI, per-CPU counters, online CPU filtering and output validation");
}
