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
#include <svindows/Multiboot2.h>

#define SVINDOWS_VERSION "0.6.0"

#define BOOT_REPORT_CAPACITY 2048
#define BOOT_REPORT_LABEL_WIDTH 38
static char g_boot_report[BOOT_REPORT_CAPACITY];
static SIZE_T g_boot_report_length;
static BOOLEAN g_boot_report_replayed;
static volatile BOOLEAN g_safe_peripherals;

static BOOLEAN KiBootHasOption(UINT64 mbi_phys, const char *option)
{
    const MB_INFO_HEADER *info = MmPhysToVirt(mbi_phys);
    UINT64 end = mbi_phys + info->total_size;
    SIZE_T length = strlen(option);
    for (UINT64 at = mbi_phys + sizeof(*info); at + sizeof(MB_TAG) <= end;) {
        const MB_TAG *tag = MmPhysToVirt(at);
        if (tag->size < sizeof(*tag) || tag->size > end - at) return FALSE;
        if (tag->type == MB_TAG_TYPE_END) return FALSE;
        if (tag->type == MB_TAG_TYPE_CMDLINE) {
            const char *text = (const char *)(tag + 1);
            SIZE_T size = tag->size - sizeof(*tag);
            for (SIZE_T i = 0; i + length <= size; ++i) {
                if (i && text[i - 1] != ' ') continue;
                if (!memcmp(text + i, option, length) &&
                    (i + length == size || !text[i + length] || text[i + length] == ' '))
                    return TRUE;
            }
        }
        at += ((UINT64)tag->size + 7u) & ~7ULL;
    }
    return FALSE;
}

/* Run before any worker or user thread is queued. Merely programming a timer
 * proves nothing about interrupt delivery on physical firmware. Keep dispatch
 * deferred while observing several actual IRQs, with a bounded polling wait. */
static BOOLEAN KiProbeBootClock(void)
{
    KIRQL old_irql;
    KeRaiseIrql(DISPATCH_LEVEL, &old_irql);
    UINT64 start = KeGetTickCount();
    __sti();
    for (UINT32 spin = 0; spin < 20000000u; ++spin) {
        if (KeGetTickCount() - start >= 3) break;
        __asm__ volatile("pause" ::: "memory");
    }
    __cli();
    BOOLEAN running = KeGetTickCount() - start >= 3;
    KeLowerIrql(old_irql);
    return running;
}

/* Legacy PCI scans and xHCI firmware handoff can be slow or non-responsive on
 * physical hardware. Run those optional transports below the interactive shell
 * so a controller timeout cannot hold the boot sequence before the prompt. */
static void PeripheralInitializationWorker(PVOID context)
{
    (void)context;
    BOOLEAN network_ready = NetInitialize();
    KeLog("[boot] background network initialization %s\n",
          network_ready ? "complete" : "unavailable");

    BOOLEAN usb_ready = UsbInitialize();
    KeLog("[boot] background USB HID initialization %s\n",
          usb_ready ? "complete" : "unavailable");
}

/* Set by the top-level Makefile (PROGRAM=...). This keeps boot-program
 * selection a build-time policy rather than a special kernel code path. */
#ifndef SVINDOWS_BOOT_PROGRAM
#define SVINDOWS_BOOT_PROGRAM "cmd.exe"
#endif
#ifndef SVINDOWS_BOOT_COMMAND_LINE
#define SVINDOWS_BOOT_COMMAND_LINE SVINDOWS_BOOT_PROGRAM
#endif
#ifndef SVINDOWS_ENABLE_SMP
#define SVINDOWS_ENABLE_SMP 0
#endif

/* The input thread bridges hardware queues to USER messages. The window
 * manager owns pointer composition and caption dragging. */
