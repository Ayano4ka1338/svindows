/*
 * ke/Thread.c - kernel threads and the preemptive round-robin scheduler.
 *
 * Threads are cooperatively AND preemptively scheduled: a thread may call
 * KeYield, and the timer interrupt (KeClockTick) forces a reschedule when a
 * thread's quantum expires. Switching is done by KiSwitchContext, which swaps
 * kernel stacks; a brand-new thread's stack is fabricated so its first switch
 * lands in KiThreadStartup -> KiThreadBootstrap -> the thread body.
 *
 * The scheduler uses one globally locked run queue shared by all online CPUs.
 * Each CPU owns an idle context and quantum; APs are woken/preempted by local
 * APIC IPIs. Until remote TLB shootdowns exist, one process is admitted to one
 * CPU at a time, while different processes may execute concurrently.
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
#include <svindows/Hal.h>

#define DEFAULT_QUANTUM 2   /* timer ticks per scheduling slice */
#define THREAD_STACK_SIZE 0x4000 /* 16 KiB kernel stack per thread */

extern void KiSwitchContext(UINT64 *save_rsp, UINT64 load_rsp,
                            PKTHREAD outgoing);
extern void KiThreadStartup(void);                              /* asm trampoline    */
extern void KiEnterUserMode(UINT64 entry, UINT64 stack, UINT64 arg); /* asm ring-3 */
extern void KiEnterLinuxUserMode(UINT64 entry, UINT64 stack); /* ELF ring-3 */
extern void KiEnterLinuxCloneUserMode(UINT64 entry, UINT64 stack,
                                      UINT64 flags, const void *registers);

static KPROCESS       g_system_process;
static KTHREAD       *g_idle_threads;
static LIST_ENTRY     g_ready_queue;
static LIST_ENTRY     g_all_threads;
static ULONG          g_next_thread_id;
static volatile UINT64 g_tick_count;
static KSPIN_LOCK g_scheduler_lock;

static LONG KepDispatchPriority(PKTHREAD thread)
{
    LONG priority = thread->Priority + thread->SchedulerBoost;
    if (!thread->Process || thread->Process->PriorityClass != 4) {
        if (priority > 15) priority = 15;
    } else if (priority > 31) {
        priority = 31;
    }
    return priority;
}
static KDPC g_clock_dpc;
static void KepClockDpc(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2);
static void KiSchedule(void);
static void KiScheduleWithLock(UINT64 interrupt_state);

/* Called with the scheduler lock held after a thread becomes Ready. Wake only
 * idle APs; busy processors will observe the shared queue on their next
 * quantum IPI. A wake handler may block on the scheduler lock until this
 * critical section publishes the complete queue update. */
static void KepWakeIdleProcessors(void)
{
    PKPRCB current = KeGetCurrentProcessorBlock();
    UINT32 count = HalGetProcessorCount();
    if (count > KE_MAXIMUM_PROCESSORS) count = KE_MAXIMUM_PROCESSORS;
    for (UINT32 i = 1; i < count; ++i) {
        PKPRCB processor = KeGetProcessorBlock(i);
        if (!processor || processor == current ||
            processor->StartupState != 2 ||
            !__atomic_load_n(&processor->StartupRelease, __ATOMIC_ACQUIRE) ||
            processor->CurrentThread != processor->IdleThread)
            continue;
        __atomic_store_n(&processor->ReschedulePending, 1, __ATOMIC_RELEASE);
        (void)HalSendIpi(processor->ApicId, HAL_SMP_WAKE_VECTOR);
    }
}

UINT64 KiAcquireSchedulerLock(void)
{
    return KiAcquireRawSpinLock(&g_scheduler_lock);
}

void KiReleaseSchedulerLock(UINT64 interrupt_state)
{
    KiReleaseRawSpinLock(&g_scheduler_lock, interrupt_state);
}

static BOOLEAN KepThreadCurrentOnAnyProcessor(PKTHREAD thread)
{
    UINT32 count = HalGetProcessorCount();
    if (!count || count > KE_MAXIMUM_PROCESSORS) count = 1;
    for (UINT32 i = 0; i < count; ++i) {
        PKPRCB processor = KeGetProcessorBlock(i);
        if (processor && processor->CurrentThread == thread)
            return TRUE;
    }
    return FALSE;
}

/* A terminating thread must stop executing on every CPU before teardown can
 * release its user stack, TEB, DOS state, or other per-thread resources. The
 * global scheduler lock is also the context-switch completion barrier: the
 * incoming stack releases it only after the outgoing RSP has been saved. */
