/*
 * ke/KeMain.c - KiSystemStartup, the kernel's C entry point.
 *
 * Reached from the boot trampoline once the CPU is in 64-bit long mode and the
 * kernel is running out of the higher half. Brings up the console, installs the
 * CPU descriptor tables, reports what it found, and idles.
 */
#include <nt/Ntdef.h>
#include <nt/Ntstatus.h>
#include <svindows/Ke.h>
#include <svindows/Hal.h>
#include <svindows/Mm.h>
#include <svindows/Ex.h>
#include <svindows/Ob.h>
#include <svindows/Ldr.h>
#include <svindows/Ps.h>
#include <svindows/Io.h>
#include <svindows/Console.h>
#include <svindows/Cm.h>
#include <svindows/Gfx.h>
#include <svindows/Win32k.h>
#include <svindows/Input.h>
#include <svindows/Linux.h>
#include <svindows/Net.h>
#include <svindows/Rtl.h>

#define SVINDOWS_VERSION "0.6.0"

#define BOOT_REPORT_CAPACITY 2048
#define BOOT_REPORT_LABEL_WIDTH 38
static char g_boot_report[BOOT_REPORT_CAPACITY];
static SIZE_T g_boot_report_length;

/* Set by the top-level Makefile (PROGRAM=...). This keeps boot-program
 * selection a build-time policy rather than a special kernel code path. */
#ifndef SVINDOWS_BOOT_PROGRAM
#define SVINDOWS_BOOT_PROGRAM "cmd.exe"
#endif
#ifndef SVINDOWS_BOOT_COMMAND_LINE
#define SVINDOWS_BOOT_COMMAND_LINE SVINDOWS_BOOT_PROGRAM
#endif

/* The input thread bridges hardware queues to USER messages. The window
 * manager owns pointer composition and caption dragging. */
static void InputWorker(PVOID context)
{
    (void)context;

    UINT32 last_seq = (UINT32)-1;

    for (;;) {
        /* One ordered hardware stream: make, translated character, break.
         * Win32 receives it when a window owns focus; Linux evdev gets its
         * independent mirror and suppresses the native-console fallback. */
        BOOLEAN window_owns_keyboard = W32kKeyboardOwner();
        BOOLEAN linux_owns_keyboard = LinuxInputKeyboardActive();
        UINT32 event;
        while (KbdReadInputEvent(&event)) {
            if (!window_owns_keyboard) continue;
            if (event & KBD_INPUT_CHARACTER) KiUserQueueCharacter((UINT16)event);
            else W32kInputKey((UINT16)event);
        }

        /* Headless `make run-serial` uses COM1 as the keyboard. */
        while (HalSerialDataAvailable()) {
            char c = HalSerialReadChar();
            if (c == '\r') c = '\n';
            if ((UINT8)c == 127) c = '\b';
            if (c) {
                IoConsoleInputCharacter(c);
                KiUserQueueCharacter((UINT8)(c == '\n' ? '\r' : c));
            }
        }

        /* The console gets its own character/ANSI stream. GUI input already
         * arrived through the ordered hardware stream above. */
        while (KbdDataAvailable()) {
            char c = KbdReadChar();
            /* Win32 windows receive this key through the ordered event stream;
             * Xorg owns it through evdev. Do not also inject it into the
             * hidden native console, where Ctrl+C could kill startx/TWM. */
            if (c && !window_owns_keyboard && !linux_owns_keyboard)
                IoConsoleInputCharacter(c);
        }

        /* Forward pointer motion to the window manager, which hit-tests,
         * activates, drags and posts the mouse messages itself. */
        UINT32 seq = MouseState.Seq;
        if (seq != last_seq) {
            last_seq = seq;
            W32kInputMouse(MouseState.X, MouseState.Y, MouseState.Buttons);
        }

        ConUpdateCursor(KeGetTickCount());

        KeYield();
    }
}

static void print_banner(void)
{
    HalVgaSetColor(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
    KeLog("\n");
    KeLog("  SVINDOWS - NT-compatible operating system for x86-64\n");
    KeLog("  version %s\n", SVINDOWS_VERSION);
    HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));
    KeLog("  --------------------------------------------------\n\n");
}

/* Every successful marker is emitted only after the associated initializer has
 * returned. Initializers that can fail return NTSTATUS; non-returning failures
 * bugcheck, so an [ ok ] marker never masks a failed boot step. */
static void KiBootRecordStep(const char *name, const char *state)
{
    const char *prefix = "  ";
    SIZE_T name_length = strlen(name);

    while (*prefix && g_boot_report_length < BOOT_REPORT_CAPACITY)
        g_boot_report[g_boot_report_length++] = *prefix++;
    for (SIZE_T i = 0; i < BOOT_REPORT_LABEL_WIDTH &&
         g_boot_report_length < BOOT_REPORT_CAPACITY; i++)
        g_boot_report[g_boot_report_length++] = i < name_length ? name[i] : ' ';
    while (*state && g_boot_report_length < BOOT_REPORT_CAPACITY)
        g_boot_report[g_boot_report_length++] = *state++;
    if (g_boot_report_length < BOOT_REPORT_CAPACITY)
        g_boot_report[g_boot_report_length++] = '\n';
}

