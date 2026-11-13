/* Dispatcher mutant lifetime checks. Embedded mutants are not Object Manager
 * bodies; executive mutants explicitly name the object retained by ownership. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <svindows/Ke.h>
#include <svindows/Ob.h>

static KTHREAD current;
static ULONG references, dereferences;
static PVOID last_reference, last_dereference;

PKTHREAD KeGetCurrentThread(void) { return &current; }
KIRQL KeGetCurrentIrql(void) { return PASSIVE_LEVEL; }
UINT64 KeGetTickCount(void) { return 0; }
UINT64 KiAcquireSchedulerLock(void) { return 0; }
void KiReleaseSchedulerLock(UINT64 flags) { (void)flags; }
void KiReadyThreadWithSchedulerLock(PKTHREAD thread)
{ thread->State = ThreadStateReady; }
void KiBlockCurrentThreadWithSchedulerLock(UINT64 flags)
{ (void)flags; abort(); }
void ObReferenceObject(POBJECT object)
{ references++; last_reference = object; }
void ObDereferenceObject(POBJECT object)
{ dereferences++; last_dereference = object; }
NORETURN void KeBugCheck(ULONG code, const char *message)
{ fprintf(stderr, "BUGCHECK %x: %s\n", code, message); abort(); }

#include "../../kernel/ke/Dispatcher.c"

int main(void)
{
    memset(&current, 0, sizeof(current));
    current.State = ThreadStateRunning;
    InitializeListHead(&current.OwnedMutantListHead);

    struct {
        UINT8 Prefix[64];
        KMUTANT Mutant;
        UINT64 Tail;
    } embedded;
    memset(&embedded, 0xA5, sizeof(embedded));
    UINT8 prefix[sizeof(embedded.Prefix)];
    memcpy(prefix, embedded.Prefix, sizeof(prefix));
    embedded.Tail = 0x1122334455667788ULL;
    KeInitializeMutant(&embedded.Mutant, FALSE);
    assert(KeWaitForSingleObjectTimeout(&embedded.Mutant.Header, 0) ==
           STATUS_SUCCESS);
    assert(!references && !dereferences);
    assert(!memcmp(prefix, embedded.Prefix, sizeof(prefix)) &&
           embedded.Tail == 0x1122334455667788ULL);
    assert(KeReleaseMutant(&embedded.Mutant, NULL));
    assert(!references && !dereferences);

    KMUTANT executive;
    UINT64 containing_object = 0;
    KeInitializeMutantWithReference(&executive, FALSE, &containing_object);
    assert(KeWaitForSingleObjectTimeout(&executive.Header, 0) == STATUS_SUCCESS);
    assert(references == 1 && last_reference == &containing_object);
    assert(KeReleaseMutant(&executive, NULL));
    assert(dereferences == 1 && last_dereference == &containing_object);
    assert(IsListEmpty(&current.OwnedMutantListHead));
    puts("PASS embedded and executive mutant ownership use explicit object lifetime");
    return 0;
}