static void KepWaitForThreadOffProcessors(PKTHREAD thread)
{
    BOOLEAN reschedule_sent = FALSE;
    for (;;) {
        PKPRCB owner = NULL;
        UINT64 flags = KiAcquireSchedulerLock();
        UINT32 count = HalGetProcessorCount();
        if (!count || count > KE_MAXIMUM_PROCESSORS)
            count = 1;
        for (UINT32 i = 0; i < count; ++i) {
            PKPRCB cpu = KeGetProcessorBlock(i);
            if (cpu && cpu->CurrentThread == thread) {
                owner = cpu;
                if (!reschedule_sent && cpu != KeGetCurrentProcessorBlock() &&
                    cpu->StartupState == 2 &&
                    __atomic_load_n(&cpu->StartupRelease, __ATOMIC_ACQUIRE)) {
                    __atomic_store_n(&cpu->ReschedulePending, 1,
                                     __ATOMIC_RELEASE);
                    if (!HalSendIpi(cpu->ApicId, HAL_SMP_WAKE_VECTOR))
                        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                                   "cannot deliver remote thread reschedule IPI");
                    reschedule_sent = TRUE;
                }
                break;
            }
        }
        KiReleaseSchedulerLock(flags);
        if (!owner)
            return;
        /* Do not hold the run-queue lock while the remote CPU handles its IPI. */
        __asm__ volatile("pause" ::: "memory");
    }
}

#include "ThreadTermination.inc"

/* Process handles are waitable too. Wake their waiters only after the exit
 * status and all sibling-thread termination state have been published. */
static void KepSignalProcessTermination(PKPROCESS process)
{
    if (!process || !process->Object) return;
    PDISPATCHER_HEADER header = (PDISPATCHER_HEADER)process->Object;
    UINT64 flags = KiAcquireSchedulerLock();
    header->SignalState = 1;
    KiSignalObjectWithSchedulerLock(header);
    KiReleaseSchedulerLock(flags);
}

