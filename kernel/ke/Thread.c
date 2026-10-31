/*
 * ke/Thread.c - kernel threads and the preemptive round-robin scheduler.
 *
 * Threads are cooperatively AND preemptively scheduled: a thread may call
 * KeYield, and the timer interrupt (KeClockTick) forces a reschedule when a
 * thread's quantum expires. Switching is done by KiSwitchContext, which swaps
 * kernel stacks; a brand-new thread's stack is fabricated so its first switch
 * lands in KiThreadStartup -> KiThreadBootstrap -> the thread body.
 *
 * Single-CPU only for now, so short critical sections just disable interrupts
 * (KiIrqSave/KiIrqRestore); thread publication uses a UP spin lock. Scheduling
 * is below DISPATCH_LEVEL, including preemption at the tail of a timer IRQ.
 */
#include <svindows/Ke.h>
#include <svindows/Ps.h>
#include <svindows/Ex.h>
#include <svindows/Ob.h>
#include <svindows/Rtl.h>
#include <nt/Peb.h>
#include <svindows/AddressSpace.h>
#include <svindows/Ldr.h>
#include <svindows/Win32k.h>
#include <svindows/Mm.h>
#include <svindows/Dos.h>
#include <svindows/Linux.h>

#define DEFAULT_QUANTUM 2   /* timer ticks per scheduling slice */
#define THREAD_STACK_SIZE 0x4000 /* 16 KiB kernel stack per thread */

extern void KiSwitchContext(UINT64 *save_rsp, UINT64 load_rsp);
extern void KiThreadStartup(void);                              /* asm trampoline    */
extern void KiEnterUserMode(UINT64 entry, UINT64 stack, UINT64 arg); /* asm ring-3 */
extern void KiEnterLinuxUserMode(UINT64 entry, UINT64 stack); /* ELF ring-3 */
extern void KiEnterLinuxCloneUserMode(UINT64 entry, UINT64 stack,
                                      UINT64 flags, const void *registers);

static KPROCESS       g_system_process;
static KTHREAD        g_idle_thread;
static PKTHREAD       g_current_thread;
static LIST_ENTRY     g_ready_queue;
static LIST_ENTRY     g_all_threads;
static ULONG          g_next_thread_id;
static volatile UINT64 g_tick_count;
static BOOLEAN g_reschedule_pending;
static KSPIN_LOCK g_thread_list_lock;
static KDPC g_clock_dpc;
static void KepClockDpc(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2);
static void KiSchedule(void);

static void KepSignalThreadTermination(PKTHREAD thread)
{
    if (!thread->TerminationObject)
        return;
    PDISPATCHER_HEADER termination = thread->TerminationObject;
    termination->SignalState = 1;
    KiSignalObject(termination);
    thread->TerminationObject = NULL;
    ObDereferenceObject((POBJECT)termination);
}

static void KepReapTerminatedThreads(void)
{
    PLIST_ENTRY head = &g_all_threads;
    for (PLIST_ENTRY entry = head->Flink; entry != head; ) {
        PKTHREAD thread = CONTAINING_RECORD(entry, KTHREAD, SchedulerEntry);
        entry = entry->Flink;
        if (thread == g_current_thread ||
            thread->State != ThreadStateTerminated)
            continue;
        W32kCleanupThreadUser(thread);
        if (thread->DosContext) {
            DosDestroyVm(thread->DosContext);
            thread->DosContext = NULL;
        }
        LinuxDestroyThread(thread);
        KiCancelThreadTimers(thread);
        UINT64 root = thread->Process->DirectoryTableBase;
        MmReleaseUserPagesInAddressSpace(root, thread->OwnedTlsBase,
                                         thread->OwnedTlsPages);
        MmReleaseUserPagesInAddressSpace(root, thread->OwnedUserTebBase,
                                         thread->OwnedUserTebPages);
        MmReleaseUserPagesInAddressSpace(root, thread->OwnedUserStackBase,
                                         thread->OwnedUserStackPages);
        RemoveEntryList(&thread->SchedulerEntry);
        RemoveEntryList(&thread->ProcessEntry);
        POBJECT process_object = thread->OwningProcessObject;
        thread->OwningProcessObject = NULL;
        if (thread->KernelStackBase)
            ExFreePool((void *)thread->KernelStackBase);
        ExFreePool(thread);
        if (process_object)
            ObDereferenceObject(process_object);
    }
}

