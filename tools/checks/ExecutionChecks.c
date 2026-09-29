/* Actual UP IRQL, spin locks and dispatcher, with emulated interrupt flags.
 * No privileged instructions and no test executable on the guest disk. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <svindows/Ke.h>
#include <svindows/Hal.h>
#include <svindows/Input.h>

static UINT64 flags = 0x202;
static UINT64 HostIrqSave(void)
{ UINT64 saved = flags; flags &= ~0x200ULL; return saved; }
static void HostIrqRestore(UINT64 saved) { flags = saved; }
#define KiIrqSave HostIrqSave
#define KiIrqRestore HostIrqRestore
#include "../../kernel/ke/Irql.c"
#include "../../kernel/ke/Dpc.c"
#include "../../kernel/ke/Dispatcher.c"
static UINT8 input_byte;
static UINT8 HostInByte(UINT16 port) { assert(port == 0x60); return input_byte; }
#define __inbyte HostInByte
#include "../../kernel/hal/Keyboard.c"
#include "../../kernel/hal/Mouse.c"
void Ps2ControllerInit(void) {}
UINT8 Ps2MouseCommand(UINT8 command) { (void)command; return 0xfa; }
void HalRegisterIrqHandler(UINT8 irq, HAL_IRQ_HANDLER routine)
{ (void)irq; (void)routine; }
void HalUnmaskIrq(UINT8 irq) { (void)irq; }

static KTHREAD thread;
static jmp_buf fault;
static ULONG expected_bugcheck;
NORETURN void KeBugCheck(ULONG code, const char *message)
{
    (void)message;
    assert(expected_bugcheck && code == expected_bugcheck);
    longjmp(fault, 1);
}
PKTHREAD KeGetCurrentThread(void) { return &thread; }
UINT64 KeGetTickCount(void) { return 10; }
void KiReadyThread(PKTHREAD t) { t->State = ThreadStateReady; }
void KiBlockCurrentThread(void) { abort(); }
void ObReferenceObject(POBJECT object) { (void)object; }
void ObDereferenceObject(POBJECT object) { (void)object; }

#define EXPECT_BUGCHECK(code, expression) do { \
    expected_bugcheck = (code); \
    if (!setjmp(fault)) { expression; assert(!"missing bugcheck"); } \
    expected_bugcheck = 0; \
} while (0)

static KDPC work_a, work_b;
static ULONG order[8], order_count, repeated;
static void Work(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2)
{
    assert(KeGetCurrentIrql() == DISPATCH_LEVEL && !(flags & 0x200));
    assert(context == &thread);
    assert(!dpc->Queued);
    KSPIN_LOCK lock;
    KIRQL old;
    KeInitializeSpinLock(&lock);
    KeAcquireSpinLock(&lock, &old);
    assert(old == DISPATCH_LEVEL);
    KeReleaseSpinLock(&lock, old);
    order[order_count++] = (ULONG)(ULONG_PTR)arg1;
    assert((ULONG_PTR)arg2 == (ULONG_PTR)arg1 + 1);
    KiRetireDpcList(); /* recursive retirement must not invoke anything */
    EXPECT_BUGCHECK(KE_IRQL_NOT_LESS_OR_EQUAL, KeLowerIrql(PASSIVE_LEVEL));
    EXPECT_BUGCHECK(KE_IRQL_NOT_LESS_OR_EQUAL, KeWaitForSingleObject(NULL));
    if ((ULONG_PTR)arg1 == 1)
        assert(KeInsertQueueDpc(dpc, (PVOID)3, (PVOID)4));
}
static void RepeatWork(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2)
{
    (void)context; (void)arg1; (void)arg2;
    if (++repeated < 100) assert(KeInsertQueueDpc(dpc, NULL, NULL));
}
static void FreeWork(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2)
{ (void)context; (void)arg1; (void)arg2; free(dpc); }

static void Drain(void)
{
    KIRQL old;
    KeRaiseIrql(HIGH_LEVEL, &old);
    KeLowerIrql(old);
}
static void Feed(HAL_IRQ_HANDLER irq, UINT8 byte)
{ input_byte = byte; irq(); }