static void KepReapTerminatedThreads(void)
{
    PLIST_ENTRY head = &g_all_threads;
    for (PLIST_ENTRY entry = head->Flink; entry != head; ) {
        PKTHREAD thread = CONTAINING_RECORD(entry, KTHREAD, SchedulerEntry);
        entry = entry->Flink;
        PKPRCB current_processor = KeGetCurrentProcessorBlock();
        PKTHREAD current_thread = current_processor->CurrentThread;
        BOOLEAN same_address_space_active = current_thread &&
            current_thread != current_processor->IdleThread &&
            current_thread->Process == thread->Process;
        if (KepThreadCurrentOnAnyProcessor(thread) ||
            thread->State != ThreadStateTerminated ||
            (!same_address_space_active && thread->Process &&
             __atomic_load_n(&thread->Process->ActiveProcessorCount,
                             __ATOMIC_ACQUIRE) != 0))
            continue;
        W32kCleanupThreadUser(thread);
        if (thread->DosContext) {
            DosDestroyVm(thread->DosContext);
            thread->DosContext = NULL;
        }
        LinuxDestroyThread(thread);
        KiCancelThreadTimersWithSchedulerLock(thread);
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

PKTHREAD KeGetCurrentThread(void) { return KeGetCurrentProcessorBlock()->CurrentThread; }

/* Called by native thread services while the syscall has interrupts masked. */
PKTHREAD KeLookupThreadById(ULONG thread_id)
{
    if (!thread_id) return NULL;
    UINT64 interrupt_state = KiAcquireRawSpinLock(&g_scheduler_lock);
    PKTHREAD result = NULL;
    for (PLIST_ENTRY entry = g_all_threads.Flink;
         entry != &g_all_threads; entry = entry->Flink) {
        PKTHREAD thread = CONTAINING_RECORD(entry, KTHREAD, SchedulerEntry);
        if (thread->ThreadId == thread_id && thread->UserMode &&
            thread->State != ThreadStateTerminated) {
            result = thread;
            break;
        }
    }
    KiReleaseRawSpinLock(&g_scheduler_lock, interrupt_state);
    return result;
}

PKPROCESS KeGetSystemProcess(void)
{
    return &g_system_process;
}

ULONG KeSnapshotProcessThreads(PKPROCESS process,
                               KE_PROCESS_THREAD_INFO *buffer, ULONG capacity)
{
    if (!process) return 0;
    UINT64 flags = KiAcquireRawSpinLock(&g_scheduler_lock);
    ULONG count = 0;
    for (PLIST_ENTRY entry = process->ThreadListHead.Flink;
         entry != &process->ThreadListHead; entry = entry->Flink) {
        PKTHREAD thread = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
        if (!thread->ThreadId || thread->State == ThreadStateTerminated)
            continue;
        if (buffer && count < capacity) {
            KE_PROCESS_THREAD_INFO *info = &buffer[count];
            info->ThreadId = thread->ThreadId;
            info->Priority = thread->Priority;
            info->BasePriority = thread->Priority - thread->RelativePriority;
            info->ExitStatus = thread->ExitStatus;
            info->KernelTime = __atomic_load_n(&thread->KernelTime,__ATOMIC_RELAXED);
            info->UserTime = __atomic_load_n(&thread->UserTime,__ATOMIC_RELAXED);
            info->ContextSwitches = thread->ContextSwitches;
            switch (thread->State) {
                case ThreadStateReady: info->State = 1; break;
                case ThreadStateRunning:
                case ThreadStateSwitching: info->State = 2; break;
                case ThreadStateTerminating:
                case ThreadStateTerminated: info->State = 4; break;
                default: info->State = 5; break; /* waiting */
            }
        }
        count++;
    }
    KiReleaseRawSpinLock(&g_scheduler_lock, flags);
    return count;
}

NTSTATUS KeSuspendThread(PKTHREAD thread, ULONG *previous_count)
{
    if (!thread) return STATUS_INVALID_HANDLE;
    UINT64 flags = KiAcquireRawSpinLock(&g_scheduler_lock);
    if (thread->State == ThreadStateTerminated ||
        thread->State == ThreadStateTerminating) {
        KiReleaseRawSpinLock(&g_scheduler_lock, flags);
        return STATUS_INVALID_HANDLE;
    }
    ULONG count = thread->SuspendCount;
    if (count == 127) {
        KiReleaseRawSpinLock(&g_scheduler_lock, flags);
        return STATUS_INVALID_PARAMETER;
    }
    thread->SuspendCount = count + 1;
    if (!count && thread->State == ThreadStateReady) {
        RemoveEntryList(&thread->ReadyEntry);
        thread->State = ThreadStateSuspended;
    }
    if (previous_count) *previous_count = count;
    BOOLEAN schedule_current =
        thread == KeGetCurrentProcessorBlock()->CurrentThread && !count &&
        thread->State == ThreadStateRunning;
    if (schedule_current) {
        thread->State = ThreadStateSuspended;
    }
    KiReleaseRawSpinLock(&g_scheduler_lock, flags);
    if (schedule_current) KiSchedule();
    return STATUS_SUCCESS;
}

NTSTATUS KeResumeThread(PKTHREAD thread, ULONG *previous_count)
{
    if (!thread) return STATUS_INVALID_HANDLE;
    UINT64 flags = KiAcquireRawSpinLock(&g_scheduler_lock);
    if (thread->State == ThreadStateTerminated ||
        thread->State == ThreadStateTerminating) {
        KiReleaseRawSpinLock(&g_scheduler_lock, flags);
        return STATUS_INVALID_HANDLE;
    }
    BOOLEAN became_ready = FALSE;
    ULONG count = thread->SuspendCount;
    if (previous_count) *previous_count = count;
    if (count && !--thread->SuspendCount &&
        thread->State == ThreadStateSuspended) {
        thread->State = ThreadStateReady;
        InsertTailList(&g_ready_queue, &thread->ReadyEntry);
        became_ready = TRUE;
    }
    if (became_ready) KepWakeIdleProcessors();
    KiReleaseRawSpinLock(&g_scheduler_lock, flags);
    return STATUS_SUCCESS;
}
UINT64 KeGetTickCount(void)
{
    return __atomic_load_n(&g_tick_count, __ATOMIC_RELAXED);
}

void KeInitializeScheduler(void)
{
    KeInitializeDpc(&g_clock_dpc, KepClockDpc, NULL);
    KeInitializeSpinLock(&g_scheduler_lock);
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
    g_system_process.ProcessId = 4;
    g_system_process.PriorityClass = 2;
    g_system_process.BasePriority = 8;
    g_system_process.Object = NULL;
    g_system_process.ParentProcessId = 0;
    g_system_process.ExitStatus = STATUS_PENDING;
    g_system_process.UserAllocationNext = 0x0000000020000000ULL;
    g_system_process.ThreadAllocationNext = 0x0000000030000000ULL;
    g_system_process.Name = "System";
    InitializeListHead(&g_system_process.ThreadListHead);

    /* Every processor needs a private idle context before it can be admitted
     * to the scheduler. Offline AP idle contexts stay out of the ready queue. */
    UINT32 processor_count = HalGetProcessorCount();
    if (!processor_count || processor_count > KE_MAXIMUM_PROCESSORS)
        processor_count = 1;
    g_idle_threads = ExAllocatePoolWithTag(NonPagedPool,
        sizeof(*g_idle_threads) * processor_count, 'dIlK');
    if (!g_idle_threads)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "cannot allocate idle thread contexts");
    memset(g_idle_threads, 0, sizeof(*g_idle_threads) * processor_count);
    for (UINT32 i = 0; i < processor_count; ++i) {
        KTHREAD *idle = &g_idle_threads[i];
        PKPRCB processor = KeGetProcessorBlock(i);
        memset(idle, 0, sizeof(*idle));
        idle->State = i == 0 ? ThreadStateRunning : ThreadStateReady;
        idle->Name = "Idle";
        idle->Process = &g_system_process;
        InitializeListHead(&idle->OwnedMutantListHead);
        InsertTailList(&g_system_process.ThreadListHead, &idle->ProcessEntry);
        InsertTailList(&g_all_threads, &idle->SchedulerEntry);
        processor->IdleThread = idle;
        if (i == 0) {
            processor->CurrentThread = idle;
            processor->StartupState = 2;
            processor->StartupRelease = 1;
            KeSetCurrentThread(idle);
        }
        KiSaveFpuState(idle->FpuState);
    }
    g_next_thread_id = 1;

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
    UINT64 id_flags = KiAcquireRawSpinLock(&g_scheduler_lock);
    t->ThreadId = g_next_thread_id++;
    KiReleaseRawSpinLock(&g_scheduler_lock, id_flags);
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

static BOOLEAN KepEnqueueThread(PKTHREAD t)
{
    UINT64 interrupt_state = KiAcquireRawSpinLock(&g_scheduler_lock);
    if (t->Process->Exiting) {
        KiReleaseRawSpinLock(&g_scheduler_lock, interrupt_state);
        return FALSE;
    }
    InsertTailList(&g_ready_queue, &t->ReadyEntry);
    InsertTailList(&t->Process->ThreadListHead, &t->ProcessEntry);
    InsertTailList(&g_all_threads, &t->SchedulerEntry);
    KepWakeIdleProcessors();
    KiReleaseRawSpinLock(&g_scheduler_lock, interrupt_state);
    return TRUE;
}

static void KepFreeUnqueuedThread(PKTHREAD thread)
{
    if (thread->TerminationObject)
        ObDereferenceObject((POBJECT)thread->TerminationObject);
    if (thread->OwningProcessObject)
        ObDereferenceObject(thread->OwningProcessObject);
    if (thread->KernelStackBase)
        ExFreePool((void *)thread->KernelStackBase);
    ExFreePool(thread);
}

PKTHREAD KeCreateThread(const char *name, PKSTART_ROUTINE routine,
                        PVOID context, LONG priority)
{
    PKTHREAD t = KepAllocThread(&g_system_process, name, priority);
    if (!t)
        return NULL;

    t->StartRoutine = routine;
    t->StartContext = context;
    if (!KepEnqueueThread(t)) {
        KepFreeUnqueuedThread(t);
        return NULL;
    }

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
    if (!KepEnqueueThread(t)) {
        KepFreeUnqueuedThread(t);
        return NULL;
    }
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
    if (!KepEnqueueThread(t)) {
        KepFreeUnqueuedThread(t);
        return NULL;
    }

    KeLog("[ke]   created user thread '%s' (id %u): entry %p, ustack %p, teb %p\n",
          name, t->ThreadId, (void *)user_entry, (void *)user_stack,
          (void *)user_gs_base);
    return t;
}

/*
 * KiSchedule - pick the next runnable thread and switch to it.
 * Must be called with interrupts disabled.
 */
static void KiScheduleWithLock(UINT64 scheduler_interrupt_state)
{
    if (KeGetCurrentIrql() > APC_LEVEL)
        KeBugCheck(KE_IRQL_NOT_LESS_OR_EQUAL, "scheduler above APC_LEVEL");
    KepReapTerminatedThreads();
    PKPRCB processor = KeGetCurrentProcessorBlock();
    processor->ReschedulePending = FALSE;
    PKTHREAD prev = processor->CurrentThread;
    PKTHREAD next;

    PKTHREAD idle = processor->IdleThread;
    if (!idle)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "processor has no idle thread");

    /* Release this CPU's address-space execution slot before choosing the
     * next thread. This prevents two CPUs from mutating/running one process
     * address space concurrently while the VM lacks remote TLB shootdowns. */
    if (prev && prev != idle && prev->Process) {
        ULONG active = __atomic_load_n(&prev->Process->ActiveProcessorCount,
                                       __ATOMIC_RELAXED);
        if (!active)
            KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                       "process active-CPU accounting underflow");
        __atomic_sub_fetch(&prev->Process->ActiveProcessorCount, 1,
                           __ATOMIC_RELEASE);
    }

    /* A single global run queue allows migration across CPUs. Skip threads
     * whose process is already executing elsewhere; keep them queued for the
     * processor that currently owns that address space. */
    PLIST_ENTRY best = NULL;
    LONG highest = (-2147483647 - 1);
    for (PLIST_ENTRY e = g_ready_queue.Flink;
         e != &g_ready_queue; e = e->Flink) {
        PKTHREAD candidate = CONTAINING_RECORD(e, KTHREAD, ReadyEntry);
        if (candidate->Process &&
            __atomic_load_n(&candidate->Process->ActiveProcessorCount,
                            __ATOMIC_ACQUIRE) != 0)
            continue;
        LONG score = KepDispatchPriority(candidate);
        if (!best || score > highest) {
            highest = score;
            best = e;
        }
    }
    if (best) {
        RemoveEntryList(best);
        next = CONTAINING_RECORD(best, KTHREAD, ReadyEntry);
    } else if (prev && prev != idle && prev->State == ThreadStateRunning) {
        /* No other eligible work: keep the current thread running rather
         * than bouncing through idle for a full timer tick. */
        next = prev;
    } else {
        next = idle;
    }

    /* Return the outgoing thread to the ready queue if it can still run. The
     * low-level switch tail publishes it only after its stack is inactive. */
    if (prev != idle && prev->State == ThreadStateRunning) {
        prev->State = ThreadStateSwitching;
    }

    if (prev == next) {
        if (prev != idle && prev->Process)
            __atomic_add_fetch(&prev->Process->ActiveProcessorCount, 1,
                               __ATOMIC_RELEASE);
        prev->State = ThreadStateRunning;
        KiReleaseRawSpinLock(&g_scheduler_lock, scheduler_interrupt_state);
        return;
    }

    if (next != idle && next->Process)
        __atomic_add_fetch(&next->Process->ActiveProcessorCount, 1,
                           __ATOMIC_RELEASE);

    next->ContextSwitches++;
    BOOLEAN trace_child_switch = SVINDOWS_EXEC_TRACE && next->ContextSwitches == 1 &&
        next->ProcessMainThread && next->Process && next->Process->ParentProcessId > 4;
    if (trace_child_switch) PsTraceProcessLaunch("6s.1 child selected", next->Name);
    KeGetCurrentProcessorBlock()->ContextSwitches++;
    next->State = ThreadStateRunning;
    next->Quantum = DEFAULT_QUANTUM;
    next->SchedulerBoost = 0;
    KeGetCurrentProcessorBlock()->CurrentThread = next;
    KeSetCurrentThread(next);

    /* Kernel mappings are shared, so it is safe to change CR3 while running
     * on the outgoing kernel stack. User mappings become those of `next`. */
    MmSwitchAddressSpace(next->Process->DirectoryTableBase);
    if (trace_child_switch) PsTraceProcessLaunch("6s.2 child CR3 active", NULL);

    /* Point the CPU's ring-0 entry stack at the incoming thread's kernel stack,
     * so a syscall or interrupt taken from ring 3 lands on the right stack, and
     * set the GS base it will see in ring 3 (its TEB). */
    if (next->KernelStackBase)
        KeSetKernelStack(next->KernelStackBase + next->KernelStackSize);
    else if (next == idle)
        KeSetKernelStack(KeGetProcessorBootstrapStackTop(processor->ProcessorNumber));
    KeSetUserGsBase(next->UserGsBase);
    KeSetUserFsBase(next->UserFsBase);
    if (trace_child_switch) PsTraceProcessLaunch("6s.3 child TLS active", NULL);

    /* Preserve SSE/x87 state across the switch: save ours, load the incoming
     * thread's. Done here (not in the asm) because the incoming thread's area
     * must be restored with a pointer that survives the stack swap. */
    KiSaveFpuState(prev->FpuState);
    KiRestoreFpuState(next->FpuState);
    if (trace_child_switch) PsTraceProcessLaunch("6s.4 child FPU restored", NULL);

    prev->SavedIrql = KiSwitchThreadIrql(next->SavedIrql);

    /* The lock changes hands with the current-thread pointer. The assembly
     * tail releases it on the incoming stack after saving the outgoing RSP. */
    __atomic_store_n(&g_scheduler_lock.OwnerThread,
                     (UINT64)(UINT_PTR)next, __ATOMIC_RELAXED);
    __atomic_store_n(&g_scheduler_lock.OwnerProcessor,
                     processor->ProcessorNumber, __ATOMIC_RELAXED);
    KiSwitchContext(&prev->KernelStackPointer, next->KernelStackPointer, prev);
    /* Control returns here only when `prev` is scheduled again. */
    KiDeliverKernelApcs();
}