PKTHREAD KeGetCurrentThread(void) { return g_current_thread; }

/* Called by native thread services while the syscall has interrupts masked. */
PKTHREAD KeLookupThreadById(ULONG thread_id)
{
    if (!thread_id) return NULL;
    for (PLIST_ENTRY entry = g_all_threads.Flink;
         entry != &g_all_threads; entry = entry->Flink) {
        PKTHREAD thread = CONTAINING_RECORD(entry, KTHREAD, SchedulerEntry);
        if (thread->ThreadId == thread_id && thread->UserMode &&
            thread->State != ThreadStateTerminated)
            return thread;
    }
    return NULL;
}

NTSTATUS KeSuspendThread(PKTHREAD thread, ULONG *previous_count)
{
    if (!thread || thread->State == ThreadStateTerminated)
        return STATUS_INVALID_HANDLE;
    UINT64 flags = KiIrqSave();
    ULONG count = thread->SuspendCount;
    if (count == 127) {
        KiIrqRestore(flags);
        return STATUS_INVALID_PARAMETER;
    }
    thread->SuspendCount = count + 1;
    if (!count && thread->State == ThreadStateReady) {
        RemoveEntryList(&thread->ReadyEntry);
        thread->State = ThreadStateSuspended;
    }
    if (previous_count) *previous_count = count;
    if (thread == g_current_thread && !count &&
        thread->State == ThreadStateRunning) {
        thread->State = ThreadStateSuspended;
        KiSchedule();
    }
    KiIrqRestore(flags);
    return STATUS_SUCCESS;
}

NTSTATUS KeResumeThread(PKTHREAD thread, ULONG *previous_count)
{
    if (!thread || thread->State == ThreadStateTerminated)
        return STATUS_INVALID_HANDLE;
    UINT64 flags = KiIrqSave();
    ULONG count = thread->SuspendCount;
    if (previous_count) *previous_count = count;
    if (count && !--thread->SuspendCount &&
        thread->State == ThreadStateSuspended) {
        thread->State = ThreadStateReady;
        InsertTailList(&g_ready_queue, &thread->ReadyEntry);
    }
    KiIrqRestore(flags);
    return STATUS_SUCCESS;
}
UINT64   KeGetTickCount(void)     { return g_tick_count; }

void KeInitializeScheduler(void)
{
    KeInitializeDpc(&g_clock_dpc, KepClockDpc, NULL);
    InitializeListHead(&g_ready_queue);
    InitializeListHead(&g_all_threads);

    UINT64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    g_system_process.DirectoryTableBase = cr3;
    g_system_process.HandleTable = ObGetBootstrapHandleTable();
    g_system_process.LoaderContext = LdrCreateProcessContext();
    if (!g_system_process.LoaderContext)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "cannot allocate System loader context");
    g_system_process.ProcessId = 0;
    g_system_process.PriorityClass = 2;
    g_system_process.BasePriority = 8;
    g_system_process.Object = NULL;
    g_system_process.ParentProcessId = 0;
    g_system_process.ExitStatus = STATUS_PENDING;
    g_system_process.UserAllocationNext = 0x0000000020000000ULL;
    g_system_process.ThreadAllocationNext = 0x0000000030000000ULL;
    g_system_process.Name = "System";
    InitializeListHead(&g_system_process.ThreadListHead);

    /* The currently executing boot context becomes the idle thread. */
    memset(&g_idle_thread, 0, sizeof(g_idle_thread));
    g_idle_thread.State = ThreadStateRunning;
    g_idle_thread.Name = "Idle";
    g_idle_thread.Process = &g_system_process;
    InitializeListHead(&g_idle_thread.OwnedMutantListHead);
    InsertTailList(&g_system_process.ThreadListHead, &g_idle_thread.ProcessEntry);
    InsertTailList(&g_all_threads, &g_idle_thread.SchedulerEntry);

    g_current_thread = &g_idle_thread;
    KeSetCurrentThread(&g_idle_thread);
    g_next_thread_id = 1;

    /* Give the idle thread a valid FPU/SSE image for the first context switch. */
    KiSaveFpuState(g_idle_thread.FpuState);

    KeLog("[ke]   scheduler ready: System process, idle thread\n");
}

