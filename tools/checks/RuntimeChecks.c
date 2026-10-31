/* Test native critical-section code with host threads and a mocked TEB. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <nt/Ntdef.h>
#include <nt/Ntstatus.h>

static _Thread_local ULONGLONG current_tid = 1;
static ULONGLONG read_teb_qword(ULONGLONG offset)
{
    assert(offset == 0x48);
    return current_tid;
}
#include "../../user/w/ntdll/Rtl/CriticalSections.inc"

static RTL_CRITICAL_SECTION_NT lock;
static unsigned counter;
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
    current_tid = 1;
    assert(RtlLeaveCriticalSection(&lock) == STATUS_SUCCESS);
    assert(RtlLeaveCriticalSection(&lock) == STATUS_SUCCESS);
    assert(RtlLeaveCriticalSection(&lock) != STATUS_SUCCESS);
    pthread_t threads[4];
    for (ULONG_PTR i = 0; i < 4; i++)
        assert(pthread_create(&threads[i], NULL, increment, (void *)(i + 10)) == 0);
    for (unsigned i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(counter == 80000);
    assert(RtlDeleteCriticalSection(&lock) == STATUS_SUCCESS);
    puts("PASS native critical section: layout, ownership, recursion, 80000 increments");
    return 0;
}
