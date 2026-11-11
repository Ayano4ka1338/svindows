#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <svindows/Ps.h>
#include <svindows/Ob.h>
#include <nt/Ntstatus.h>
static KPROCESS self_process, target_process;
static KTHREAD caller;
static unsigned references, terminations, self_exits;
static jmp_buf exit_jump;
PKTHREAD KeGetCurrentThread(void) { return &caller; }
NTSTATUS ObReferenceObjectByHandle(HANDLE handle, ACCESS_MASK access,
    POBJECT_TYPE type, POBJECT *out)
{
    (void)type; assert(access == 1);
    if (handle != (HANDLE)4) return STATUS_ACCESS_DENIED;
    ++references; *out = &target_process; return STATUS_SUCCESS;
}
void ObDereferenceObject(POBJECT object)
{ assert(object == &target_process && references); --references; }
PKPROCESS PsGetProcessFromObject(PVOID object)
{ return object == &target_process ? &target_process : NULL; }
NTSTATUS KeTerminateProcess(PKPROCESS process, NTSTATUS status)
{
    assert(process == &target_process);
    ++terminations; process->Exiting = TRUE; process->ExitStatus = status;
    return STATUS_SUCCESS;
}
NORETURN void KeTerminateThread(void)
{ ++self_exits; longjmp(exit_jump, 1); }
#include "../../kernel/ps/ProcessTerminate.inc"
int main(void)
{
    caller.Process = &self_process;
    UINT64 arguments[] = {4, 42};
    assert(NtTerminateProcess(arguments) == STATUS_SUCCESS);
    assert(terminations == 1 && !references && target_process.ExitStatus == 42);
    assert(!self_process.Exiting && !self_exits);
    arguments[0] = 8;
    assert((NTSTATUS)NtTerminateProcess(arguments) == STATUS_ACCESS_DENIED);
    assert(terminations == 1 && !references);
    arguments[0] = ~(UINT64)0;
    if (!setjmp(exit_jump)) { NtTerminateProcess(arguments); assert(0); }
    assert(self_exits == 1 && caller.ExitStatus == 42 && caller.ProcessMainThread);
    puts("PASS process termination targets its handle, checks access and preserves the caller");
}