/* Allocate a thread and its kernel stack, fabricating the initial stack so the
 * first KiSwitchContext to it 'ret's into KiThreadStartup. Not yet enqueued. */
static PKTHREAD KepAllocThread(PKPROCESS process, const char *name,
                               LONG priority)
{
    PKTHREAD t = ExAllocatePoolWithTag(NonPagedPool, sizeof(KTHREAD), 'rhtK');
    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));

    void *stack = ExAllocatePoolWithTag(NonPagedPool, THREAD_STACK_SIZE, 'ktsK');
    if (!stack) {
        ExFreePool(t);
        return NULL;
    }
    t->KernelStackBase = (UINT64)stack;
    t->KernelStackSize = THREAD_STACK_SIZE;

    UINT64 *sp = (UINT64 *)((UINT64)stack + THREAD_STACK_SIZE);
    *--sp = (UINT64)KiThreadStartup; /* return address for the final 'ret' */
    *--sp = 0;                       /* rbx */
    *--sp = 0;                       /* rbp */
    *--sp = 0;                       /* r12 */
    *--sp = 0;                       /* r13 */
    *--sp = 0;                       /* r14 */
    *--sp = 0;                       /* r15 (popped first) */
    t->KernelStackPointer = (UINT64)sp;

    t->State = ThreadStateReady;
    t->Priority = priority;
    t->Quantum = DEFAULT_QUANTUM;
    t->ThreadId = g_next_thread_id++;
    t->Name = name;
    t->Process = process ? process : &g_system_process;
    InitializeListHead(&t->OwnedMutantListHead);
    if (t->Process->Object) {
        ObReferenceObject(t->Process->Object);
        t->OwningProcessObject = t->Process->Object;
    }

    /* Seed a valid FPU/SSE image (captures the default MXCSR) so the first
     * FXRSTOR into this thread doesn't fault or load garbage. */
    KiSaveFpuState(t->FpuState);
    return t;
}

static void KepEnqueueThread(PKTHREAD t)
{
    KIRQL old_irql;
    KeAcquireSpinLock(&g_thread_list_lock, &old_irql);
    InsertTailList(&g_ready_queue, &t->ReadyEntry);
    InsertTailList(&t->Process->ThreadListHead, &t->ProcessEntry);
    InsertTailList(&g_all_threads, &t->SchedulerEntry);
    KeReleaseSpinLock(&g_thread_list_lock, old_irql);
}

PKTHREAD KeCreateThread(const char *name, PKSTART_ROUTINE routine,
                        PVOID context, LONG priority)
{
    PKTHREAD t = KepAllocThread(&g_system_process, name, priority);
    if (!t)
        return NULL;

    t->StartRoutine = routine;
    t->StartContext = context;
    KepEnqueueThread(t);

    KeLog("[ke]   created thread '%s' (id %u), stack %p\n",
          name, t->ThreadId, (void *)t->KernelStackBase);
    return t;
}

PKTHREAD KeCreateKernelThreadForProcess(PKPROCESS process, const char *name,
                                        PKSTART_ROUTINE routine, PVOID context,
                                        PDISPATCHER_HEADER termination,
                                        LONG priority)
{
    PKTHREAD t = KepAllocThread(process, name, priority);
    if (!t) return NULL;
    t->StartRoutine = routine;
    t->StartContext = context;
    t->DosContext = context;
    t->TerminationObject = termination;
    t->ProcessMainThread = TRUE;
    ObReferenceObject((POBJECT)termination);
    KepEnqueueThread(t);
    return t;
}

