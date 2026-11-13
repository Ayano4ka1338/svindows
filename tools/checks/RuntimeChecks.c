/* Test native critical-section code with host threads and a mocked TEB. */
#include "UserRuntimeHost.h"
#include "../../user/w/ntdll/Rtl/CriticalSections.inc"

static RTL_CRITICAL_SECTION_NT lock;
static unsigned counter;
static RTL_CRITICAL_SECTION_NT blocking_lock;
static NTSTATUS blocking_status;
static void *blocked_acquire(void *fail)
{
    current_tid=100;host_fail_semaphore=fail!=NULL;
    blocking_status=RtlEnterCriticalSection(&blocking_lock);
    if(blocking_status==STATUS_SUCCESS)assert(RtlLeaveCriticalSection(&blocking_lock)==STATUS_SUCCESS);
    return NULL;
}
static void check_blocking_failure(void)
{
    assert(RtlInitializeCriticalSection(&blocking_lock)==STATUS_SUCCESS);
    assert(RtlEnterCriticalSection(&blocking_lock)==STATUS_SUCCESS);
    pthread_t worker;assert(!pthread_create(&worker,NULL,blocked_acquire,(void *)1));
    assert(!pthread_join(worker,NULL));assert(blocking_status==STATUS_NO_MEMORY);
    assert(blocking_lock.LockCount==0&&blocking_lock.OwningThread==(HANDLE)current_tid);
    assert(RtlDeleteCriticalSection(&blocking_lock)!=STATUS_SUCCESS);
    assert(!pthread_create(&worker,NULL,blocked_acquire,NULL));
    struct timespec pause={0,1000000};unsigned count=0;
    while(!atomic_load(&host_waiters)){assert(count++<5000);nanosleep(&pause,NULL);}
    assert(RtlLeaveCriticalSection(&blocking_lock)==STATUS_SUCCESS);
    assert(!pthread_join(worker,NULL)&&blocking_status==STATUS_SUCCESS);
    assert(RtlDeleteCriticalSection(&blocking_lock)==STATUS_SUCCESS);
}
static void *increment(void *id)
{
    current_tid = (ULONG_PTR)id;
    for (unsigned i = 0; i < 20000; i++) {
        assert(RtlEnterCriticalSection(&lock) == STATUS_SUCCESS);
        assert(RtlEnterCriticalSection(&lock) == STATUS_SUCCESS);
        counter++;
        assert(RtlLeaveCriticalSection(&lock) == STATUS_SUCCESS);
        assert(RtlLeaveCriticalSection(&lock) == STATUS_SUCCESS);
    }
    return NULL;
}

int main(void)
{
    assert(sizeof(lock) == 40);
    assert(RtlInitializeCriticalSection(&lock) == STATUS_SUCCESS);
    assert(RtlTryEnterCriticalSection(&lock));
    assert(RtlTryEnterCriticalSection(&lock));
    current_tid = 2;
    assert(!RtlTryEnterCriticalSection(&lock));
    assert(RtlLeaveCriticalSection(&lock) != STATUS_SUCCESS);
    check_blocking_failure();
    current_tid = 1;
    assert(RtlLeaveCriticalSection(&lock) == STATUS_SUCCESS);
    assert(RtlLeaveCriticalSection(&lock) == STATUS_SUCCESS);
    assert(RtlLeaveCriticalSection(&lock) != STATUS_SUCCESS);
    pthread_t threads[4];
    for (ULONG_PTR i = 0; i < 4; i++)
        assert(pthread_create(&threads[i], NULL, increment, (void *)(i + 10)) == 0);
    for (unsigned i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(counter == 80000);
    assert(atomic_load(&host_wait_calls)>0);
    assert(RtlDeleteCriticalSection(&lock) == STATUS_SUCCESS);
    host_runtime_cleanup();
    puts("PASS native critical section: layout, ownership, recursion, 80000 increments");
    return 0;
}
