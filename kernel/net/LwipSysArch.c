/* lwIP OS port for the Svindows Executive.  All waits are scheduler waits;
 * no busy loops or fake timeouts are used.  Svindows currently has a UP
 * scheduler, so short queue critical sections use the same IRQ exclusion as
 * the rest of the kernel's dispatcher data structures. */
#include <svindows/Ke.h>
#include <svindows/Ex.h>
#include <svindows/Mm.h>
#include <svindows/Rtl.h>
#include <nt/Ntstatus.h>
#include <lwip/err.h>
#include <lwip/sys.h>

/* Kernel-global errno for lwIP error translation in this port. */
int errno;

#define SV_LWIP_MBOX_MAX 128u
#define SV_LWIP_TAG_SEM  'sLwS'
#define SV_LWIP_TAG_MTX  'mLwS'
#define SV_LWIP_TAG_MBX  'bLwS'

struct _SV_LWIP_SEM { KSEMAPHORE Semaphore; };
struct _SV_LWIP_MUTEX { KSEMAPHORE Semaphore; };
struct _SV_LWIP_MBOX {
    KSEMAPHORE Items;
    KSEMAPHORE Slots;
    void **Messages;
    UINT32 Capacity;
    UINT32 Head;
    UINT32 Tail;
    UINT32 Count;
};

static KEVENT g_sleep_event;
static BOOLEAN g_sys_initialized;

static UINT64 ms_to_ticks(u32_t ms)
{
    if (!ms) return ~(UINT64)0;
    UINT64 ticks = ((UINT64)ms + 9u) / 10u;
    return ticks ? ticks : 1;
}

static u32_t elapsed_ms(UINT64 start)
{
    UINT64 elapsed = KeGetTickCount() - start;
    UINT64 ms = elapsed * 10u;
    return ms > 0xfffffffeu ? 0xfffffffeu : (u32_t)ms;
}

void sys_init(void)
{
    if (!g_sys_initialized) {
        KeInitializeEvent(&g_sleep_event, FALSE, FALSE);
        g_sys_initialized = TRUE;
    }
}

err_t sys_sem_new(sys_sem_t *sem, u8_t initial_count)
{
    if (!sem) return ERR_ARG;
    struct _SV_LWIP_SEM *object = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*object), SV_LWIP_TAG_SEM);
    if (!object) { *sem = NULL; return ERR_MEM; }
    KeInitializeSemaphore(&object->Semaphore, initial_count, 0x7fff);
    *sem = object;
    return ERR_OK;
}

void sys_sem_signal(sys_sem_t *sem)
{
    if (sem && *sem) (void)KeReleaseSemaphore(&(*sem)->Semaphore, 1);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout_ms)
{
    if (!sem || !*sem) return SYS_ARCH_TIMEOUT;
    UINT64 start = KeGetTickCount();
    NTSTATUS status = timeout_ms
        ? KeWaitForSingleObjectTimeout(&(*sem)->Semaphore.Header,
                                       ms_to_ticks(timeout_ms))
        : KeWaitForSingleObject(&(*sem)->Semaphore.Header);
    if (status == STATUS_TIMEOUT) return SYS_ARCH_TIMEOUT;
    return elapsed_ms(start);
}

u32_t sys_arch_sem_wait_interruptible(sys_sem_t *sem, u32_t timeout_ms)
{
    /* Svindows does not yet deliver POSIX signals to blocked Linux threads.
     * Therefore this wait has the same completion/timeout semantics as the
     * ordinary wait and cannot currently return SYS_ARCH_INTR. */
    return sys_arch_sem_wait(sem, timeout_ms);
}

void sys_sem_free(sys_sem_t *sem)
{
    if (!sem || !*sem) return;
    ExFreePool(*sem);
    *sem = NULL;
}

int sys_sem_valid(sys_sem_t *sem) { return sem && *sem; }
void sys_sem_set_invalid(sys_sem_t *sem) { if (sem) *sem = NULL; }

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    if (!mutex) return ERR_ARG;
    struct _SV_LWIP_MUTEX *object = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*object), SV_LWIP_TAG_MTX);
    if (!object) { *mutex = NULL; return ERR_MEM; }
    /* lwIP mutex storage is pool memory, not an Object Manager object.
     * KMUTANT acquire/release currently owns an OB reference and therefore
     * must not be embedded in this allocation. A binary semaphore provides
     * the required non-recursive mutex semantics without object refcounts. */
    KeInitializeSemaphore(&object->Semaphore, 1, 1);
    *mutex = object;
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    if (mutex && *mutex) (void)KeWaitForSingleObject(&(*mutex)->Semaphore.Header);
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    if (mutex && *mutex) (void)KeReleaseSemaphore(&(*mutex)->Semaphore, 1);
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    if (!mutex || !*mutex) return;
    ExFreePool(*mutex);
    *mutex = NULL;
}