PKTHREAD KeCreateUserThread(const char *name, UINT64 user_entry,
                            UINT64 user_stack, UINT64 user_gs_base,
                            UINT64 user_arg, LONG priority)
{
    return KeCreateUserThreadForProcess(&g_system_process, name, user_entry,
                                        user_stack, user_gs_base, user_arg,
                                        priority);
}

PKTHREAD KeCreateUserThreadForProcess(PKPROCESS process, const char *name,
                                      UINT64 user_entry, UINT64 user_stack,
                                      UINT64 user_gs_base, UINT64 user_arg,
                                      LONG priority)
{
    PKTHREAD t = KepAllocThread(process, name, priority);
    if (!t)
        return NULL;

    t->UserMode = TRUE;
    t->UserEntry = user_entry;
    t->UserStack = user_stack;
    t->UserGsBase = user_gs_base;
    t->UserArg = user_arg;

    /* Ps builds the TEB before Ke allocates the KTHREAD, so this is the first
     * point at which the real thread id is known.  Native code reads the id
     * directly from gs:[0x48]; leaving child TEBs at zero makes every created
     * thread look identical and breaks owner-based user-mode synchronization. */
    if (user_gs_base)
        ((PTEB)user_gs_base)->ClientId.UniqueThread =
            (HANDLE)(ULONG_PTR)t->ThreadId;
    KepEnqueueThread(t);

    KeLog("[ke]   created user thread '%s' (id %u): entry %p, ustack %p, teb %p\n",
          name, t->ThreadId, (void *)user_entry, (void *)user_stack,
          (void *)user_gs_base);
    return t;
}

/*
 * KiSchedule - pick the next runnable thread and switch to it.
 * Must be called with interrupts disabled.
 */
static void KiSchedule(void)
{
    if (KeGetCurrentIrql() > APC_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "scheduler above APC_LEVEL");
    g_reschedule_pending = FALSE;
    KepReapTerminatedThreads();
    PKTHREAD prev = g_current_thread;
    PKTHREAD next;

    if (IsListEmpty(&g_ready_queue)) {
        next = &g_idle_thread;
    } else {
        PLIST_ENTRY e = g_ready_queue.Flink;
        PLIST_ENTRY best = e;
        LONG highest = CONTAINING_RECORD(e, KTHREAD, ReadyEntry)->Priority;
        for (e = e->Flink; e != &g_ready_queue; e = e->Flink) {
            PKTHREAD candidate = CONTAINING_RECORD(e, KTHREAD, ReadyEntry);
            if (candidate->Priority > highest) {
                highest = candidate->Priority;
                best = e;
            }
        }
        e = best;
        RemoveEntryList(e);
        next = CONTAINING_RECORD(e, KTHREAD, ReadyEntry);
    }

    /* Return the outgoing thread to the ready queue if it can still run. The
     * idle thread is the fallback and is never queued. */
    if (prev != &g_idle_thread && prev->State == ThreadStateRunning) {
        prev->State = prev->SuspendCount ? ThreadStateSuspended : ThreadStateReady;
        if (!prev->SuspendCount)
            InsertTailList(&g_ready_queue, &prev->ReadyEntry);
    }

    if (prev == next) {
        prev->State = ThreadStateRunning;
        return;
    }

    next->State = ThreadStateRunning;
    next->Quantum = DEFAULT_QUANTUM;
    g_current_thread = next;
    KeSetCurrentThread(next);

    /* Kernel mappings are shared, so it is safe to change CR3 while running
     * on the outgoing kernel stack. User mappings become those of `next`. */
    MmSwitchAddressSpace(next->Process->DirectoryTableBase);

    /* Point the CPU's ring-0 entry stack at the incoming thread's kernel stack,
     * so a syscall or interrupt taken from ring 3 lands on the right stack, and
     * set the GS base it will see in ring 3 (its TEB). */
    if (next->KernelStackBase)
        KeSetKernelStack(next->KernelStackBase + next->KernelStackSize);
    KeSetUserGsBase(next->UserGsBase);
    KeSetUserFsBase(next->UserFsBase);

    /* Preserve SSE/x87 state across the switch: save ours, load the incoming
     * thread's. Done here (not in the asm) because the incoming thread's area
     * must be restored with a pointer that survives the stack swap. */
    KiSaveFpuState(prev->FpuState);
    KiRestoreFpuState(next->FpuState);

    prev->SavedIrql = KiSwitchThreadIrql(next->SavedIrql);

    KiSwitchContext(&prev->KernelStackPointer, next->KernelStackPointer);
    /* Control returns here only when `prev` is scheduled again. */
    KiDeliverKernelApcs();
}