static void KiReplayBootReport(void)
{
    const SIZE_T label_end = 2 + BOOT_REPORT_LABEL_WIDTH;
    SIZE_T offset = 0;

    while (offset < g_boot_report_length) {
        SIZE_T line_end = offset;
        while (line_end < g_boot_report_length && g_boot_report[line_end] != '\n')
            line_end++;

        ConAppendKernelText(g_boot_report + offset, label_end);
        if (line_end > offset + label_end) {
            const char *state = g_boot_report + offset + label_end;
            UINT8 color = VGA_LGREEN;
            if (state[1] == 'n') color = VGA_YELLOW;
            if (state[1] == 'f') color = VGA_LRED;
            ConAppendKernelTextWithAttribute(state, line_end - offset - label_end,
                                              VGA_COLOR(color, VGA_BLACK));
        }
        if (line_end < g_boot_report_length)
            ConAppendKernelText("\n", 1);
        offset = line_end + 1;
    }
}

static void KiBootStepOk(const char *name)
{
    HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));
    KeLog("  %-48s", name);
    HalVgaSetColor(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    KeLog("[ ok ]\n");
    HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));

    KiBootRecordStep(name, "[ ok ]");
}

static void KiBootStepUnavailable(const char *name)
{
    HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));
    KeLog("  %-48s", name);
    HalVgaSetColor(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    KeLog("[ n/a ]\n");
    HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));

    KiBootRecordStep(name, "[ n/a ]");
}

static void KiBootStepFailed(const char *name, NTSTATUS status)
{
    HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));
    KeLog("  %-48s", name);
    HalVgaSetColor(VGA_COLOR(VGA_LRED, VGA_BLACK));
    KeLog("[ failed: 0x%08x ]\n", (unsigned)status);
    HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));

    KiBootRecordStep(name, "[ failed ]");
}

/*
 * KiSystemStartup - phase 0 kernel initialization.
 *
 * @magic:    the Multiboot2 bootloader magic reported by Limine.
 * @mbi_phys: physical address of the Multiboot2 information structure.
 */
