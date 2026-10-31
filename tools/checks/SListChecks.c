#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
#define __declspec(x)
#include "../../user/w/kernel32/SList.inc"

typedef struct __attribute__((aligned(16))) { SLIST_ENTRY_K Entry; } NODE;
static SLIST_HEADER_K head;
static NODE nodes[4][5000];
static NODE a, b;

static void *producer(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    for (unsigned i = 0; i < 5000; i++)
        InterlockedPushEntrySList(&head, &nodes[id][i].Entry);
    return NULL;
}

int main(void)
{
    InitializeSListHead(&head);
    assert(!InterlockedPushEntrySList(&head, &a.Entry));
    assert(InterlockedPushEntrySList(&head, &b.Entry) == &a.Entry);
    assert(QueryDepthSList(&head) == 2);
    assert(InterlockedPopEntrySList(&head) == &b.Entry);
    assert(InterlockedPopEntrySList(&head) == &a.Entry);
    assert(!InterlockedPopEntrySList(&head));
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++)
        assert(!pthread_create(&threads[i], NULL, producer, (void *)i));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(QueryDepthSList(&head) == 20000);
    SLIST_ENTRY_K *first = InterlockedFlushSList(&head);
    unsigned count = 0;
    for (SLIST_ENTRY_K *p = first; p; p = p->Next) count++;
    assert(count == 20000 && QueryDepthSList(&head) == 0);
    head.Alignment = 0; head.Region = 2; /* initialized Header8 */
    assert(!InterlockedPushEntrySList(&head, &a.Entry));
    assert(InterlockedPopEntrySList(&head) == &a.Entry);
    puts("PASS x64 SList order, depth, flush, Header8 and concurrent pushes");
    return 0;
}
