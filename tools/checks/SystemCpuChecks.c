#include <assert.h>
#include <stdio.h>
#include <svindows/Ke.h>
#include <nt/Ntstatus.h>
static KPRCB cpu;
PKPRCB KeGetProcessorBlock(ULONG i){assert(!i);return &cpu;}
BOOLEAN MmProbeForWrite(UINT64 p,SIZE_T n){return p>=4096&&n!=0;}
#include "../../kernel/ke/SystemCpuInformation.inc"
int main(void)
{
    struct{KI_PROCESSOR_PERFORMANCE sample;UINT64 guard;}out={0};out.guard=0x123456789abcdef0ULL;
    ULONG size=0;assert(KiQueryProcessorPerformance(&out,0,&size)==STATUS_INFO_LENGTH_MISMATCH&&size==48);
    assert(KiQueryProcessorPerformance(0,48,0)==STATUS_ACCESS_VIOLATION);
    assert(KiQueryProcessorPerformance(&out,48,(ULONG *)1)==STATUS_ACCESS_VIOLATION);
    cpu.IdleTime=300000;cpu.KernelTime=900000;cpu.UserTime=500000;cpu.DpcTime=100000;cpu.InterruptCount=121;
    assert(KiQueryProcessorPerformance(&out,48,&size)==STATUS_SUCCESS&&size==48);
    assert(out.sample.IdleTime==300000&&out.sample.KernelTime==900000&&out.sample.UserTime==500000);
    assert(out.sample.DpcTime==100000&&out.sample.InterruptCount==121&&!out.sample.Spare);
    assert(out.guard==0x123456789abcdef0ULL);
    cpu.UserTime+=100000;assert(KiQueryProcessorPerformance(&out,48,0)==STATUS_SUCCESS&&out.sample.UserTime==600000);
    puts("PASS processor performance ABI, native counters, length and output validation");
}
