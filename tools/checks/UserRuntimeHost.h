/* Native-service fixture for the actual user-runtime code. Host semaphores
 * block on condition variables; VM reservations are inaccessible until commit. */
#ifndef SV_USER_RUNTIME_HOST_H
#define SV_USER_RUNTIME_HOST_H
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
typedef void *HANDLE;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint16_t WCHAR;
typedef uint32_t DWORD,ULONG,UINT;
typedef int32_t LONG,NTSTATUS;
typedef uint64_t ULONGLONG;
typedef uintptr_t ULONG_PTR;
typedef int64_t LONGLONG;
typedef unsigned long long SIZE_T;
typedef int BOOL,BOOLEAN;
#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(s) ((NTSTATUS)(s)>=0)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_NO_MEMORY ((NTSTATUS)0xc0000017u)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000du)
#define __declspec(x)
typedef void *LPVOID;
typedef struct { BYTE Padding[0x30];void *ProcessHeap; } HOST_PEB;
static HOST_PEB host_peb;
static _Thread_local ULONGLONG current_tid=1;
static _Thread_local DWORD host_last_error;
static _Thread_local int host_fail_semaphore;
static _Thread_local int host_fail_vm;
static _Thread_local jmp_buf host_termination_jump;
static _Thread_local int host_expect_termination;
static _Thread_local NTSTATUS host_termination_status;
static _Atomic unsigned host_wait_calls,host_waiters,host_sem_created,host_sem_closed;
static _Atomic unsigned host_vm_reserved,host_vm_released;
static _Atomic unsigned host_raised_exceptions;
static ULONGLONG read_teb_qword(ULONGLONG offset)
{ if(offset==0x48)return current_tid;assert(offset==0x60);return (ULONGLONG)(uintptr_t)&host_peb; }
static void write_teb_dword(ULONGLONG offset,DWORD value)
{ assert(offset==0x68);host_last_error=value; }
static HOST_PEB *NtCurrentPeb(void) {return &host_peb;}
static void SetLastError(DWORD error) {host_last_error=error;}
static DWORD ntstatus_to_win32(NTSTATUS status)
{return status==(NTSTATUS)0xc0000008u?6:status==STATUS_NO_MEMORY?8:status==(NTSTATUS)0xc0000023u?122:87;}

typedef struct HOST_SEMAPHORE {
    struct HOST_SEMAPHORE *Next;
    pthread_mutex_t Mutex;
    pthread_cond_t Condition;
    LONG Count,Maximum;
    unsigned Waiters;
} HOST_SEMAPHORE;
static pthread_mutex_t host_semaphore_registry_mutex=PTHREAD_MUTEX_INITIALIZER;
static HOST_SEMAPHORE *host_semaphores;

NTSTATUS NtCreateSemaphore(HANDLE *out,ULONG access,void *attributes,LONG initial,LONG maximum)
{
    assert(access==0x001f0003u && !attributes);
    if(host_fail_semaphore){host_fail_semaphore--;return STATUS_NO_MEMORY;}
    if(!out || maximum<=0 || initial<0 || initial>maximum)return STATUS_INVALID_PARAMETER;
    HOST_SEMAPHORE *sem=calloc(1,sizeof(*sem));assert(sem);
    assert(!pthread_mutex_init(&sem->Mutex,0));assert(!pthread_cond_init(&sem->Condition,0));
    sem->Count=initial;sem->Maximum=maximum;
    pthread_mutex_lock(&host_semaphore_registry_mutex);sem->Next=host_semaphores;host_semaphores=sem;
    pthread_mutex_unlock(&host_semaphore_registry_mutex);
    atomic_fetch_add(&host_sem_created,1);*out=sem;return STATUS_SUCCESS;
}
NTSTATUS NtWaitForSingleObject(HANDLE handle,int alertable,const LONGLONG *timeout)
{
    assert(!alertable && !timeout);HOST_SEMAPHORE *sem=handle;assert(sem);
    atomic_fetch_add(&host_wait_calls,1);
    pthread_mutex_lock(&sem->Mutex);sem->Waiters++;atomic_fetch_add(&host_waiters,1);
    while(!sem->Count)pthread_cond_wait(&sem->Condition,&sem->Mutex);
    sem->Count--;sem->Waiters--;atomic_fetch_sub(&host_waiters,1);
    pthread_mutex_unlock(&sem->Mutex);return STATUS_SUCCESS;
}
NTSTATUS NtReleaseSemaphore(HANDLE handle,LONG count,LONG *previous)
{
    HOST_SEMAPHORE *sem=handle;if(!sem || count<=0)return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&sem->Mutex);
    if(count>sem->Maximum-sem->Count){pthread_mutex_unlock(&sem->Mutex);return STATUS_INVALID_PARAMETER;}
    if(previous)*previous=sem->Count;sem->Count+=count;
    for(LONG i=0;i<count;++i)pthread_cond_signal(&sem->Condition);
    pthread_mutex_unlock(&sem->Mutex);return STATUS_SUCCESS;
}
NTSTATUS NtClose(HANDLE handle)
{
    HOST_SEMAPHORE *sem=handle;if(!sem)return (NTSTATUS)0xc0000008u;
    pthread_mutex_lock(&host_semaphore_registry_mutex);
    HOST_SEMAPHORE **link=&host_semaphores;while(*link && *link!=sem)link=&(*link)->Next;
    assert(*link==sem);*link=sem->Next;
    pthread_mutex_unlock(&host_semaphore_registry_mutex);
    assert(!sem->Waiters);assert(!pthread_cond_destroy(&sem->Condition));assert(!pthread_mutex_destroy(&sem->Mutex));
    free(sem);atomic_fetch_add(&host_sem_closed,1);return STATUS_SUCCESS;
}

