/*
 * svindows/Ke.h - Kernel core (Ke).
 *
 * Entry point, CPU structure init (GDT/IDT), the trap frame, panic/bugcheck,
 * and the kernel-wide formatted logger.
 */
#ifndef _SVINDOWS_KE_H_
#define _SVINDOWS_KE_H_

#include <nt/Ntdef.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Atomic (interlocked) operations                                    */
/* ------------------------------------------------------------------ */

static ALWAYS_INLINE LONG InterlockedIncrement(volatile LONG *value)
{
    return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST);
}

static ALWAYS_INLINE LONG InterlockedDecrement(volatile LONG *value)
{
    return __atomic_sub_fetch(value, 1, __ATOMIC_SEQ_CST);
}

static ALWAYS_INLINE LONG InterlockedExchange(volatile LONG *target, LONG value)
{
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

/* Save RFLAGS and disable interrupts; restore later. Used to make short
 * critical sections atomic against preemption on the local CPU. */
static ALWAYS_INLINE UINT64 KiIrqSave(void)
{
    UINT64 flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static ALWAYS_INLINE void KiIrqRestore(UINT64 flags)
{
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

/* Save / restore the x87+SSE state (XMM registers, MXCSR, ...) to a 512-byte,
 * 16-byte-aligned area. The context switch uses these so threads that use SSE
 * (all user code does) don't clobber each other's XMM registers. */
static ALWAYS_INLINE void KiSaveFpuState(void *area)
{
    __asm__ volatile("fxsave (%0)" : : "r"(area) : "memory");
}

static ALWAYS_INLINE void KiRestoreFpuState(const void *area)
{
    __asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory");
}

/* ------------------------------------------------------------------ */
/* Boot information handed to the kernel by the boot trampoline        */
/* ------------------------------------------------------------------ */

/*
 * Physical pointer to the Multiboot2 information structure and its magic
 * value, captured by Boot.asm and passed to KiSystemStartup. The structures
 * live in low physical memory, which early boot keeps identity-mapped.
 */
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289u

/* ------------------------------------------------------------------ */
/* Kernel logger                                                      */
/* ------------------------------------------------------------------ */

void KeLogInit(void);
int  KeVLog(const char *fmt, va_list ap);
int  KeLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* High frequency diagnostics must not stall release builds on the UART. */
#ifdef SVINDOWS_RELEASE
#define KeTrace(...) do { if (0) KeLog(__VA_ARGS__); } while (0)
#else
#define KeTrace KeLog
#endif


/* Familiar spellings used throughout NT-style code. */
#define DbgPrint  KeLog
#define kprintf   KeLog

/* ------------------------------------------------------------------ */
/* Fatal errors (bugcheck / KeBugCheck)                               */
/* ------------------------------------------------------------------ */

NORETURN void KeBugCheck(ULONG code, const char *message);

/* A few bugcheck codes borrowed from NT semantics. */
#define KE_PHASE0_INITIALIZATION_FAILED 0x00000031u
#define KE_TRAP_UNHANDLED               0x0000007Fu
#define KE_UNEXPECTED_KERNEL_MODE_TRAP  0x0000007Fu
#define KE_IRQL_NOT_LESS_OR_EQUAL        0x0000000Au
#define KE_IRQL_NOT_GREATER_OR_EQUAL     0x00000009u

/* UP/PIC execution contract. DISPATCH_LEVEL and above mask all local IRQs;
 * DIRQL nesting and SMP synchronization are not implemented yet. */
typedef UINT8 KIRQL;
#define PASSIVE_LEVEL  0
#define APC_LEVEL      1
#define DISPATCH_LEVEL 2
#define HIGH_LEVEL     31
struct _KTHREAD;
typedef struct _KSPIN_LOCK {
    volatile UINT32 Held;
    volatile UINT64 OwnerThread;
    volatile UINT32 OwnerProcessor;
} KSPIN_LOCK, *PKSPIN_LOCK;

KIRQL KeGetCurrentIrql(void);
void KeRaiseIrql(KIRQL new_irql, KIRQL *old_irql);
void KeLowerIrql(KIRQL new_irql);
void KeInitializeSpinLock(PKSPIN_LOCK lock);
void KeAcquireSpinLock(PKSPIN_LOCK lock, KIRQL *old_irql);
void KeReleaseSpinLock(PKSPIN_LOCK lock, KIRQL old_irql);
void KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK lock);
void KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK lock);
UINT64 KiAcquireRawSpinLock(PKSPIN_LOCK lock);
void KiReleaseRawSpinLock(PKSPIN_LOCK lock, UINT64 interrupt_state);
/* Scheduler-only: IRQs disabled, both contexts below DISPATCH_LEVEL. */
KIRQL KiSwitchThreadIrql(KIRQL incoming_irql);

/* Resident, caller-owned DPCs. Queue membership ends before invocation, so a
 * routine may requeue itself; its object/context must outlive queued work. */
struct _KDPC;
typedef void (*PKDEFERRED_ROUTINE)(struct _KDPC *dpc, PVOID context,
                                  PVOID argument1, PVOID argument2);
typedef struct _KDPC {
    LIST_ENTRY Entry;
    PKDEFERRED_ROUTINE Routine;
    PVOID Context, Argument1, Argument2;
    BOOLEAN Queued;
} KDPC, *PKDPC;
void KeInitializeDpc(PKDPC dpc, PKDEFERRED_ROUTINE routine, PVOID context);
BOOLEAN KeInsertQueueDpc(PKDPC dpc, PVOID argument1, PVOID argument2);
BOOLEAN KeRemoveQueueDpc(PKDPC dpc);
void KiRetireDpcList(void);
BOOLEAN KiIsExecutingDpc(void);

/* ------------------------------------------------------------------ */
/* Descriptor tables                                                  */
/* ------------------------------------------------------------------ */

void KeInitializeGdt(void);
void KeInitializeGdtForProcessor(ULONG processor_number);
UINT64 KeGetProcessorBootstrapStackTop(ULONG processor_number);
void KeInitializeIdt(void);
void KeLoadIdt(void);

/*
 * KTRAP_FRAME - the register state pushed by our interrupt/exception stubs.
 * The layout is produced by arch/x86_64/Isr.asm and consumed by the C trap
 * dispatcher, so the two must stay in lock-step.
 */
typedef struct _KTRAP_FRAME {
    /* Pushed by the common stub (in this order via `push`), so they appear
     * here from the last-pushed (r15) to the first-pushed (rax) reversed by
     * the stack growing down — see Isr.asm for the exact sequence. */
    UINT64 r15, r14, r13, r12, r11, r10, r9, r8;
    UINT64 rbp, rdi, rsi, rdx, rcx, rbx, rax;

    UINT64 vector;      /* interrupt/exception vector number */
    UINT64 error_code;  /* CPU error code, or 0 if none      */

    /* Pushed automatically by the CPU on interrupt entry. */
    UINT64 rip;
    UINT64 cs;
    UINT64 rflags;
    UINT64 rsp;
    UINT64 ss;
} KTRAP_FRAME, *PKTRAP_FRAME;

/* Called from the assembly stubs with a pointer to the trap frame. */
void KiDispatchTrap(PKTRAP_FRAME frame);

/* ------------------------------------------------------------------ */
/* Threads, processes, and the scheduler                              */
/* ------------------------------------------------------------------ */

typedef enum _KTHREAD_STATE {
    ThreadStateInitialized = 0,
    ThreadStateReady,
    ThreadStateRunning,
    ThreadStateSwitching,
    ThreadStateTerminating,
    ThreadStateWaiting,
    ThreadStateTerminated,
    ThreadStateSuspended,
} KTHREAD_STATE;

typedef void (*PKSTART_ROUTINE)(PVOID StartContext);

struct _KPROCESS;
struct _KTHREAD;
struct _OB_HANDLE_TABLE;
struct _LDR_PROCESS_CONTEXT;

/* ------------------------------------------------------------------ */
/* Dispatcher (waitable) objects                                       */
/* ------------------------------------------------------------------ */

typedef enum _DISPATCHER_TYPE {
    EventNotificationObject = 0, /* stays signaled until reset */
    EventSynchronizationObject,  /* auto-reset: one waiter consumes it */
    SemaphoreObject,             /* SignalState is the count */
    MutantObject,                /* 1 = free, 0 = held */
    ThreadObject,                /* signaled when the thread terminates */
    TimerNotificationObject,
    TimerSynchronizationObject,
} DISPATCHER_TYPE;

/*
 * DISPATCHER_HEADER - the common prefix of every waitable kernel object. A
 * thread waits on an object by casting the object body to this header, so every
 * waitable object body must begin with one.
 */
typedef struct _DISPATCHER_HEADER {
    LONG         Type;         /* DISPATCHER_TYPE */
    volatile LONG SignalState;
    LIST_ENTRY   WaitListHead; /* KWAIT_BLOCKs of waiting threads */
} DISPATCHER_HEADER, *PDISPATCHER_HEADER;

typedef struct _KWAIT_BLOCK {
    LIST_ENTRY        WaitListEntry;
    struct _KTHREAD  *Thread;
    PDISPATCHER_HEADER Object;
} KWAIT_BLOCK, *PKWAIT_BLOCK;

#define KE_MAXIMUM_WAIT_OBJECTS 64

typedef struct _KEVENT {
    DISPATCHER_HEADER Header;
} KEVENT, *PKEVENT;

#define KE_TIME_UNITS_PER_TICK 100000ULL /* PIT 100 Hz, 100 ns units */
typedef struct _KTIMER {
    DISPATCHER_HEADER Header;
    LIST_ENTRY Entry;
    UINT64 DueTick, PeriodTicks;
    PKDPC Dpc;
    BOOLEAN Inserted;
    struct _KTHREAD *StackOwner; /* optional: detach before owner stack teardown */
} KTIMER, *PKTIMER;
void KeInitializeTimerEx(PKTIMER timer, BOOLEAN notification);
BOOLEAN KeSetTimerEx(PKTIMER timer, INT64 due_time, ULONG period_ms, PKDPC dpc);
BOOLEAN KeCancelTimer(PKTIMER timer);
UINT64 KeDueTimeToTicks(INT64 due_time);
void KiExpireTimers(UINT64 now);
void KiCancelThreadTimers(struct _KTHREAD *thread);
void KiCancelThreadTimersWithSchedulerLock(struct _KTHREAD *thread);

typedef struct _KSEMAPHORE {
    DISPATCHER_HEADER Header;
    LONG              Limit;
} KSEMAPHORE, *PKSEMAPHORE;

typedef struct _KMUTANT {
    DISPATCHER_HEADER Header;
    struct _KTHREAD  *OwnerThread;
    LONG              RecursionCount;
    BOOLEAN           Abandoned;
    UINT8             Padding[3];
    LIST_ENTRY        OwnerEntry;
} KMUTANT, *PKMUTANT;

/*
 * KTHREAD - a schedulable thread of execution. Each thread owns a kernel stack;
 * KernelStackPointer holds its saved RSP while it is not the running thread.
 */
/* Bounded user APC queue: no pool allocation while waking a target thread. */
#define KE_MAXIMUM_USER_APCS 16
#define KE_MAXIMUM_KERNEL_APCS 16
typedef void (*PKKERNEL_APC_ROUTINE)(PVOID context, PVOID argument1,
                                     PVOID argument2);
typedef struct _KKERNEL_APC {
    PKKERNEL_APC_ROUTINE Routine;
    PVOID Context;
    PVOID Argument1;
    PVOID Argument2;
} KKERNEL_APC, *PKKERNEL_APC;
typedef struct _KUSER_APC {
    UINT64 Routine;
    UINT64 Context;
    UINT64 Argument1;
    UINT64 Argument2;
} KUSER_APC, *PKUSER_APC;

typedef struct _KTHREAD {
    UINT64          KernelStackPointer; /* saved RSP when switched out       */
    UINT64          KernelStackBase;    /* allocation base (for teardown)    */
    SIZE_T          KernelStackSize;
    KTHREAD_STATE   State;
    LONG            Priority;
    LONG            RelativePriority;
    LONG            Quantum;            /* ticks left in the current slice   */
    PKSTART_ROUTINE StartRoutine;
    PVOID           StartContext;
    LIST_ENTRY      ReadyEntry;         /* link in the scheduler ready queue */
    LIST_ENTRY      ProcessEntry;       /* link in the owning process        */
    ULONG           ThreadId;
    ULONG           SuspendCount;
    const char     *Name;
    struct _KPROCESS *Process;

    /* User-mode threads start life in ring 3 at UserEntry with stack UserStack
     * instead of calling a kernel StartRoutine. UserGsBase is the TEB address
     * the GS segment resolves to while the thread runs in ring 3. UserArg is
     * passed to the entry point in RCX (Windows convention). */
    BOOLEAN         UserMode;
    UINT64          UserEntry;
    UINT64          UserStack;
    UINT64          UserGsBase;
    UINT64          UserArg;

    /* Synchronization: the block used while this thread waits, and the
     * dispatcher header (if any) that is signaled when it terminates. */
    KWAIT_BLOCK        WaitBlocks[KE_MAXIMUM_WAIT_OBJECTS];
    ULONG              WaitBlockCount;
    UINT64             WaitDeadline; /* tick deadline; 0 means infinite */
    NTSTATUS           WaitStatus;
    PDISPATCHER_HEADER TerminationObject;

    /* x87+SSE state saved across context switches (FXSAVE image; 16-aligned). */
    __attribute__((aligned(16))) UINT8 FpuState[512];

    /* User-mode callback machinery (the win32k KiUserCallbackDispatcher
     * equivalent). UserRip/UserRsp/UserRflags mirror the trap frame of the
     * syscall currently being serviced; a service may arm a one-shot redirect
     * (RedirectPending) that sends the sysret into a generated user
     * trampoline (Callback*) which calls a WndProc and re-enters via
     * NtCallbackReturn. The first callback of a chain saves the original
     * caller's frame in Orig*, and the final redirect restores it with the
     * callback's result in RAX. CallbackContinue selects the kernel-side
     * continuation. The entry stub hardcodes these offsets; _Static_asserts
     * in ke/Syscall.c fail the build if the layout drifts. */
    UINT64 UserRip;
    UINT64 UserRsp;
    UINT64 UserRflags;
    UINT64 CallbackRip;
    UINT64 CallbackRsp;
    UINT64 CallbackFlags;
    UINT64 CallbackContinue;
    UINT64 CallbackWindow;
    UINT64 CallbackMsg;
    UINT64 CallbackLParam;
    UINT64 OrigRip;
    UINT64 OrigRsp;
    UINT64 OrigRflags;
    UINT8  CallbackActive;
    UINT8  RedirectPending;
    UINT8  Padding0[6];
    /* The qword the interrupted caller's stub would `ret` through. The
     * callback chain must leave the interrupted frame exactly as it found
     * it (the win32k guarantee); this copy lets the final redirect restore
     * that slot even if ring-3 scratch code clobbered it in between. */
    UINT64 SavedReturnAddress;
    LIST_ENTRY SchedulerEntry; /* global timeout scan, independent of process */
    PVOID            OwningProcessObject;
    struct _LDR_PROCESS_CONTEXT *LoaderContextOverride;
    NTSTATUS         ExitStatus;
    BOOLEAN          ProcessMainThread;
    LIST_ENTRY       OwnedMutantListHead;
    BOOLEAN          ExceptionActive;
    UINT64           ExceptionRecord;
    UINT64           ExceptionContext;
    /* Appended: assembly-visible callback/trap field offsets stay unchanged. */
    UINT64           UserCallbackBase;
    KIRQL            SavedIrql;
    KUSER_APC        UserApcs[KE_MAXIMUM_USER_APCS];
    UINT8            UserApcHead;
    UINT8            UserApcCount;
    BOOLEAN          WaitAlertable;
    /* Mappings owned by NtCreateThreadEx; main-thread mappings belong to the
     * process image and are intentionally left at zero. */
    UINT64           OwnedUserStackBase;
    SIZE_T           OwnedUserStackPages;
    UINT64           OwnedUserTebBase;
    SIZE_T           OwnedUserTebPages;
    UINT64           OwnedTlsBase;
    SIZE_T           OwnedTlsPages;
    KKERNEL_APC      KernelApcs[KE_MAXIMUM_KERNEL_APCS];
    UINT8            KernelApcHead;
    UINT8            KernelApcCount;
    /* Nested USER callbacks preserve the interrupted continuation in kernel
     * memory. Appended so assembly-visible offsets remain unchanged. */
    UINT8            UserCallbackDepth;
    struct {
        UINT64 OrigRip, OrigRsp, OrigRflags, SavedReturnAddress;
        UINT64 Continue, Window, Msg, LParam;
    } UserCallbackFrames[7];
    PVOID           DosContext; /* appended; syscall assembly offsets fixed */
    UINT64          UserFsBase; /* architectural user FS base */
    struct _LINUX_THREAD_CONTEXT *LinuxThread;
    /* One-shot dispatch boost for runnable Linux I/O clients (Xorg, window
     * managers, and terminals). Appended so assembly-visible offsets stay put. */
    LONG            SchedulerBoost;
    UINT64          UserDesktop; /* explicit desktop association, or inherited */
    UINT64          KernelTime, UserTime; /* scheduler samples, 100 ns */
    ULONG           ContextSwitches;
} KTHREAD, *PKTHREAD;

/* Per-processor state reached through GS after swapgs. Keep the first three
 * qwords stable: syscall entry assembly reads the scratch RSP, kernel RSP and
 * current thread at offsets 0, 8 and 16. */
typedef struct _KPRCB {
    UINT64 UserRspScratch;
    UINT64 KernelRsp;
    PKTHREAD CurrentThread;
    struct _KPRCB *Self;       /* +24: stable base for C access via GS */
    UINT32 ProcessorNumber;
    UINT32 ApicId;
    KIRQL Irql;
    BOOLEAN DpcRunning;
    UINT8 Reserved[2];
    UINT64 DispatchFlags;
    ULONG HeldLocks;
    volatile ULONG ReschedulePending;
    volatile ULONG StartupState;   /* 0=offline, 1=booting, 2=online */
    volatile ULONG StartupRelease; /* AP may enter the shared scheduler */
    PKTHREAD IdleThread;
    UINT64 IdleTime, KernelTime, UserTime;
    ULONG ContextSwitches;
    UINT64 DpcTime;
    ULONG InterruptCount;
    UINT64 SystemCalls, PageFaults, OtherIoOperations;
} KPRCB, *PKPRCB;

#define KE_MAXIMUM_PROCESSORS 64u

_Static_assert(__builtin_offsetof(KTHREAD, UserRip) == 0xAC0,
               "KTHREAD.UserRip offset changed");
_Static_assert(__builtin_offsetof(KTHREAD, UserRsp) == 0xAC8,
               "KTHREAD.UserRsp offset changed");
_Static_assert(__builtin_offsetof(KTHREAD, RedirectPending) == 0xB29,
               "KTHREAD.RedirectPending offset changed");


/*
 * KPROCESS - the kernel-owned execution and resource context of a process.
 * Kernel threads use the System process; every user process gets independent
 * lower-half page tables and a private Object Manager handle table.
 */
#define MM_MAX_PROCESS_REGIONS 64
typedef struct _MM_PROCESS_REGION {
    UINT64 Base;
    UINT64 ReserveSize;
    UINT64 CommittedPages;
    ULONG  AllocationProtect;
    ULONG  Protect;
    ULONG  Type;
    BOOLEAN InUse;
    UINT8  Padding[3];
    PVOID  Section; /* referenced section object for MEM_MAPPED views */
    UINT64 SectionOffset;
} MM_PROCESS_REGION, *PMM_PROCESS_REGION;

typedef struct _KPROCESS {
    UINT64     DirectoryTableBase; /* CR3 for this process                    */
    struct _OB_HANDLE_TABLE *HandleTable;
    struct _LDR_PROCESS_CONTEXT *LoaderContext;
    PVOID      Object;
    LIST_ENTRY ThreadListHead;
    LIST_ENTRY JobEntry;
    PVOID      JobObject;
    ULONG      ProcessId;
    ULONG      ParentProcessId;
    ULONG      SecurityUserRid;
    UCHAR      PriorityClass;  /* native PROCESS_PRIORITY_CLASS value */
    LONG       BasePriority;
    NTSTATUS   ExitStatus;
    BOOLEAN    Exiting;
    UINT8      ProcessPadding[3];
    ULONG      ShutdownLevel;
    ULONG      ShutdownFlags;
    volatile ULONG ErrorMode;
    UINT64     UserAllocationNext;
    UINT64     ThreadAllocationNext;
    HANDLE     StandardInput;
    HANDLE     StandardOutput;
    HANDLE     StandardError;
    const char *Name;
    char       ImagePath[260];
    char       CurrentDirectory[260];
    /* NUL-separated NAME=value entries, terminated by a second NUL.
     * Kept with the process so ELF/PE transitions preserve the environment. */
    char       Environment[2048];
    MM_PROCESS_REGION VmRegions[MM_MAX_PROCESS_REGIONS];
    struct _LINUX_PROCESS_CONTEXT *LinuxContext;
    UINT64 KernelTime, UserTime; /* includes threads which have exited */
} KPROCESS, *PKPROCESS;

typedef struct _KE_PROCESS_THREAD_INFO {
    ULONG ThreadId;
    LONG Priority;
    LONG BasePriority;
    ULONG State;
    NTSTATUS ExitStatus;
    UINT64 KernelTime, UserTime;
    ULONG ContextSwitches;
} KE_PROCESS_THREAD_INFO;
ULONG KeSnapshotProcessThreads(PKPROCESS process,
                               KE_PROCESS_THREAD_INFO *buffer, ULONG capacity);

void      KeInitializeScheduler(void);
PKTHREAD  KeCreateThread(const char *name, PKSTART_ROUTINE routine,
                         PVOID context, LONG priority);
PKTHREAD  KeCreateKernelThreadForProcess(PKPROCESS process, const char *name,
                                         PKSTART_ROUTINE routine, PVOID context,
                                         PDISPATCHER_HEADER termination,
                                         LONG priority);
PKTHREAD  KeCreateUserThread(const char *name, UINT64 user_entry,
                            UINT64 user_stack, UINT64 user_gs_base,
                            UINT64 user_arg, LONG priority);
PKTHREAD  KeCreateUserThreadForProcess(PKPROCESS process, const char *name,
                                       UINT64 user_entry, UINT64 user_stack,
                                       UINT64 user_gs_base, UINT64 user_arg,
                                       LONG priority);
PKTHREAD  KeGetCurrentThread(void);
PKPRCB    KeGetCurrentProcessorBlock(void);
PKPRCB    KeGetProcessorBlock(ULONG processor_number);
/* Current-thread access is CPU-local; the fast inline accessor keeps the
 * instruction-emulator hot path free of an extra call. */
static ALWAYS_INLINE PKTHREAD KeGetCurrentThreadFast(void)
{
    return KeGetCurrentProcessorBlock()->CurrentThread;
}
void      KeSetUserFsBase(UINT64 base);
void      KeSetUserGsBase(UINT64 base);
PKTHREAD  KeLookupThreadById(ULONG thread_id);
NTSTATUS  KeSuspendThread(PKTHREAD thread, ULONG *previous_count);
NTSTATUS  KeResumeThread(PKTHREAD thread, ULONG *previous_count);
NTSTATUS  KeTerminateThreadByPointer(PKTHREAD thread, NTSTATUS exit_status);
NTSTATUS  KeTerminateProcess(PKPROCESS process, NTSTATUS exit_status);
void      KeYield(void);
NORETURN void KeTerminateThread(void);

/* Called from the timer interrupt to drive preemption. */
void      KeClockTick(void);
void      KeAccountClockTick(BOOLEAN user_mode);
void      KiScheduleAfterInterrupt(void);
UINT64    KeGetTickCount(void);

/* Emit the saved first-chance record/context after user-mode dispatch has
 * determined that an exception is genuinely unhandled. */
void      KiDumpUserException(UINT64 record_va, UINT64 context_va);

/* Feed hardware input into the minimal process-wide USER message queue. */
void      KiUserQueueCharacter(UINT16 character);
void      KiUserQueueMouse(INT32 x, INT32 y, UINT8 buttons);

/* KUSER_SHARED_DATA: the read-only page (user VA 0x7FFE0000) the kernel keeps
 * current so ring 3 can read the tick count / system time without a syscall. */
void      KeInitializeSharedData(void);
void      KeUpdateSharedData(UINT64 ticks);
UINT64    KeQuerySystemTimeFileTime(void);

/* Scheduler hooks used by the dispatcher (interrupts must be disabled). */
void      KiReadyThread(PKTHREAD thread);   /* move a waiting thread to Ready */
void      KiBlockCurrentThread(void);       /* current is Waiting -> reschedule */
UINT64    KiAcquireSchedulerLock(void);
void      KiReleaseSchedulerLock(UINT64 interrupt_state);
void      KiReadyThreadWithSchedulerLock(PKTHREAD thread);
void      KiBlockCurrentThreadWithSchedulerLock(UINT64 interrupt_state);
void      KiSignalObjectWithSchedulerLock(PDISPATCHER_HEADER header);
void      KiCancelThreadWaitWithSchedulerLock(PKTHREAD thread, NTSTATUS status);

/* ------------------------------------------------------------------ */
/* Synchronization primitives                                          */
/* ------------------------------------------------------------------ */

void KeInitializeEvent(PKEVENT event, BOOLEAN notification, BOOLEAN signaled);
LONG KeSetEvent(PKEVENT event);   /* returns the previous state */
void KeResetEvent(PKEVENT event);

void KeInitializeSemaphore(PKSEMAPHORE sem, LONG initial, LONG limit);
LONG KeReleaseSemaphore(PKSEMAPHORE sem, LONG count);

void KeInitializeMutant(PKMUTANT mutant, BOOLEAN initially_owned);
BOOLEAN KeReleaseMutant(PKMUTANT mutant, LONG *previous_count);
void KeAbandonThreadMutants(PKTHREAD thread);

/* Block until `object` is signaled/acquired. timeout_ticks is zero for a poll,
 * UINT64_MAX for an infinite wait, or a relative number of clock ticks. */
NTSTATUS KeWaitForSingleObject(PDISPATCHER_HEADER object);
NTSTATUS KeWaitForSingleObjectTimeout(PDISPATCHER_HEADER object,
                                      UINT64 timeout_ticks);
NTSTATUS KeWaitForMultipleObjects(ULONG count,
                                  PDISPATCHER_HEADER *objects,
                                  BOOLEAN wait_all, UINT64 timeout_ticks);
NTSTATUS KeWaitForMultipleObjectsAlertable(ULONG count,
                                          PDISPATCHER_HEADER *objects,
                                          BOOLEAN wait_all, UINT64 timeout_ticks,
                                          BOOLEAN alertable);
NTSTATUS KeWaitForSingleObjectTimeoutAlertable(PDISPATCHER_HEADER object,
                                               UINT64 timeout_ticks,
                                               BOOLEAN alertable);
NTSTATUS KeQueueUserApc(PKTHREAD thread, const KUSER_APC *apc);
NTSTATUS KeDequeueUserApc(PKTHREAD thread, PKUSER_APC apc);
BOOLEAN KeHasUserApc(PKTHREAD thread);
/* Special kernel APCs run in the target thread at APC_LEVEL. Callbacks must
 * not block. They do not turn a wait into STATUS_USER_APC. */
NTSTATUS KeQueueKernelApc(PKTHREAD thread, const KKERNEL_APC *apc);
void KiDeliverKernelApcs(void);

/* Cancel a pending dispatcher wait and make the thread runnable. Called by the
 * clock when a wait deadline expires (interrupts already disabled). */
void KiTimeoutThreadWait(PKTHREAD thread);
void KiTimeoutThreadWaitWithSchedulerLock(PKTHREAD thread);
void KiCancelThreadWait(PKTHREAD thread, NTSTATUS status);

/* Signal a header and wake its waiters (interrupts must be disabled). */
void KiSignalObject(PDISPATCHER_HEADER header);

/* ------------------------------------------------------------------ */
/* System calls and user mode (ring 3)                                */
/* ------------------------------------------------------------------ */

/* Program the syscall MSRs and per-CPU block. Call once during boot after the
 * GDT is installed. */
void KiInitializeSystemCalls(void);
void KiInitializeSystemCallsForProcessor(ULONG processor_number);
void KiStartApplicationProcessors(void);

/* Point the CPU's ring-0 entry stack (TSS.RSP0 and the syscall entry's kernel
 * stack) at `kernel_rsp`. The scheduler calls this on every context switch. */
void KeSetKernelStack(UINT64 kernel_rsp);
void KeSetCurrentThread(PKTHREAD thread);

/* Set the GS base a thread will see in ring 3 (its TEB); 0 selects the kernel
 * per-CPU block. The scheduler calls this on every context switch so the
 * kernel-exit swapgs restores the right per-thread GS. */
void KeSetUserGsBase(UINT64 teb);

/* Set the TSS ring-0 stack pointer (implemented in Gdt.c). */
void KeSetTssRsp0(UINT64 rsp0);

/* Assembly: drop to ring 3 at `entry` (arg in RCX) with `user_stack`. */
void KiEnterUserMode(UINT64 entry, UINT64 user_stack, UINT64 arg);

/* The C half of the system-service dispatcher, called from KiSystemCallEntry
 * with the service number, the four register arguments, and the user RSP (from
 * which any stack arguments are gathered, with probing). */
UINT64 KiSystemServiceDispatch(UINT64 number, UINT64 *reg_args, UINT64 user_rsp);
void   KiInitializeServiceTable(void);

#endif /* _SVINDOWS_KE_H_ */