int sys_mutex_valid(sys_mutex_t *mutex) { return mutex && *mutex; }
void sys_mutex_set_invalid(sys_mutex_t *mutex) { if (mutex) *mutex = NULL; }

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    if (!mbox || size < 1) return ERR_ARG;
    struct _SV_LWIP_MBOX *object = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*object), SV_LWIP_TAG_MBX);
    if (!object) { *mbox = NULL; return ERR_MEM; }
    /* Pool allocations are deliberately not zero-filled.  Every ring index
     * and counter must be initialized before this mailbox is published to
     * lwIP; otherwise the first fetch can index outside Messages. */
    memset(object, 0, sizeof(*object));
    UINT32 capacity = (UINT32)size;
    if (capacity > SV_LWIP_MBOX_MAX) { ExFreePool(object); *mbox = NULL; return ERR_MEM; }
    object->Messages = ExAllocatePoolWithTag(
        NonPagedPool, (SIZE_T)capacity * sizeof(void *), SV_LWIP_TAG_MBX);
    if (!object->Messages) { ExFreePool(object); *mbox = NULL; return ERR_MEM; }
    memset(object->Messages, 0, (SIZE_T)capacity * sizeof(void *));
    object->Capacity = capacity;
    object->Head = 0;
    object->Tail = 0;
    object->Count = 0;
    KeInitializeSemaphore(&object->Items, 0, 0x7fff);
    KeInitializeSemaphore(&object->Slots, (LONG)capacity, 0x7fff);
    *mbox = object;
    KeLog("[lwip] mailbox created: object=%p messages=%p capacity=%u\n",
          object, object->Messages, object->Capacity);
    return ERR_OK;
}

static void mbox_validate_or_bugcheck(struct _SV_LWIP_MBOX *mbox,
                                      const char *where)
{
    if (!mbox || (UINT64)(ULONG_PTR)mbox < MM_KERNEL_HEAP_BASE) {
        KeLog("[lwip] invalid mailbox object at %s: object=%p\n",
              where, mbox);
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "lwIP mailbox object pointer corrupted");
    }

    if (!mbox->Messages ||
        (UINT64)(ULONG_PTR)mbox->Messages < MM_KERNEL_HEAP_BASE ||
        !mbox->Capacity || mbox->Capacity > SV_LWIP_MBOX_MAX ||
        mbox->Head >= mbox->Capacity || mbox->Tail >= mbox->Capacity ||
        mbox->Count > mbox->Capacity) {
        KeLog("[lwip] corrupt mailbox at %s: object=%p messages=%p "
              "capacity=%u head=%u tail=%u count=%u\n",
              where, mbox, mbox->Messages, mbox->Capacity,
              mbox->Head, mbox->Tail, mbox->Count);
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "lwIP mailbox state corrupted");
    }
}

static void mbox_enqueue(struct _SV_LWIP_MBOX *mbox, void *message)
{
    UINT64 irq = KiIrqSave();
    mbox_validate_or_bugcheck(mbox, "enqueue");
    if (mbox->Count >= mbox->Capacity) {
        KiIrqRestore(irq);
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "lwIP mailbox overfilled");
    }
    mbox->Messages[mbox->Tail] = message;
    mbox->Tail = (mbox->Tail + 1u) % mbox->Capacity;
    mbox->Count++;
    KiIrqRestore(irq);
    (void)KeReleaseSemaphore(&mbox->Items, 1);
}

