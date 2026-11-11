#include <assert.h>
#include <stdio.h>
#include <svindows/Ke.h>
#include <svindows/Ob.h>
static unsigned signals;
void KiSignalObjectWithSchedulerLock(PDISPATCHER_HEADER header)
{ assert(header->SignalState == 1); ++signals; }
#include "../../kernel/ke/ThreadTermination.inc"
int main(void)
{
    struct { DISPATCHER_HEADER Header; KPROCESS Process; } process = {0};
    process.Process.Object = &process;
    process.Process.HandleTable = (void *)0xffffffffc1234560ull;
    process.Process.DirectoryTableBase = 0x12345000;
    KTHREAD thread = {0};
    thread.Process = &process.Process;
    thread.ProcessMainThread = TRUE;
    thread.TerminationObject = &process.Header;
    thread.ExitStatus = 0;
    assert(KepPublishThreadTerminationWithSchedulerLock(&thread) == &process);
    assert(thread.State == ThreadStateTerminated && !thread.TerminationObject);
    assert(process.Process.HandleTable == (void *)0xffffffffc1234560ull);
    assert(process.Process.DirectoryTableBase == 0x12345000 && signals == 1);
    struct { DISPATCHER_HEADER Header; PKTHREAD Thread; NTSTATUS Exit; } record = {0};
    record.Thread = &thread;
    thread.ProcessMainThread = FALSE;
    thread.TerminationObject = &record.Header;
    thread.ExitStatus = (NTSTATUS)0xc0000001u;
    assert(KepPublishThreadTerminationWithSchedulerLock(&thread) == &record);
    assert(record.Exit == thread.ExitStatus && record.Thread == &thread && signals == 2);
    assert(!KepPublishThreadTerminationWithSchedulerLock(&thread) && signals == 2);
    puts("PASS main-thread exit preserves process handles; ordinary thread records retain exit status");
}