static void CheckDpcs(void)
{
    flags = 0x202;
    KeInitializeDpc(&work_a, Work, &thread);
    KeInitializeDpc(&work_b, Work, &thread);
    assert(KeInsertQueueDpc(&work_a, (PVOID)1, (PVOID)2));
    assert(!KeInsertQueueDpc(&work_a, (PVOID)99, (PVOID)100));
    assert(KeInsertQueueDpc(&work_b, (PVOID)5, (PVOID)6));
    assert(order_count == 0); /* enqueue is not synchronous execution */
    Drain();
    assert(order_count == 3 && order[0] == 1 && order[1] == 5 && order[2] == 3);
    assert(flags == 0x202 && !KiIsExecutingDpc());
    assert(KeInsertQueueDpc(&work_b, NULL, NULL));
    assert(KeRemoveQueueDpc(&work_b));
    assert(!KeRemoveQueueDpc(&work_b));
    assert(!KeRemoveQueueDpc(NULL));
    assert(!KeInsertQueueDpc(NULL, NULL, NULL));
    Drain();
    assert(order_count == 3);
    KeInitializeDpc(&work_a, RepeatWork, NULL);
    KeInsertQueueDpc(&work_a, NULL, NULL);
    Drain();
    assert(repeated == 64 && work_a.Queued); /* bounded runaway requeue */
    Drain();
    assert(repeated == 100 && !work_a.Queued);
    PKDPC temporary = malloc(sizeof(*temporary));
    assert(temporary);
    KeInitializeDpc(temporary, FreeWork, NULL);
    KeInsertQueueDpc(temporary, NULL, NULL);
    Drain(); /* ASan checks that retirement does not use the freed object */
    puts("PASS DPC FIFO, duplicate args, cancellation, requeue, drain budget, IF and callback lifetime");
}

static void CheckInput(void)
{
    HalInitializeKeyboard();
    HalInitializeMouse();
    KIRQL old;
    KeRaiseIrql(HIGH_LEVEL, &old);
    Feed(kbd_irq, 0x2a); Feed(kbd_irq, 0x36); Feed(kbd_irq, 0xaa);
    Feed(kbd_irq, 0x1e); Feed(kbd_irq, 0xb6); Feed(kbd_irq, 0x3a);
    Feed(kbd_irq, 0x2a); Feed(kbd_irq, 0x1e); Feed(kbd_irq, 0xaa);
    Feed(kbd_irq, 0x3a); Feed(kbd_irq, 0xe0); Feed(kbd_irq, 0x48);
    assert(!KbdDataAvailable()); /* ISR never decodes */
    KeLowerIrql(old);
    assert(KbdReadChar() == 'A'); /* right Shift still held */
    assert(KbdReadChar() == 'a'); /* Caps XOR Shift */
    assert(KbdReadChar() == 27 && KbdReadChar() == '[' && KbdReadChar() == 'A');
    assert(!KbdReadChar());
    KeRaiseIrql(HIGH_LEVEL, &old);
    Feed(kbd_irq, 0x2a);
    KeLowerIrql(old);
    KeRaiseIrql(HIGH_LEVEL, &old);
    for (ULONG i = 0; i < HAL_INPUT_BYTES + 2; i++) Feed(kbd_irq, 0);
    Feed(kbd_irq, 0x1e);
    KeLowerIrql(old);
    assert(KbdReadChar() == 'a'); /* lost release must not leave sticky Shift */

    HalMouseSetBounds(100, 100);
    UINT32 seq = MouseState.Seq;
    KeRaiseIrql(HIGH_LEVEL, &old);
    Feed(mouse_irq, 0x09); Feed(mouse_irq, 5); Feed(mouse_irq, 3);
    assert(MouseState.Seq == seq);
    KeLowerIrql(old);
    assert(MouseState.X == 55 && MouseState.Y == 47 && MouseState.Buttons == 1);
    KeRaiseIrql(HIGH_LEVEL, &old);
    Feed(mouse_irq, 0x08); /* incomplete packet preceding raw-ring overflow */
    KeLowerIrql(old);
    KeRaiseIrql(HIGH_LEVEL, &old);
    for (ULONG i = 0; i < HAL_INPUT_BYTES + 2; i++) Feed(mouse_irq, 0);
    Feed(mouse_irq, 0x08); Feed(mouse_irq, 2); Feed(mouse_irq, 1);
    KeLowerIrql(old);
    assert(MouseState.X == 57 && MouseState.Y == 46 && !MouseState.Buttons);
    HalMouseSetBounds(0, -10);
    assert(MouseState.X == 0 && MouseState.Y == 0);
    puts("PASS real keyboard/mouse ISR-to-DPC decoding, modifiers, arrows and overflow resync");
}

