#include <assert.h>
#include <stdio.h>
#include <svindows/Ke.h>
static KPRCB cpu;
PKPRCB KeGetCurrentProcessorBlock(void){return &cpu;}
#include "../../kernel/ke/CpuAccounting.inc"
int main(void)
{
    KPROCESS process={0};KTHREAD first={0},second={0},idle={0};
    first.Process=&process;second.Process=&process;cpu.IdleThread=&idle;
    cpu.CurrentThread=&first;
    KeAccountClockTick(TRUE);KeAccountClockTick(TRUE);KeAccountClockTick(FALSE);
    assert(first.UserTime==200000&&first.KernelTime==100000);
    cpu.CurrentThread=&second;KeAccountClockTick(FALSE);
    assert(process.UserTime==200000&&process.KernelTime==200000);
    assert(second.KernelTime==100000&&!second.UserTime);
    cpu.CurrentThread=&idle;KeAccountClockTick(FALSE);KeAccountClockTick(FALSE);
    assert(cpu.IdleTime==200000&&cpu.KernelTime==400000&&cpu.UserTime==200000);
    assert(!idle.KernelTime&&process.KernelTime==200000);
    /* A terminated thread's historical process time must remain counted. */
    first=(KTHREAD){0};assert(process.UserTime==200000&&process.KernelTime==200000);
    cpu.CurrentThread=0;KeAccountClockTick(TRUE);assert(cpu.UserTime==200000);
    cpu.CurrentThread=&second;cpu.DpcRunning=TRUE;KeAccountClockTick(FALSE);
    assert(cpu.DpcTime==100000&&cpu.KernelTime==500000&&process.KernelTime==300000);
    puts("PASS user/kernel/idle tick accounting and process totals across thread teardown");
}
