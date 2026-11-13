/* First input-idle state belongs to a process, not to a fixed delay. The
 * process object remains referenced while either idle or exit can wake us. */
#include <svindows/Ke.h>
#include <svindows/Ps.h>
#include <svindows/Ob.h>
#include <svindows/Mm.h>

static void W32kInputIdleError(ULONG error)
{
    PKTHREAD thread=KeGetCurrentThread();
    if(thread && thread->UserGsBase && MmProbeForWrite(thread->UserGsBase+0x68,sizeof(error)))
        *(ULONG *)(thread->UserGsBase+0x68)=error;
}

void W32kMarkProcessInputIdle(PKPROCESS process)
{
    if(!process || process->Exiting)return;
    __atomic_store_n(&process->Win32InputIdleReached,1,__ATOMIC_RELEASE);
    PKEVENT event=__atomic_load_n(&process->Win32InputIdleEvent,__ATOMIC_ACQUIRE);
    if(event)KeSetEvent(event);
}

static PKEVENT W32kProcessInputIdleEvent(PKPROCESS process)
{
    PKEVENT event=__atomic_load_n(&process->Win32InputIdleEvent,__ATOMIC_ACQUIRE);
    if(event)return event;
    PKEVENT created;HANDLE temporary;
    if(!NT_SUCCESS(PsCreateNotificationEvent(&created,&temporary)))return NULL;
    ObCloseHandle(temporary);
    PVOID expected=NULL;
    if(__atomic_compare_exchange_n(&process->Win32InputIdleEvent,&expected,created,
        FALSE,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE))event=created;
    else {event=expected;ObDereferenceObject(created);}
    if(__atomic_load_n(&process->Win32InputIdleReached,__ATOMIC_ACQUIRE))KeSetEvent(event);
    return event;
}

void W32kCleanupProcessInputIdle(PKPROCESS process)
{
    if(!process)return;
    PKEVENT event=__atomic_exchange_n(&process->Win32InputIdleEvent,NULL,__ATOMIC_ACQ_REL);
    if(event)ObDereferenceObject(event);
}

UINT64 SvNtUserWaitForInputIdle(UINT64 *arguments)
{
    HANDLE handle=(HANDLE)arguments[0];ULONG timeout=(ULONG)arguments[1];
    POBJECT object=NULL;PKPROCESS process=NULL;
    if(handle==(HANDLE)(ULONG_PTR)-1) {
        PKTHREAD caller=KeGetCurrentThread();
        if(caller && caller->Process && caller->Process->Object) {
            process=caller->Process;object=process->Object;ObReferenceObject(object);
        }
    } else {
        NTSTATUS status=ObReferenceObjectByHandle(handle,0x00100000UL,NULL,&object);
        if(!NT_SUCCESS(status)) {
            W32kInputIdleError(status==STATUS_ACCESS_DENIED?5:6);return (UINT32)-1;
        }
        if(PsIsProcessObjectType(ObHeaderFromObject(object)->Type))process=PsGetProcessFromObject(object);
    }
    if(!process) {
        if(object)ObDereferenceObject(object);W32kInputIdleError(6);return (UINT32)-1;
    }
    if(process->ImageSubsystem==3 || __atomic_load_n(&process->Win32InputIdleReached,__ATOMIC_ACQUIRE)) {
        ObDereferenceObject(object);return 0;
    }
    if(process->Exiting) {
        ObDereferenceObject(object);W32kInputIdleError(1067);return (UINT32)-1;
    }
    PKEVENT idle=W32kProcessInputIdleEvent(process);
    if(!idle) {
        ObDereferenceObject(object);W32kInputIdleError(8);return (UINT32)-1;
    }
    UINT64 tick_milliseconds=KE_TIME_UNITS_PER_TICK/10000ULL;
    UINT64 ticks=timeout==(ULONG)-1 ? ~(UINT64)0 :
        ((UINT64)timeout+tick_milliseconds-1)/tick_milliseconds;
    PDISPATCHER_HEADER waits[2]={&idle->Header,(PDISPATCHER_HEADER)object};
    NTSTATUS status=KeWaitForMultipleObjects(2,waits,FALSE,ticks);
    BOOLEAN reached=__atomic_load_n(&process->Win32InputIdleReached,__ATOMIC_ACQUIRE)!=0;
    ObDereferenceObject(object);
    if(reached || status==STATUS_SUCCESS)return 0;
    if(status==STATUS_TIMEOUT)return 0x102;
    W32kInputIdleError(status==1?1067:6);return (UINT32)-1;
}