void KiSystemStartup(UINT32 magic, UINT32 mbi_phys)
{
    /* Console first, so everything after this is observable. */
    KeLogInit();
    print_banner();

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        KiBootStepFailed("Validating Multiboot2 hand-off", STATUS_INVALID_PARAMETER);
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "invalid Multiboot2 bootloader magic");
    }
    KiBootStepOk("Multiboot2 hand-off");

    KeInitializeGdt();
    KiBootStepOk("GDT / task state segment");

    KeInitializeIdt();
    KiBootStepOk("Interrupt descriptor table");

    KiInitializeSystemCalls();
    KiBootStepOk("Syscall / exception dispatcher");

    /* Phase 1: memory management. */
    if (GfxRequestBootMode((UINT64)mbi_phys, 1200, 800))
        KeLog("[gfx]  selected 1200x800 via VBE DISPI\n");
    MmInitialize((UINT64)mbi_phys);
    KiBootStepOk("Physical / virtual memory");

    /* Discover firmware CPU topology before allocating any per-CPU state.
     * APs remain parked until their interrupt and scheduler state is ready. */
    (void)HalDiscoverProcessors((UINT64)mbi_phys);
    KeLog("[smp]  discovered %u logical processor(s)\n",
          HalGetProcessorCount());
    /* Keep the established PIC/PIT path on a uniprocessor. Enabling the LAPIC
     * without any AP to start is unnecessary and can leave firmware virtual-
     * wire interrupt state active on machines that booted with the PIC path. */
    if (HalGetProcessorCount() > 1) {
        if (!HalInitializeLocalApic())
            KeLog("[smp]  local APIC unavailable; SMP interrupt delivery disabled\n");
    } else {
        KeLog("[smp]  one processor; using PIC/PIT interrupt delivery\n");
    }

    /* Map the shared user-data page (tick count / system time for ring 3). */
    KeInitializeSharedData();
    KiBootStepOk("Shared user data page");

    /* The framebuffer backs the console. Desktop rendering is not a kernel
     * boot responsibility. */
    BOOLEAN framebuffer_ready = GfxInitialize();
    if (framebuffer_ready) {
        GfxClear(GfxColor(0, 0, 0));
        KiBootStepOk("Framebuffer console");
    } else {
        KiBootStepUnavailable("Framebuffer console");
    }

    /* Kernel pool. */
    ExInitializePool();
    KiBootStepOk("Executive pool allocator");

    /* Phase 2: object manager, then the Ps object types. */
    ObInitialize();
    KiBootStepOk("Object manager / namespace");
    PsInitialize();
    KiBootStepOk("Process / dispatcher objects");

    /* Configuration manager (registry). */
    CmInitialize();
    KiBootStepOk("Registry manager");

    UINT64 initrd_base = 0, initrd_size = 0;
    if (MmFindMultibootModule((UINT64)mbi_phys, "initrd.fat",
                              &initrd_base, &initrd_size) &&
        initrd_base <= ~(UINT64)0 - initrd_size &&
        initrd_size <= (SIZE_T)~(SIZE_T)0 &&
        IoDiskSetMemoryImage(MmPhysToVirt(initrd_base),
                             (SIZE_T)initrd_size)) {
        KeLog("[io]   Limine initrd.fat module: %lu bytes at 0x%lx\n",
              (unsigned long)initrd_size, (unsigned long)initrd_base);
    } else {
        KeLog("[io]   required Limine initrd.fat module not found\n");
    }

    /* win32k's shared SERVERINFO, which USER32 reads during process attach. */
    W32kInitializeServerInfo();
    KiBootStepOk("Win32 subsystem data");

    /* Arm the boot stack's overflow guard before the loader starts recursing
     * through the dependency graph. */
    LdrInitializeStackGuard();
    KiBootStepOk("Kernel stack guard");

    /* Phase 5/6: mount initrd.fat, then load a PE from its FAT volume. */
    NTSTATUS io = IoInitialize();
    const char *filesystem_label = "Limine initrd FAT32 filesystem";
    if (NT_SUCCESS(io))
        KiBootStepOk(filesystem_label);
    else
        KiBootStepFailed(filesystem_label, io);

    KeInitializeScheduler();
    KiBootStepOk("Scheduler");

    HalInitializePic();
    KiBootStepOk("Interrupt controller");
    HalRegisterIrqHandler(0, KeClockTick);
    HalInitializeTimer(100); /* 100 Hz preemption tick */
    KiBootStepOk("100 Hz system timer");

    /* APs initialize their private architectural state, then park on a wake
     * IPI until shared scheduler and dispatcher state is SMP-safe. */
    KiStartApplicationProcessors();
    UINT32 online_processors = 1; /* BSP */
    for (UINT32 i = 1; i < HalGetProcessorCount(); ++i) {
        PKPRCB processor = KeGetProcessorBlock(i);
        if (processor && processor->StartupState == 2)
            online_processors++;
    }
    KeLog("[smp]  SMP запущен: %u из %u CPU онлайн; AP пока припаркованы\n",
          online_processors, HalGetProcessorCount());
    KiBootStepOk("SMP startup");

    /* Bring up the PCI Ethernet transport after the scheduler and timer.
     * Protocols are layered above this frame interface, never in the IRQ path. */
    /* Initialize networking silently; it is a background service, not an
     * interactive boot-status item. Failures remain available through the
     * network subsystem diagnostic/logging paths. */
    (void)NetInitialize();

    /* Do not enqueue the first ring-3 process until the network stack has
     * completed its tcpip_thread/netif initialization. NetInitialize may
     * yield while waiting for lwIP's tcpip callback; if the user process is
     * already queued, the scheduler can run it during that wait and its first
     * AF_INET socket() incorrectly observes ENETDOWN. */
    NTSTATUS st = PsCreateInitialProcess(SVINDOWS_BOOT_PROGRAM,
                                         SVINDOWS_BOOT_COMMAND_LINE);
    if (NT_SUCCESS(st))
        KiBootStepOk("Initial user process");
    else
        KiBootStepFailed("Initial user process", st);

    /* Input: PS/2 keyboard + mouse, feeding the USER message queues. */
    HalInitializeKeyboard();
    HalInitializeMouse();
    /* Expose the same sources as evdev character devices so Linux binaries
     * (Xorg, libinput) can open /dev/input/eventN. */
    LinuxInputInitialize();
    if (GfxAvailable())
        HalMouseSetBounds((INT32)GfxFramebuffer.Width, (INT32)GfxFramebuffer.Height);
    if (KeCreateThread("Input", InputWorker, NULL, 8))
        KiBootStepOk("Keyboard / mouse input");
    else
        KiBootStepFailed("Keyboard / mouse input", STATUS_NO_MEMORY);

    /* IoInitialize created the process-visible console and cleared its screen
     * buffer. Replay the independently collected, real boot results there so
     * they remain visible when cmd.exe starts. */
    KiReplayBootReport();

    KeLog("\n  Free physical memory: %lu MiB\n",
          (unsigned long)((MmFreePageCount() << PAGE_SHIFT) >> 20));
    if (NT_SUCCESS(st)) {
        HalVgaSetColor(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
        KeLog("  Boot completed. Starting %s.\n\n", SVINDOWS_BOOT_PROGRAM);
        HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));
    } else {
        HalVgaSetColor(VGA_COLOR(VGA_LRED, VGA_BLACK));
        KeLog("  Boot completed without an initial user process.\n\n");
        HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));
    }

    /* Become the idle thread. The scheduler runs the ring-3 PE (which makes
     * syscalls) and the kernel worker. */
    __sti();
    for (;;)
        __halt();
}
