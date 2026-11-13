/* Exercise the production first-idle broker with actual blocking host waits,
 * process/event object references, exit races and timeout/error cases. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <svindows/Ke.h>
#include <svindows/Ps.h>
#include <svindows/Ob.h>
#include <svindows/Win32k.h>
#include "../../kernel/win32k/InputIdle.inc"
typedef struct {DISPATCHER_HEADER Header;KPROCESS Process;} TEST_PROCESS;
static OBJECT_TYPE process_type,event_type;
static TEST_PROCESS *target;
static KPROCESS caller_process;
static KTHREAD caller_thread;
static BYTE teb[256];
static pthread_mutex_t wait_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_condition=PTHREAD_COND_INITIALIZER;
static unsigned event_allocated,event_deleted,wait_entered;
static BOOLEAN fail_event;
static TEST_PROCESS *new_process(ULONG subsystem)
{
    OBJECT_HEADER *header=calloc(1,sizeof(*header)+sizeof(TEST_PROCESS));assert(header);
    header->PointerCount=1;header->Type=&process_type;
    TEST_PROCESS *process=(void *)(header+1);process->Process.Object=process;process->Process.ImageSubsystem=subsystem;return process;
}
PKTHREAD KeGetCurrentThread(void){return &caller_thread;}
BOOLEAN MmProbeForWrite(UINT64 pointer,UINT64 bytes)
{return pointer>=(UINT64)teb && pointer+bytes<=(UINT64)(teb+sizeof(teb));}
NTSTATUS ObReferenceObjectByHandle(HANDLE handle,ACCESS_MASK access,POBJECT_TYPE type,POBJECT *out)
{
    assert(access==0x00100000 && !type);
    if(handle==(HANDLE)2)return STATUS_ACCESS_DENIED;
    if(handle!=(HANDLE)1)return STATUS_INVALID_HANDLE;
    *out=target;ObReferenceObject(target);return STATUS_SUCCESS;
}
void ObReferenceObject(POBJECT object){__atomic_add_fetch(&ObHeaderFromObject(object)->PointerCount,1,__ATOMIC_RELAXED);}
void ObDereferenceObject(POBJECT object)
{
    OBJECT_HEADER *header=ObHeaderFromObject(object);
    LONG count=__atomic_sub_fetch(&header->PointerCount,1,__ATOMIC_RELEASE);assert(count>=0);
    if(!count){if(header->Type==&event_type)event_deleted++;free(header);}
}
BOOLEAN PsIsProcessObjectType(POBJECT_TYPE type){return type==&process_type;}
PKPROCESS PsGetProcessFromObject(PVOID object){return &((TEST_PROCESS *)object)->Process;}
NTSTATUS PsCreateNotificationEvent(PKEVENT *out,HANDLE *temporary)
{
    if(fail_event)return STATUS_NO_MEMORY;
    OBJECT_HEADER *header=calloc(1,sizeof(*header)+sizeof(KEVENT));assert(header);
    header->PointerCount=2;header->Type=&event_type;*out=(void *)(header+1);*temporary=*out;event_allocated++;return STATUS_SUCCESS;
}
NTSTATUS ObCloseHandle(HANDLE event){ObDereferenceObject(event);return STATUS_SUCCESS;}
LONG KeSetEvent(PKEVENT event)
{
    pthread_mutex_lock(&wait_mutex);LONG old=event->Header.SignalState;event->Header.SignalState=1;
    pthread_cond_broadcast(&wait_condition);pthread_mutex_unlock(&wait_mutex);return old;
}
NTSTATUS KeWaitForMultipleObjects(ULONG count,PDISPATCHER_HEADER *objects,BOOLEAN all,UINT64 ticks)
{
    assert(count==2 && !all);pthread_mutex_lock(&wait_mutex);wait_entered++;
    struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);
    if(ticks!=~(UINT64)0){UINT64 nanos=ticks*10000000ULL;deadline.tv_sec+=(time_t)(nanos/1000000000);deadline.tv_nsec+=(long)(nanos%1000000000);if(deadline.tv_nsec>=1000000000){deadline.tv_nsec-=1000000000;deadline.tv_sec++;}}
    NTSTATUS status;
    for(;;) {
        if(objects[0]->SignalState){status=0;break;}if(objects[1]->SignalState){status=1;break;}
        if(!ticks){status=STATUS_TIMEOUT;break;}
        int result=ticks==~(UINT64)0?pthread_cond_wait(&wait_condition,&wait_mutex):pthread_cond_timedwait(&wait_condition,&wait_mutex,&deadline);
        if(result){status=STATUS_TIMEOUT;break;}
    }
    pthread_mutex_unlock(&wait_mutex);return status;
}
static void *mark_idle(void *unused)
{
    (void)unused;struct timespec delay={0,20000000};nanosleep(&delay,0);W32kMarkProcessInputIdle(&target->Process);return 0;
}
static void *exit_process(void *unused)
{
    (void)unused;struct timespec delay={0,20000000};nanosleep(&delay,0);
    pthread_mutex_lock(&wait_mutex);target->Process.Exiting=TRUE;target->Header.SignalState=1;pthread_cond_broadcast(&wait_condition);pthread_mutex_unlock(&wait_mutex);return 0;
}
static ULONG last_error(void){ULONG value;memcpy(&value,teb+0x68,sizeof(value));return value;}
static void destroy_target(void)
{W32kCleanupProcessInputIdle(&target->Process);assert(ObHeaderFromObject(target)->PointerCount==1);ObDereferenceObject(target);target=0;}
int main(void)
{
    caller_thread.Process=&caller_process;caller_thread.UserGsBase=(UINT64)teb;
    UINT64 arguments[]={1,0};target=new_process(3);
    assert(!SvNtUserWaitForInputIdle(arguments)&&!event_allocated);destroy_target();
    target=new_process(2);assert(SvNtUserWaitForInputIdle(arguments)==0x102);
    assert(event_allocated==1 && !target->Process.Win32InputIdleReached);
    pthread_t worker;arguments[1]=1000;assert(!pthread_create(&worker,0,mark_idle,0));
    assert(!SvNtUserWaitForInputIdle(arguments));assert(!pthread_join(worker,0));
    unsigned prior=wait_entered;assert(!SvNtUserWaitForInputIdle(arguments)&&wait_entered==prior);
    assert(target->Process.Win32InputIdleReached);destroy_target();
    target=new_process(2);assert(!pthread_create(&worker,0,exit_process,0));
    assert(SvNtUserWaitForInputIdle(arguments)==(UINT32)-1 && last_error()==1067);assert(!pthread_join(worker,0));destroy_target();
    target=new_process(2);fail_event=TRUE;
    assert(SvNtUserWaitForInputIdle(arguments)==(UINT32)-1 && last_error()==8 && !target->Process.Win32InputIdleEvent);fail_event=FALSE;
    arguments[0]=2;assert(SvNtUserWaitForInputIdle(arguments)==(UINT32)-1&&last_error()==5);
    arguments[0]=3;assert(SvNtUserWaitForInputIdle(arguments)==(UINT32)-1&&last_error()==6);
    destroy_target();assert(event_allocated==event_deleted);
    puts("PASS input idle: real empty-loop marker, blocking event, sticky first idle, console fast path, process exit, timeout and references");
    return 0;
}