static void KiSchedule(void)
{
    UINT64 interrupt_state = KiAcquireSchedulerLock();
    KiScheduleWithLock(interrupt_state);
}

void KiBlockCurrentThreadWithSchedulerLock(UINT64 interrupt_state)
{
    KiScheduleWithLock(interrupt_state);
}

/* Called by KiSwitchContext on the incoming stack, after the outgoing RSP has
 * been saved. At this point a switching-out thread may safely re-enter the
 * shared ready queue, and the scheduler lock can be released. */
void KiFinishContextSwitch(PKTHREAD outgoing)
{
    if (outgoing && outgoing->State == ThreadStateSwitching) {
        if (outgoing->SuspendCount) {
            outgoing->State = ThreadStateSuspended;
        } else {
            outgoing->State = ThreadStateReady;
            InsertTailList(&g_ready_queue, &outgoing->ReadyEntry);
            KepWakeIdleProcessors();
        }
    }
    KiReleaseRawSpinLock(&g_scheduler_lock, 0);
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
    UINT64 interrupt_state = KiAcquireSchedulerLock();
    KiReadyThreadWithSchedulerLock(thread);
    KiReleaseSchedulerLock(interrupt_state);
}

void KiReadyThreadWithSchedulerLock(PKTHREAD thread)
{
    if (thread->State == ThreadStateWaiting) {
        thread->State = thread->SuspendCount ? ThreadStateSuspended :
                                               ThreadStateReady;
        if (!thread->SuspendCount) {
            /* Xorg and its clients frequently wake for socket/input work.
             * Give a just-woken Linux thread one dispatch at +1 priority so
             * a continuously running DOS VM cannot add a full quantum of
             * latency to each display request. The boost is consumed when
             * the scheduler selects the thread and never changes its base or
             * Windows-visible priority. */
            if (thread->Process && thread->Process->LinuxContext &&
                thread->SchedulerBoost < 1)
                thread->SchedulerBoost = 1;
            InsertTailList(&g_ready_queue, &thread->ReadyEntry);
            KepWakeIdleProcessors();
        }
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
    PKTHREAD t = KeGetCurrentProcessorBlock()->CurrentThread;
    KiDeliverKernelApcs();

    if (t->UserMode) {
        if (SVINDOWS_EXEC_TRACE && t->ProcessMainThread && t->Process && t->Process->ParentProcessId > 4)
            PsTraceProcessLaunch("7 enter child user mode", t->Name);
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

    PKTHREAD t = KeGetCurrentProcessorBlock()->CurrentThread;
    if (SVINDOWS_EXEC_TRACE && t->ProcessMainThread && t->Process && t->Process->ParentProcessId > 4)
        PsTraceProcessLaunch("8 child exit", t->Name);
    UINT64 scheduler_flags = KiAcquireSchedulerLock();
    t->State = ThreadStateTerminating;
    KiReleaseSchedulerLock(scheduler_flags);
    LinuxThreadExited(t);
    KeLog("[ke]   thread '%s' (id %u) terminated\n", t->Name, t->ThreadId);

    KeAbandonThreadMutants(t);

    /* Signal the thread object so anyone waiting on this thread wakes up, then
     * release the reference the thread held on it (see NtCreateThreadEx). The
     * dispatcher header is the object body, so the body pointer is the same
     * address. */
    if (t->ProcessMainThread && t->Process) {
        PKPROCESS process = t->Process;
        scheduler_flags = KiAcquireSchedulerLock();
        process->Exiting = TRUE;
        process->ExitStatus = t->ExitStatus;
        KiReleaseSchedulerLock(scheduler_flags);
        PsJobProcessExited(process);
        PLIST_ENTRY head = &process->ThreadListHead;
        for (PLIST_ENTRY entry = head->Flink; entry != head;
             entry = entry->Flink) {
            PKTHREAD other = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
            if (other == t) continue;
            scheduler_flags = KiAcquireSchedulerLock();
            if (other->State == ThreadStateTerminated ||
                other->State == ThreadStateTerminating) {
                KiReleaseSchedulerLock(scheduler_flags);
                continue;
            }
            if (other->State == ThreadStateReady)
                RemoveEntryList(&other->ReadyEntry);
            else if (other->State == ThreadStateWaiting)
                KiCancelThreadWaitWithSchedulerLock(other, t->ExitStatus);
            other->ExitStatus = t->ExitStatus;
            other->State = ThreadStateTerminating;
            KiReleaseSchedulerLock(scheduler_flags);
            LinuxThreadExited(other);
            KeAbandonThreadMutants(other);
            scheduler_flags = KiAcquireSchedulerLock();
            POBJECT termination = KepPublishThreadTerminationWithSchedulerLock(other);
            KiReleaseSchedulerLock(scheduler_flags);
            if (termination) ObDereferenceObject(termination);
            KeLog("[ke]   process exit terminated thread '%s' (id %u)\n",
                  other->Name, other->ThreadId);
        }
        KepSignalProcessTermination(process);
    }
    scheduler_flags = KiAcquireSchedulerLock();
    POBJECT termination = KepPublishThreadTerminationWithSchedulerLock(t);
    KiReleaseSchedulerLock(scheduler_flags);
    if (termination) ObDereferenceObject(termination);
    KiSchedule();

    for (;;)
        __asm__ volatile("hlt");
}

/* Finish a thread whose queue membership and Terminating state were already
 * claimed under the scheduler lock. */
static void KepFinishClaimedTermination(PKTHREAD target, NTSTATUS exit_status)
{
    UINT64 flags;
    LinuxThreadExited(target);
    KeAbandonThreadMutants(target);
    if (target->ProcessMainThread && target->Process) {
        PKPROCESS process = target->Process;
        flags = KiAcquireSchedulerLock();
        process->Exiting = TRUE;
        process->ExitStatus = exit_status;
        KiReleaseSchedulerLock(flags);
        PsJobProcessExited(process);
        for (PLIST_ENTRY entry = process->ThreadListHead.Flink;
             entry != &process->ThreadListHead; entry = entry->Flink) {
            PKTHREAD other = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
            if (other == target || other == KeGetCurrentProcessorBlock()->CurrentThread)
                continue;
            flags = KiAcquireSchedulerLock();
            if (other->State == ThreadStateTerminated ||
                other->State == ThreadStateTerminating) {
                KiReleaseSchedulerLock(flags);
                continue;
            }
            if (other->State == ThreadStateReady)
                RemoveEntryList(&other->ReadyEntry);
            else if (other->State == ThreadStateWaiting)
                KiCancelThreadWaitWithSchedulerLock(other, exit_status);
            other->ExitStatus = exit_status;
            other->State = ThreadStateTerminating;
            KiReleaseSchedulerLock(flags);
            KepWaitForThreadOffProcessors(other);
            LinuxThreadExited(other);
            KeAbandonThreadMutants(other);
            flags = KiAcquireSchedulerLock();
            POBJECT termination = KepPublishThreadTerminationWithSchedulerLock(other);
            KiReleaseSchedulerLock(flags);
            if (termination) ObDereferenceObject(termination);
        }
        KepSignalProcessTermination(process);
    }
    BOOLEAN exit_caller = target->ProcessMainThread &&
                          KeGetCurrentProcessorBlock()->CurrentThread->Process == target->Process;
    flags = KiAcquireSchedulerLock();
    POBJECT termination = KepPublishThreadTerminationWithSchedulerLock(target);
    KiReleaseSchedulerLock(flags);
    if (termination) ObDereferenceObject(termination);
    if (exit_caller) {
        KeGetCurrentProcessorBlock()->CurrentThread->ExitStatus = exit_status;
        KeTerminateThread();
    }
}

static BOOLEAN KepClaimThreadTerminationWithSchedulerLock(PKTHREAD target,
                                                           NTSTATUS exit_status)
{
    if (target->State == ThreadStateTerminated ||
        target->State == ThreadStateTerminating)
        return FALSE;
    if (target->State == ThreadStateReady)
        RemoveEntryList(&target->ReadyEntry);
    else if (target->State == ThreadStateWaiting)
        KiCancelThreadWaitWithSchedulerLock(target, exit_status);
    target->ExitStatus = exit_status;
    target->State = ThreadStateTerminating;
    if (target->ProcessMainThread && target->Process) {
        target->Process->Exiting = TRUE;
        target->Process->ExitStatus = exit_status;
    }
    /* The caller sends one wake IPI after releasing the run-queue lock and
     * waits for the remote stack switch before reclaiming target resources. */
    return TRUE;
}

/* Terminate a non-running thread without letting it re-enter user mode. */
NTSTATUS KeTerminateThreadByPointer(PKTHREAD target, NTSTATUS exit_status)
{
    if (!target) return STATUS_INVALID_HANDLE;
    if (target == KeGetCurrentProcessorBlock()->CurrentThread) {
        target->ExitStatus = exit_status;
        KeTerminateThread();
    }
    UINT64 flags = KiAcquireSchedulerLock();
    BOOLEAN claimed = KepClaimThreadTerminationWithSchedulerLock(target,
                                                                 exit_status);
    KiReleaseSchedulerLock(flags);
    if (!claimed) return STATUS_INVALID_HANDLE;
    KepWaitForThreadOffProcessors(target);
    KepFinishClaimedTermination(target, exit_status);
    return STATUS_SUCCESS;
}

/* Used by terminal-generated default signals. Claim the main thread while it
 * is still protected by the shared thread list, then run normal process exit. */
NTSTATUS KeTerminateProcess(PKPROCESS process, NTSTATUS exit_status)
{
    if (!process) return STATUS_INVALID_PARAMETER;
    PKTHREAD target = NULL;
    UINT64 flags = KiAcquireSchedulerLock();
    for (PLIST_ENTRY entry = process->ThreadListHead.Flink;
         entry != &process->ThreadListHead; entry = entry->Flink) {
        PKTHREAD candidate = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
        if (!candidate->ProcessMainThread) continue;
        if (KepClaimThreadTerminationWithSchedulerLock(candidate, exit_status)) {
            target = candidate;
            break;
        }
    }
    KiReleaseSchedulerLock(flags);
    if (!target) return STATUS_INVALID_HANDLE;
    if (target == KeGetCurrentProcessorBlock()->CurrentThread) {
        KeTerminateThread();
    }
    KepWaitForThreadOffProcessors(target);
    KepFinishClaimedTermination(target, exit_status);
    return STATUS_SUCCESS;
}

static void KepClockDpc(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2)
{
    (void)dpc; (void)context; (void)arg1; (void)arg2;
    UINT64 now = KeGetTickCount();
    KeUpdateSharedData(now);
    KiExpireTimers(now);
    W32kPollUserTimers(now);

    /* Dispatcher timeouts are absolute tick deadlines stored on blocked
     * threads. The process list is stable while interrupts are disabled. */
    UINT64 scheduler_flags = KiAcquireSchedulerLock();
    PLIST_ENTRY head = &g_all_threads;
    for (PLIST_ENTRY entry = head->Flink; entry != head; ) {
        PKTHREAD waiting = CONTAINING_RECORD(entry, KTHREAD, SchedulerEntry);
        entry = entry->Flink;
        if (waiting->State == ThreadStateWaiting && waiting->WaitDeadline &&
            now >= waiting->WaitDeadline)
            KiTimeoutThreadWaitWithSchedulerLock(waiting);
    }
    KiReleaseSchedulerLock(scheduler_flags);
}

#include "CpuAccounting.inc"

void KeClockTick(void)
{
    __atomic_add_fetch(&g_tick_count, 1, __ATOMIC_RELAXED);
    KeInsertQueueDpc(&g_clock_dpc, NULL, NULL);
    PKPRCB processor = KeGetCurrentProcessorBlock();
    PKTHREAD t = processor->CurrentThread;
    if (t->Quantum > 0)
        t->Quantum--;

    if (t->Quantum <= 0)
        processor->ReschedulePending = TRUE;

    /* The 8254/PIC clock is routed only to the BSP. Use its tick as the
     * system-wide scheduling clock and send a local-APIC IPI to each online
     * AP so CPU-local quanta expire and CPU-local idle threads wake. */
    if (processor->ProcessorNumber == 0) {
        UINT32 count = HalGetProcessorCount();
        if (count > KE_MAXIMUM_PROCESSORS) count = KE_MAXIMUM_PROCESSORS;
        for (UINT32 i = 1; i < count; ++i) {
            PKPRCB ap = KeGetProcessorBlock(i);
            if (!ap || ap->StartupState != 2 ||
                !__atomic_load_n(&ap->StartupRelease, __ATOMIC_ACQUIRE))
                continue;
            (void)HalSendIpi(ap->ApicId, HAL_SMP_TICK_VECTOR);
        }
    }
}

/* Called after the IRQ's execution level is restored, with IF still clear.
 * A switch can suspend this trap frame without leaking ISR IRQL to a thread. */
void KiScheduleAfterInterrupt(void)
{
    if (KeGetCurrentProcessorBlock()->ReschedulePending &&
        KeGetCurrentIrql() <= APC_LEVEL)
        KiSchedule();
    KiDeliverKernelApcs();
}