void KeYield(void)
{
    UINT64 flags = KiIrqSave();
    KiSchedule();
    KiIrqRestore(flags);
}

/* Move a waiting thread back to the ready queue (called by the dispatcher with
 * interrupts disabled). */
void KiReadyThread(PKTHREAD thread)
{
    if (thread->State == ThreadStateWaiting) {
        thread->State = thread->SuspendCount ? ThreadStateSuspended :
                                               ThreadStateReady;
        if (!thread->SuspendCount)
            InsertTailList(&g_ready_queue, &thread->ReadyEntry);
    }
}

/* The current thread has marked itself Waiting; switch away until readied. */
void KiBlockCurrentThread(void)
{
    KiSchedule(); /* won't re-enqueue a non-Running thread */
}

/* Entered from KiThreadStartup (assembly) the first time a new thread runs. */
void KiThreadBootstrap(void)
{
    PKTHREAD t = g_current_thread;
    KiDeliverKernelApcs();

    if (t->UserMode) {
        /* Drop to ring 3; the thread lives in user mode from here and only
         * re-enters the kernel via syscalls and interrupts. Never returns. */
        if (t->LinuxThread && LinuxIsCloneThread(t))
            KiEnterLinuxCloneUserMode(t->UserEntry, t->UserStack,
                                      t->UserRflags,
                                      LinuxCloneRegisters(t));
        if (t->Process && t->Process->LinuxContext)
            KiEnterLinuxUserMode(t->UserEntry, t->UserStack);
        KiEnterUserMode(t->UserEntry, t->UserStack, t->UserArg);
    } else if (t->StartRoutine) {
        t->StartRoutine(t->StartContext);
    }

    KeTerminateThread();
}

NORETURN void KeTerminateThread(void)
{
    if (KeGetCurrentIrql() > APC_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "thread termination above APC_LEVEL");
    (void)KiIrqSave(); /* disable preemption for good on this thread */

    PKTHREAD t = g_current_thread;
    t->State = ThreadStateTerminated;
    LinuxThreadExited(t);
    KeLog("[ke]   thread '%s' (id %u) terminated\n", t->Name, t->ThreadId);

    KeAbandonThreadMutants(t);

    /* Signal the thread object so anyone waiting on this thread wakes up, then
     * release the reference the thread held on it (see NtCreateThreadEx). The
     * dispatcher header is the object body, so the body pointer is the same
     * address. */
    if (t->ProcessMainThread && t->Process) {
        PKPROCESS process = t->Process;
        process->Exiting = TRUE;
        process->ExitStatus = t->ExitStatus;
        PsJobProcessExited(process);
        PLIST_ENTRY head = &process->ThreadListHead;
        for (PLIST_ENTRY entry = head->Flink; entry != head;
             entry = entry->Flink) {
            PKTHREAD other = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
            if (other == t || other->State == ThreadStateTerminated)
                continue;
            if (other->State == ThreadStateReady)
                RemoveEntryList(&other->ReadyEntry);
            else if (other->State == ThreadStateWaiting)
                KiCancelThreadWait(other, t->ExitStatus);
            other->ExitStatus = t->ExitStatus;
            other->State = ThreadStateTerminated;
            KeAbandonThreadMutants(other);
            KepSignalThreadTermination(other);
            KeLog("[ke]   process exit terminated thread '%s' (id %u)\n",
                  other->Name, other->ThreadId);
        }
    }
    KepSignalThreadTermination(t);
    KiSchedule();

    for (;;)
        __asm__ volatile("hlt");
}

