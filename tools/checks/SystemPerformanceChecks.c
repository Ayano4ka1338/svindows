#include <assert.h>
#include <stdio.h>
#include <svindows/Ke.h>
#include <svindows/Mm.h>
#include <svindows/Ex.h>
#include <svindows/Io.h>
#include <nt/Ntstatus.h>
static KPRCB cpu;
static MM_MEMORY_USAGE memory;
PKPRCB KeGetProcessorBlock(ULONG i){assert(!i);return &cpu;}
BOOLEAN MmProbeForWrite(UINT64 p,UINT64 n){return p>=4096&&n;}
void MmQueryMemoryUsage(MM_MEMORY_USAGE *u){*u=memory;}
void ExQueryPoolUsage(EX_POOL_USAGE *u){*u=(EX_POOL_USAGE){40,800,300};}
void IoQueryStatistics(IO_STATISTICS *u){*u=(IO_STATISTICS){12345,67890,23,42};}
#include "../../kernel/ke/SystemPerformance.inc"
int main(void)
{
    struct{KI_SYSTEM_PERFORMANCE p;UINT64 guard;}out={0};out.guard=0x12345678;
    ULONG size=0;assert(KiQuerySystemPerformance(&out,0,&size)==STATUS_INFO_LENGTH_MISMATCH&&size==328);
    assert(KiQuerySystemPerformance(0,328,0)==STATUS_ACCESS_VIOLATION);
    assert(KiQuerySystemPerformance(&out,328,(ULONG *)1)==STATUS_ACCESS_VIOLATION);
    memory=(MM_MEMORY_USAGE){1000,200,1200,250};
    cpu.IdleTime=800000;cpu.PageFaults=13;cpu.SystemCalls=600;cpu.ContextSwitches=330;cpu.OtherIoOperations=99;
    assert(KiQuerySystemPerformance(&out,328,&size)==STATUS_SUCCESS&&size==328);
    assert(out.p.AvailablePages==1000&&out.p.CommittedPages==200&&out.p.CommitLimit==1200&&out.p.PeakCommitment==250);
    assert(out.p.NonPagedPoolPages==40&&out.p.NonPagedPoolAllocs==800&&out.p.NonPagedPoolFrees==300);
    assert(out.p.IoReadTransferCount==12345&&out.p.IoWriteTransferCount==67890&&out.p.IoReadOperationCount==23&&out.p.IoWriteOperationCount==42);
    assert(out.p.SystemCalls==600&&out.p.ContextSwitches==330&&out.p.PageFaultCount==13&&out.p.IoOtherOperationCount==99);
    assert(out.guard==0x12345678);
    memory.AvailablePages-=10;memory.CommittedPages+=10;
    assert(KiQuerySystemPerformance(&out,328,0)==STATUS_SUCCESS&&out.p.AvailablePages==990&&out.p.CommittedPages==210);
    puts("PASS Win7 system performance layout, native snapshots, updated memory and output bounds");
}