typedef struct HOST_VM {
    struct HOST_VM *Next;
    void *Base;
    SIZE_T Bytes;
} HOST_VM;
static HOST_VM *host_vms;
static pthread_mutex_t host_vm_mutex=PTHREAD_MUTEX_INITIALIZER;
NTSTATUS NtAllocateVirtualMemory(HANDLE process,void **base,ULONGLONG zero_bits,
    ULONGLONG *size,DWORD type,DWORD protection)
{
    assert(process==(HANDLE)-1LL && !zero_bits && (protection==4 || protection==0x40));
    if(host_fail_vm){if(host_fail_vm>0)host_fail_vm--;return STATUS_NO_MEMORY;}
    if(!base || !size || !*size || *size>SIZE_MAX-4095)return STATUS_INVALID_PARAMETER;
    SIZE_T bytes=((SIZE_T)*size+4095)&~(SIZE_T)4095;
    pthread_mutex_lock(&host_vm_mutex);
    if(type&0x2000) {
        assert(!*base);void *memory=mmap(0,bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(memory==MAP_FAILED){pthread_mutex_unlock(&host_vm_mutex);return STATUS_NO_MEMORY;}
        HOST_VM *vm=malloc(sizeof(*vm));assert(vm);*vm=(HOST_VM){host_vms,memory,bytes};host_vms=vm;
        *base=memory;atomic_fetch_add(&host_vm_reserved,1);
    }
    if(type&0x1000) {
        HOST_VM *vm=host_vms;
        while(vm && !((BYTE *)*base>=(BYTE *)vm->Base && (BYTE *)*base+bytes<=(BYTE *)vm->Base+vm->Bytes))vm=vm->Next;
        assert(vm && !((uintptr_t)*base&4095));
        assert(!mprotect(*base,bytes,PROT_READ|PROT_WRITE|(protection==0x40?PROT_EXEC:0)));
    }
    *size=bytes;pthread_mutex_unlock(&host_vm_mutex);return STATUS_SUCCESS;
}
NTSTATUS NtFreeVirtualMemory(HANDLE process,void **base,ULONGLONG *size,ULONG type)
{
    assert(process==(HANDLE)-1LL && base && size && !*size && type==0x8000);
    pthread_mutex_lock(&host_vm_mutex);HOST_VM **link=&host_vms;
    while(*link && (*link)->Base!=*base)link=&(*link)->Next;
    assert(*link);HOST_VM *vm=*link;*link=vm->Next;
    assert(!munmap(vm->Base,vm->Bytes));free(vm);atomic_fetch_add(&host_vm_released,1);
    pthread_mutex_unlock(&host_vm_mutex);return STATUS_SUCCESS;
}
NTSTATUS NtTerminateProcess(HANDLE process,NTSTATUS status)
{
    assert(process==(HANDLE)-1LL && host_expect_termination);
    host_termination_status=status;longjmp(host_termination_jump,1);
}
static void rtl_heap_raise(NTSTATUS status)
{assert(status==STATUS_NO_MEMORY);atomic_fetch_add(&host_raised_exceptions,1);}
static void host_runtime_cleanup(void)
{
    while(host_semaphores)NtClose(host_semaphores);
    while(host_vms){void *base=host_vms->Base;ULONGLONG zero=0;NtFreeVirtualMemory((HANDLE)-1LL,&base,&zero,0x8000);}
    assert(atomic_load(&host_sem_created)==atomic_load(&host_sem_closed));
    assert(atomic_load(&host_vm_reserved)==atomic_load(&host_vm_released));
}
#endif