void sys_mbox_post(sys_mbox_t *mbox, void *message)
{
    if (!mbox || !*mbox) return;
    struct _SV_LWIP_MBOX *object = *mbox;
    mbox_validate_or_bugcheck(object, "post-before-wait");
    (void)KeWaitForSingleObject(&object->Slots.Header);
    mbox_enqueue(object, message);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *message)
{
    if (!mbox || !*mbox) return ERR_ARG;
    struct _SV_LWIP_MBOX *object = *mbox;
    mbox_validate_or_bugcheck(object, "trypost-before-wait");
    if (KeWaitForSingleObjectTimeout(&object->Slots.Header, 0) == STATUS_TIMEOUT)
        return ERR_MEM;
    mbox_enqueue(object, message);
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *message)
{
    if (!mbox || !*mbox) return ERR_ARG;
    struct _SV_LWIP_MBOX *object = *mbox;
    mbox_validate_or_bugcheck(object, "trypost-isr");
    UINT64 irq = KiIrqSave();
    if (object->Slots.Header.SignalState <= 0 ||
        object->Count >= object->Capacity) {
        KiIrqRestore(irq);
        return ERR_MEM;
    }
    object->Slots.Header.SignalState--;
    object->Messages[object->Tail] = message;
    object->Tail = (object->Tail + 1u) % object->Capacity;
    object->Count++;
    KiIrqRestore(irq);
    (void)KeReleaseSemaphore(&object->Items, 1);
    return ERR_OK;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **message, u32_t timeout_ms)
{
    if (!mbox || !*mbox) return SYS_ARCH_TIMEOUT;
    /* Keep one stable object pointer across the blocking wait.  Re-reading
     * *mbox after a context switch allowed a changed/freed handle to redirect
     * the queue access into an unrelated address space. */
    struct _SV_LWIP_MBOX *object = *mbox;
    mbox_validate_or_bugcheck(object, "fetch-before-wait");

    UINT64 start = KeGetTickCount();
    NTSTATUS status = timeout_ms
        ? KeWaitForSingleObjectTimeout(&object->Items.Header,
                                       ms_to_ticks(timeout_ms))
        : KeWaitForSingleObject(&object->Items.Header);
    if (status == STATUS_TIMEOUT) return SYS_ARCH_TIMEOUT;

    UINT64 irq = KiIrqSave();
    mbox_validate_or_bugcheck(object, "fetch-after-wait");
    if (!object->Count) {
        KiIrqRestore(irq);
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "lwIP mailbox item semaphore/count mismatch");
    }
    void *value = object->Messages[object->Head];
    object->Messages[object->Head] = NULL;
    object->Head = (object->Head + 1u) % object->Capacity;
    object->Count--;
    KiIrqRestore(irq);
    (void)KeReleaseSemaphore(&object->Slots, 1);
    if (message) *message = value;
    return elapsed_ms(start);
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **message)
{
    if (!mbox || !*mbox) return SYS_MBOX_EMPTY;
    struct _SV_LWIP_MBOX *object = *mbox;
    mbox_validate_or_bugcheck(object, "tryfetch-before-wait");
    /* A zero-tick dispatcher wait is a true non-blocking semaphore acquire. */
    if (KeWaitForSingleObjectTimeout(&object->Items.Header, 0) == STATUS_TIMEOUT)
        return SYS_MBOX_EMPTY;
    UINT64 irq = KiIrqSave();
    mbox_validate_or_bugcheck(object, "tryfetch-after-wait");
    void *value = NULL;
    if (object->Count) {
        value = object->Messages[object->Head];
        object->Messages[object->Head] = NULL;
        object->Head = (object->Head + 1u) % object->Capacity;
        object->Count--;
    }
    KiIrqRestore(irq);
    (void)KeReleaseSemaphore(&object->Slots, 1);
    if (message) *message = value;
    return 0;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    if (!mbox || !*mbox) return;
    struct _SV_LWIP_MBOX *object = *mbox;
    mbox_validate_or_bugcheck(object, "free");
    if (object->Count) KeLog("[lwip] freeing mailbox with %u queued messages\n", object->Count);
    ExFreePool(object->Messages);
    ExFreePool(object);
    *mbox = NULL;
}

int sys_mbox_valid(sys_mbox_t *mbox) { return mbox && *mbox; }
void sys_mbox_set_invalid(sys_mbox_t *mbox) { if (mbox) *mbox = NULL; }

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread,
                            void *argument, int stack_size, int priority)
{
    (void)stack_size; /* KeCreateThread owns and sizes the kernel stack. */
    if (!thread) return NULL;
    return KeCreateThread(name ? name : "lwip", (PKSTART_ROUTINE)thread,
                          argument, priority);
}

/* lwIP's core/sys.c owns sys_msleep(); the OS port supplies only
 * sys_arch_msleep() to avoid exporting a duplicate public symbol. */
void sys_arch_msleep(u32_t milliseconds)
{
    if (!g_sys_initialized) sys_init();
    if (!milliseconds) { KeYield(); return; }
    (void)KeWaitForSingleObjectTimeout(&g_sleep_event.Header,
                                       ms_to_ticks(milliseconds));
}

sys_prot_t sys_arch_protect(void) { return (sys_prot_t)KiIrqSave(); }
void sys_arch_unprotect(sys_prot_t protection) { KiIrqRestore(protection); }

u32_t sys_now(void) { return (u32_t)(KeGetTickCount() * 10u); }
u32_t sys_jiffies(void) { return (u32_t)KeGetTickCount(); }