static void InputWorker(PVOID context)
{
    (void)context;

    UINT32 last_seq = (UINT32)-1;
    KTIMER poll_timer;
    KeInitializeTimerEx(&poll_timer, FALSE);

    for (;;) {
        HalPollKeyboard();
        UsbPollInput();
        /* One ordered hardware stream: make, translated character, break.
         * Win32 receives it when a window owns focus; Linux evdev gets its
         * independent mirror and suppresses the native-console fallback. */
        BOOLEAN xorg_owns_win32_input = W32kXorgInputActive();
        BOOLEAN window_owns_keyboard = W32kKeyboardOwner() && !xorg_owns_win32_input;
        BOOLEAN linux_owns_keyboard = LinuxInputKeyboardActive();
        UINT32 event;
        UINT16 pending_key = 0;
        BOOLEAN have_key = FALSE;
        while (KbdReadInputEvent(&event)) {
            if (!window_owns_keyboard) continue;
            if (event & KBD_INPUT_CHARACTER) {
                if (have_key) {
                    W32kInputKey(pending_key, (UINT16)event);
                    have_key = FALSE;
                } else KiUserQueueCharacter((UINT16)event);
            } else {
                if (have_key) W32kInputKey(pending_key, 0);
                pending_key = (UINT16)event;
                have_key = TRUE;
            }
        }
        if (have_key) W32kInputKey(pending_key, 0);

        /* Headless `make run-serial` uses COM1 as the keyboard. */
        for (UINT32 serial_budget = 0;
             serial_budget < 64 && HalSerialDataAvailable(); ++serial_budget) {
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
        if (seq != last_seq && !xorg_owns_win32_input) {
            last_seq = seq;
            W32kInputMouse(MouseState.X, MouseState.Y, MouseState.Buttons);
        }

        ConUpdateCursor(KeGetTickCount());

        KeSetTimerEx(&poll_timer, -(INT64)KE_TIME_UNITS_PER_TICK, 0, NULL);
        KeWaitForSingleObject(&poll_timer.Header);
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
    /* UEFI has no visible VGA text console. Keep the latest completed step
     * on the GOP framebuffer even when there is no COM1 attached. The full
     * report replaces this line before the initial process starts. */
    if (!g_boot_report_replayed && GfxAvailable()) {
        GfxFillRect(0, 0, GfxFramebuffer.Width, GFX_FONT_H, GfxColor(0, 0, 0));
        GfxDrawString(0, 0, name, GfxColor(210, 210, 210), GfxColor(0, 0, 0));
        GfxDrawString(50 * GFX_FONT_W, 0, state,
                      GfxColor(120, 220, 120), GfxColor(0, 0, 0));
    }
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
    g_boot_report_replayed = TRUE;
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
    KeLog("[boot]  Multiboot2 information block at 0x%08x\n",
          (unsigned)mbi_phys);

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
    g_safe_peripherals = KiBootHasOption(mbi_phys, "safe-peripherals");
    KiBootStepOk("Physical / virtual memory");

    /* Discover firmware CPU topology before allocating any per-CPU state.
     * AP startup is deferred until all scheduler and worker state is ready. */
    (void)HalDiscoverProcessors((UINT64)mbi_phys);
    KeLog("[smp]  discovered %u logical processor(s)\n",
          HalGetProcessorCount());
#if !SVINDOWS_ENABLE_SMP
    /* Respect firmware's IMCR route. Enable/EOI the local APIC only when the
     * chipset already sends legacy PIC interrupts through LINT0; never force
     * that route on machines without an IMCR or with direct PIC delivery. */
    BOOLEAN pic_virtual_wire = HalLegacyPicUsesLocalApic();
    /* Enable the local APIC without changing IMCR. This keeps LAPIC EOI ready
     * if firmware routes PIC IRQs through LINT0, while leaving direct PIC
     * routing untouched on boards that connect it straight to the BSP. */
    if (HalInitializeLocalApic()) {
        KeLog("[smp]  BSP local APIC ready; firmware PIC route is %s\n",
              pic_virtual_wire ? "virtual wire" : "direct");
        KiBootStepOk("BSP local APIC EOI / PIC route");
    } else {
        if (pic_virtual_wire) HalRouteLegacyPicDirect();
        KeLog("[smp]  local APIC unavailable; using direct PIC/PIT delivery\n");
        KiBootStepUnavailable("BSP local APIC EOI / PIC route");
    }
#else
    if (HalGetProcessorCount() > 1 && HalInitializeLocalApic()) {
        KeLog("[smp]  BSP local APIC ready for legacy IRQ delivery\n");
        KiBootStepOk("BSP local APIC IRQ route");
    } else {
        KeLog("[smp]  BSP local APIC unavailable; AP startup may fail\n");
        KiBootStepUnavailable("BSP local APIC IRQ route");
    }
#endif

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
    if (!KiProbeBootClock()) {
        KeLog("[boot] local timer IRQ probe failed; trying legacy PIT\n");
        HalUseLegacyTimer();
        if (!KiProbeBootClock()) {
            KiBootStepFailed("System clock interrupt delivery", STATUS_UNSUCCESSFUL);
            KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                       "neither local APIC nor PIT delivers system clock interrupts");
        }
        KiBootStepOk("Verified legacy PIT clock interrupts");
    } else {
        KiBootStepOk("Verified system clock interrupts");
    }

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
    if (KeCreateThread("Win32 Xorg mirror", W32kXorgWorker, NULL, 10))
        KiBootStepOk("Win32 Xorg presentation");
    else
        KiBootStepFailed("Win32 Xorg presentation", STATUS_NO_MEMORY);
    if (g_safe_peripherals) {
        KiBootStepUnavailable("USB / network disabled (safe mode)");
    } else if (KeCreateThread("Peripheral initialization",
                       PeripheralInitializationWorker, NULL, 4))
        KiBootStepOk("Deferred network / USB initialization");
    else
        KiBootStepFailed("Deferred network / USB initialization",
                         STATUS_NO_MEMORY);

    /* The shell can run as soon as it is queued: clock IRQs may preempt the
     * rest of kernel startup. Publish it after PS/2 and input dispatch are
     * ready; slower optional PCI/network/USB initialization runs at low
     * priority so a physical controller timeout cannot block the boot prompt. */
    NTSTATUS st = PsCreateInitialProcess(SVINDOWS_BOOT_PROGRAM,
                                         SVINDOWS_BOOT_COMMAND_LINE);
    if (NT_SUCCESS(st))
        KiBootStepOk("Initial user process");
    else
        KiBootStepFailed("Initial user process", st);

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

    /* Start APs after all shared kernel services and initial worker threads
     * have been published. The APs now execute the multiprocessor scheduler;
     * each CPU receives reschedule IPIs from the BSP's 100 Hz clock tick. */
#if SVINDOWS_ENABLE_SMP
    KiStartApplicationProcessors();
    UINT32 online_processors = 1; /* BSP */
    for (UINT32 i = 1; i < HalGetProcessorCount(); ++i) {
        PKPRCB processor = KeGetProcessorBlock(i);
        if (processor && processor->StartupState == 2 &&
            __atomic_load_n(&processor->StartupRelease, __ATOMIC_ACQUIRE))
            online_processors++;
    }
    KeLog("[smp]  multiprocessor scheduler online: %u/%u CPU\n",
          online_processors, HalGetProcessorCount());
    if (online_processors > 1)
        KiBootStepOk("SMP scheduler / AP workers");
    else
        KiBootStepUnavailable("SMP scheduler / AP workers");
#else
    KiBootStepUnavailable("SMP scheduler / AP workers");
#endif

    /* Become the idle thread. The scheduler runs the ring-3 PE (which makes
     * syscalls) and the kernel worker. */
    ConAppendKernelText("Starting scheduler (clock IRQs verified)...\n",
                       sizeof("Starting scheduler (clock IRQs verified)...\n") - 1);
    __sti();
    KeYield(); /* Dispatch queued work without waiting for the first idle IRQ. */
    /* The BSP owns the only system clock. Until we have an independent
     * broadcast clock/wakeup source, keep it out of firmware C1/C1E states:
     * a LAPIC that ticks during the busy boot probe may stop once HLT enters
     * idle. PS/2 polling and every dispatcher timer then stop with it. PAUSE
     * leaves IRQs enabled and lets the clock continue scheduling real work.
     * This compatibility policy costs idle power on one logical processor. */
    for (;;)
        __asm__ volatile("pause" ::: "memory");
}