/* Terminate a non-running thread without ever letting it re-enter user mode.
 * Its object remains signaled and the normal scheduler reap path owns the
 * stack/TEB cleanup. */
NTSTATUS KeTerminateThreadByPointer(PKTHREAD target, NTSTATUS exit_status)
{
    if (!target || target->State == ThreadStateTerminated)
        return STATUS_INVALID_HANDLE;
    if (target == g_current_thread) {
        target->ExitStatus = exit_status;
        KeTerminateThread();
    }
    UINT64 flags = KiIrqSave();
    if (target->State == ThreadStateReady)
        RemoveEntryList(&target->ReadyEntry);
    else if (target->State == ThreadStateWaiting)
        KiCancelThreadWait(target, exit_status);
    target->ExitStatus = exit_status;
    target->State = ThreadStateTerminated;
    KeAbandonThreadMutants(target);
    if (target->ProcessMainThread && target->Process) {
        PKPROCESS process = target->Process;
        process->Exiting = TRUE;
        process->ExitStatus = exit_status;
        PsJobProcessExited(process);
        for (PLIST_ENTRY entry = process->ThreadListHead.Flink;
             entry != &process->ThreadListHead; entry = entry->Flink) {
            PKTHREAD other = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
            if (other == target || other == g_current_thread ||
                other->State == ThreadStateTerminated) continue;
            if (other->State == ThreadStateReady)
                RemoveEntryList(&other->ReadyEntry);
            else if (other->State == ThreadStateWaiting)
                KiCancelThreadWait(other, exit_status);
            other->ExitStatus = exit_status;
            other->State = ThreadStateTerminated;
            KeAbandonThreadMutants(other);
            KepSignalThreadTermination(other);
        }
    }
    KepSignalThreadTermination(target);
    BOOLEAN exit_caller = target->ProcessMainThread &&
                          g_current_thread->Process == target->Process;
    KiIrqRestore(flags);
    if (exit_caller) {
        g_current_thread->ExitStatus = exit_status;
        KeTerminateThread();
    }
    return STATUS_SUCCESS;
}

static void KepClockDpc(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2)
{
    (void)dpc; (void)context; (void)arg1; (void)arg2;
    KeUpdateSharedData(g_tick_count);
    KiExpireTimers(g_tick_count);

    /* Dispatcher timeouts are absolute tick deadlines stored on blocked
     * threads. The process list is stable while interrupts are disabled. */
    PLIST_ENTRY head = &g_all_threads;
    for (PLIST_ENTRY entry = head->Flink; entry != head; ) {
        PKTHREAD waiting = CONTAINING_RECORD(entry, KTHREAD, SchedulerEntry);
        entry = entry->Flink;
        if (waiting->State == ThreadStateWaiting && waiting->WaitDeadline &&
            g_tick_count >= waiting->WaitDeadline)
            KiTimeoutThreadWait(waiting);
    }

}

void KeClockTick(void)
{
    g_tick_count++;
    KeInsertQueueDpc(&g_clock_dpc, NULL, NULL);
    PKTHREAD t = g_current_thread;
    if (t->Quantum > 0)
        t->Quantum--;

    if (t->Quantum <= 0)
        g_reschedule_pending = TRUE;
}

/* Called after the IRQ's execution level is restored, with IF still clear.
 * A switch can suspend this trap frame without leaking ISR IRQL to a thread. */
void KiScheduleAfterInterrupt(void)
{
    if (g_reschedule_pending && KeGetCurrentIrql() <= APC_LEVEL)
        KiSchedule();
    KiDeliverKernelApcs();
}