int main(void)
{
    KIRQL old, nested;
    KSPIN_LOCK first, second;
    KeInitializeSpinLock(&first);
    KeInitializeSpinLock(&second);
    assert(KeGetCurrentIrql() == PASSIVE_LEVEL);
    KeAcquireSpinLock(&first, &old);
    assert(old == PASSIVE_LEVEL && flags == 2);
    KeAcquireSpinLock(&second, &nested);
    assert(nested == DISPATCH_LEVEL);
    EXPECT_BUGCHECK(KE_IRQL_NOT_LESS_OR_EQUAL, KeLowerIrql(PASSIVE_LEVEL));
    EXPECT_BUGCHECK(KE_IRQL_NOT_LESS_OR_EQUAL, KeAcquireSpinLockAtDpcLevel(&first));
    KeReleaseSpinLock(&second, nested);
    assert(flags == 2 && KeGetCurrentIrql() == DISPATCH_LEVEL);
    KeReleaseSpinLock(&first, old);
    assert(flags == 0x202 && KeGetCurrentIrql() == PASSIVE_LEVEL);
    EXPECT_BUGCHECK(KE_IRQL_NOT_LESS_OR_EQUAL, KeReleaseSpinLockFromDpcLevel(&first));
    EXPECT_BUGCHECK(KE_IRQL_NOT_LESS_OR_EQUAL, KeAcquireSpinLockAtDpcLevel(&first));
    EXPECT_BUGCHECK(KE_IRQL_NOT_GREATER_OR_EQUAL, KeRaiseIrql(HIGH_LEVEL + 1, &old));
    EXPECT_BUGCHECK(KE_IRQL_NOT_GREATER_OR_EQUAL, KeRaiseIrql(APC_LEVEL, NULL));

    /* Starting with IF clear must not enable it on release (ISR/boot paths). */
    flags = 2;
    KeAcquireSpinLock(&first, &old);
    KeReleaseSpinLock(&first, old);
    assert(flags == 2);
    KeRaiseIrql(APC_LEVEL, &old);
    KeRaiseIrql(DISPATCH_LEVEL, &nested);
    EXPECT_BUGCHECK(KE_IRQL_NOT_GREATER_OR_EQUAL, KeRaiseIrql(APC_LEVEL, &old));
    EXPECT_BUGCHECK(KE_IRQL_NOT_LESS_OR_EQUAL, KeLowerIrql(HIGH_LEVEL));
    EXPECT_BUGCHECK(KE_IRQL_NOT_LESS_OR_EQUAL, KiSwitchThreadIrql(PASSIVE_LEVEL));
    KeLowerIrql(nested);
    assert(KeGetCurrentIrql() == APC_LEVEL && flags == 2);
    assert(KiSwitchThreadIrql(PASSIVE_LEVEL) == APC_LEVEL);
    assert(KiSwitchThreadIrql(APC_LEVEL) == PASSIVE_LEVEL);
    KeLowerIrql(PASSIVE_LEVEL);

    KEVENT event;
    KeInitializeEvent(&event, TRUE, TRUE);
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    assert(KeWaitForSingleObjectTimeout(&event.Header, 0) == STATUS_WAIT_0);
    KeResetEvent(&event);
    assert(KeWaitForSingleObjectTimeout(&event.Header, 0) == STATUS_TIMEOUT);
    EXPECT_BUGCHECK(KE_IRQL_NOT_LESS_OR_EQUAL, KeWaitForSingleObject(&event.Header));
    KeLowerIrql(old);

    /* PIC IRQ entry/exit: signals, waits and thread switch are forbidden.
     * A suspended trap restores IF only through the eventual IRET. */
    KeRaiseIrql(HIGH_LEVEL, &old);
    EXPECT_BUGCHECK(KE_IRQL_NOT_LESS_OR_EQUAL, KeSetEvent(&event));
    EXPECT_BUGCHECK(KE_IRQL_NOT_LESS_OR_EQUAL,
                    KeWaitForSingleObjectTimeout(&event.Header, 0));
    EXPECT_BUGCHECK(KE_IRQL_NOT_LESS_OR_EQUAL, KiSwitchThreadIrql(PASSIVE_LEVEL));
    KeLowerIrql(old);
    assert(flags == 2 && KeGetCurrentIrql() == PASSIVE_LEVEL);
    puts("PASS UP execution: IRQL/IF restore, nested locks, misuse bugchecks, wait limits, ISR signal rejection");
    CheckDpcs();
    CheckInput();
    return 0;
}
