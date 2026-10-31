/* Linux VT and KD ioctls over the native text console and boot framebuffer. */
#include "Internal.h"
#include <svindows/Console.h>
#include <svindows/Gfx.h>
#include <svindows/Hal.h>
#include <svindows/Rtl.h>

#define LINUX_VT_MAX 12u
#define LINUX_VT_AUTO 0u
#define LINUX_VT_PROCESS 1u
#define LINUX_KD_TEXT 0u
#define LINUX_KD_GRAPHICS 1u

#define LINUX_VT_OPENQRY 0x5600u
#define LINUX_VT_GETMODE 0x5601u
#define LINUX_VT_SETMODE 0x5602u
#define LINUX_VT_GETSTATE 0x5603u
#define LINUX_VT_RELDISP 0x5605u
#define LINUX_VT_ACTIVATE 0x5606u
#define LINUX_VT_WAITACTIVE 0x5607u
#define LINUX_VT_DISALLOCATE 0x5608u
#define LINUX_KDGKBTYPE 0x4b33u
#define LINUX_KDSETMODE 0x4b3au
#define LINUX_KDGETMODE 0x4b3bu
#define LINUX_KDGKBMODE 0x4b44u
#define LINUX_KDSKBMODE 0x4b45u
#define LINUX_TCGETS 0x5401u
#define LINUX_TCSETS 0x5402u
#define LINUX_TCSETSW 0x5403u
#define LINUX_TCSETSF 0x5404u
#define LINUX_TIOCGPGRP 0x540fu
#define LINUX_TIOCSPGRP 0x5410u
#define LINUX_TIOCGWINSZ 0x5413u
#define LINUX_TIOCSWINSZ 0x5414u
#define LINUX_TIOCSCTTY 0x540eu
#define LINUX_TIOCNOTTY 0x5422u

typedef struct _LINUX_VT_MODE {
    UINT8 Mode, Wait;
    INT16 ReleaseSignal, AcquireSignal, ForceSignal;
} LINUX_VT_MODE;

typedef struct _LINUX_VT_STAT {
    UINT16 Active, Signal, State;
} LINUX_VT_STAT;

typedef struct _LINUX_TTY_TERMIOS {
    UINT32 Input, Output, Control, Local;
    UINT8 Line;
    UINT8 Characters[19];
} LINUX_TTY_TERMIOS;

typedef struct _LINUX_TTY_WINSIZE {
    UINT16 Rows, Columns, PixelWidth, PixelHeight;
} LINUX_TTY_WINSIZE;

typedef struct _LINUX_VT {
    UINT32 OpenCount;
    UINT32 OwnerPid;
    LINUX_VT_MODE Mode;
    LINUX_TTY_TERMIOS Termios;
    UINT8 DisplayMode;
    UINT8 KeyboardMode;
} LINUX_VT;

_Static_assert(sizeof(LINUX_VT_MODE) == 8, "Linux vt_mode ABI");
_Static_assert(sizeof(LINUX_VT_STAT) == 6, "Linux vt_stat ABI");
_Static_assert(sizeof(LINUX_TTY_TERMIOS) == 36, "Linux termios ABI");

static LINUX_VT g_vts[LINUX_VT_MAX + 1];
static UINT8 g_active_vt = 1;
static BOOLEAN g_graphics_lease;

static BOOLEAN linux_vt_same_path(const char *path, const char *expected)
{
    for (; *path && *expected; path++, expected++) {
        char a = *path, b = *expected;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return FALSE;
    }
    return !*path && !*expected;
}

BOOLEAN LinuxVtPath(const char *path, UINT8 *number)
{
    if (linux_vt_same_path(path, "C:\\dev\\console") ||
        linux_vt_same_path(path, "C:\\dev\\tty")) {
        *number = 0;
        return TRUE;
    }
    static const char prefix[] = "C:\\dev\\tty";
    for (UINT32 i = 0; i < sizeof(prefix) - 1; i++) {
        char a = path[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return FALSE;
    }
    const char *digits = path + sizeof(prefix) - 1;
    if (*digits < '0' || *digits > '9') return FALSE;
    UINT32 value = 0;
    for (; *digits >= '0' && *digits <= '9'; digits++) {
        value = value * 10 + (UINT32)(*digits - '0');
        if (value > LINUX_VT_MAX) return FALSE;
    }
    if (*digits) return FALSE;
    *number = (UINT8)value;
    return TRUE;
}

static UINT8 linux_vt_number(UINT8 descriptor_vt)
{
    return descriptor_vt ? descriptor_vt : g_active_vt;
}

static void linux_vt_apply_display_mode(void)
{
    BOOLEAN graphics = g_vts[g_active_vt].DisplayMode == LINUX_KD_GRAPHICS;
    if (graphics && !g_graphics_lease) {
        HalConsoleAcquireFramebuffer();
        g_graphics_lease = TRUE;
    } else if (!graphics && g_graphics_lease) {
        g_graphics_lease = FALSE;
        if (HalConsoleReleaseFramebuffer()) ConRedraw();
    }
}

UINT64 LinuxVtOpen(PKPROCESS process, UINT64 slot, UINT8 number, UINT64 flags)
{
    if ((flags & 3u) == 3u ||
        (flags & (0x40u | 0x80u | 0x200u | 0x10000u)))
        return LinuxError(LINUX_EINVAL);
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[slot];
    memset(entry, 0, sizeof(*entry));
    entry->Kind = LINUX_FD_TTY;
    entry->Terminal = number;
    entry->Mode = (UINT8)(flags & 3u);
    entry->NonBlocking = !!(flags & 0x800u);
    entry->CloseOnExec = !!(flags & 0x80000u);
    entry->Used = TRUE;
    UINT64 irq = KiIrqSave();
    if (number) g_vts[number].OpenCount++;
    KiIrqRestore(irq);
    return slot;
}

void LinuxVtInherit(UINT8 number)
{
    UINT64 irq = KiIrqSave();
    if (number) g_vts[number].OpenCount++;
    KiIrqRestore(irq);
}

void LinuxVtClose(PKPROCESS process, UINT8 number)
{
    (void)process;
    UINT64 irq = KiIrqSave();
    if (number && g_vts[number].OpenCount) g_vts[number].OpenCount--;
    KiIrqRestore(irq);
}

void LinuxVtProcessExit(PKPROCESS process)
{
    BOOLEAN changed = FALSE;
    UINT64 irq = KiIrqSave();
    for (UINT32 number = 1; number <= LINUX_VT_MAX; number++) {
        LINUX_VT *vt = &g_vts[number];
        if (vt->OwnerPid != process->ProcessId) continue;
        vt->OwnerPid = 0;
        memset(&vt->Mode, 0, sizeof(vt->Mode));
        vt->DisplayMode = LINUX_KD_TEXT;
        vt->KeyboardMode = 0;
        if (number == g_active_vt) changed = TRUE;
    }
    KiIrqRestore(irq);
    if (changed) linux_vt_apply_display_mode();
}

UINT64 LinuxVtReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd,
                        UINT64 buffer, UINT64 length)
{
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    if (write ? entry->Mode == 0 : entry->Mode == 1)
        return LinuxError(LINUX_EBADF);
    if (length > LINUX_IO_MAX) return LinuxError(LINUX_EINVAL);
    if (length && !(write ? MmProbeForRead(buffer, length) :
                          MmProbeForWrite(buffer, length)))
        return LinuxError(LINUX_EFAULT);
    if (!length) return 0;
    if (!write && entry->NonBlocking && !ConInputAvailable())
        return LinuxError(LINUX_EAGAIN);
    return write ? ConWrite((const void *)(ULONG_PTR)buffer, length) :
                   ConRead((void *)(ULONG_PTR)buffer, length);
}

UINT32 LinuxVtPoll(PKPROCESS process, UINT64 fd)
{
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    UINT32 ready = 0;
    if (entry->Mode != 1 && ConInputAvailable()) ready |= 1u;
    if (entry->Mode != 0) ready |= 4u;
    return ready;
}

UINT64 LinuxVtIoctl(PKPROCESS process, UINT64 fd, UINT64 command,
                    UINT64 argument)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used)
        return LinuxError(LINUX_EBADF);
    if (process->LinuxContext->Files[fd].Kind != LINUX_FD_TTY)
        return LinuxError(LINUX_ENOTTY);
    UINT8 number = linux_vt_number(process->LinuxContext->Files[fd].Terminal);
    LINUX_VT *vt = &g_vts[number];
    switch (command) {
    case LINUX_VT_OPENQRY: {
        if (!MmProbeForWrite(argument, sizeof(INT32)))
            return LinuxError(LINUX_EFAULT);
        INT32 available = -1;
        for (UINT32 i = 1; i <= LINUX_VT_MAX; i++)
            if (!g_vts[i].OpenCount && i != g_active_vt) {
                available = (INT32)i;
                break;
            }
        *(INT32 *)(ULONG_PTR)argument = available;
        return 0;
    }
    case LINUX_VT_GETMODE:
        if (!MmProbeForWrite(argument, sizeof(vt->Mode)))
            return LinuxError(LINUX_EFAULT);
        memcpy((void *)(ULONG_PTR)argument, &vt->Mode, sizeof(vt->Mode));
        return 0;
    case LINUX_VT_SETMODE: {
        if (!MmProbeForRead(argument, sizeof(LINUX_VT_MODE)))
            return LinuxError(LINUX_EFAULT);
        LINUX_VT_MODE requested = *(LINUX_VT_MODE *)(ULONG_PTR)argument;
        if (requested.Mode != LINUX_VT_AUTO &&
            requested.Mode != LINUX_VT_PROCESS)
            return LinuxError(LINUX_EINVAL);
        if (vt->OwnerPid && vt->OwnerPid != process->ProcessId)
            return LinuxError(LINUX_EPERM);
        requested.ForceSignal = 0;
        vt->Mode = requested;
        vt->OwnerPid = requested.Mode == LINUX_VT_PROCESS ||
                       vt->DisplayMode == LINUX_KD_GRAPHICS ?
                       process->ProcessId : 0;
        return 0;
    }
    case LINUX_VT_GETSTATE: {
        if (!MmProbeForWrite(argument, sizeof(LINUX_VT_STAT)))
            return LinuxError(LINUX_EFAULT);
        LINUX_VT_STAT state = {g_active_vt, 0, 1u};
        for (UINT32 i = 1; i <= LINUX_VT_MAX; i++)
            if (g_vts[i].OpenCount || i == g_active_vt)
                state.State |= (UINT16)(1u << i);
        memcpy((void *)(ULONG_PTR)argument, &state, sizeof(state));
        return 0;
    }
    case LINUX_VT_RELDISP:
        if (vt->OwnerPid && vt->OwnerPid != process->ProcessId)
            return LinuxError(LINUX_EPERM);
        return argument <= 2 ? 0 : LinuxError(LINUX_EINVAL);
    case LINUX_VT_ACTIVATE:
        if (argument < 1 || argument > LINUX_VT_MAX)
            return LinuxError(LINUX_EINVAL);
        if (g_vts[g_active_vt].Mode.Mode == LINUX_VT_PROCESS &&
            g_vts[g_active_vt].OwnerPid != process->ProcessId)
            return LinuxError(LINUX_EOPNOTSUPP);
        g_active_vt = (UINT8)argument;
        linux_vt_apply_display_mode();
        return 0;
    case LINUX_VT_WAITACTIVE: {
        if (argument < 1 || argument > LINUX_VT_MAX)
            return LinuxError(LINUX_EINVAL);
        KEVENT event;
        KeInitializeEvent(&event, FALSE, FALSE);
        while (g_active_vt != argument)
            KeWaitForSingleObjectTimeout(&event.Header, 1);
        return 0;
    }
    case LINUX_VT_DISALLOCATE:
        if (argument > LINUX_VT_MAX) return LinuxError(LINUX_EINVAL);
        if (!argument) return 0;
        return g_vts[argument].OpenCount || argument == g_active_vt ?
               LinuxError(LINUX_EBUSY) : 0;
    case LINUX_KDSETMODE:
        if (argument != LINUX_KD_TEXT && argument != LINUX_KD_GRAPHICS)
            return LinuxError(LINUX_EINVAL);
        if (vt->OwnerPid && vt->OwnerPid != process->ProcessId)
            return LinuxError(LINUX_EPERM);
        vt->DisplayMode = (UINT8)argument;
        if (argument == LINUX_KD_GRAPHICS) vt->OwnerPid = process->ProcessId;
        else if (vt->Mode.Mode == LINUX_VT_AUTO) vt->OwnerPid = 0;
        if (number == g_active_vt) linux_vt_apply_display_mode();
        return 0;
    case LINUX_KDGETMODE:
    case LINUX_KDGKBMODE:
        if (!MmProbeForWrite(argument, sizeof(INT32)))
            return LinuxError(LINUX_EFAULT);
        *(INT32 *)(ULONG_PTR)argument = command == LINUX_KDGETMODE ?
            vt->DisplayMode : vt->KeyboardMode;
        return 0;
    case LINUX_KDSKBMODE:
        if (argument > 4) return LinuxError(LINUX_EINVAL);
        if (vt->OwnerPid && vt->OwnerPid != process->ProcessId)
            return LinuxError(LINUX_EPERM);
        vt->KeyboardMode = (UINT8)argument;
        return 0;
    case LINUX_KDGKBTYPE:
        if (!MmProbeForWrite(argument, 1)) return LinuxError(LINUX_EFAULT);
        *(UINT8 *)(ULONG_PTR)argument = 2; /* KB_101 */
        return 0;
    case LINUX_TCGETS:
        if (!MmProbeForWrite(argument, sizeof(vt->Termios)))
            return LinuxError(LINUX_EFAULT);
        memcpy((void *)(ULONG_PTR)argument, &vt->Termios,
               sizeof(vt->Termios));
        return 0;
    case LINUX_TCSETS:
    case LINUX_TCSETSW:
    case LINUX_TCSETSF:
        if (!MmProbeForRead(argument, sizeof(vt->Termios)))
            return LinuxError(LINUX_EFAULT);
        memcpy(&vt->Termios, (const void *)(ULONG_PTR)argument,
               sizeof(vt->Termios));
        return 0;
    case LINUX_TIOCGWINSZ: {
        if (!MmProbeForWrite(argument, sizeof(LINUX_TTY_WINSIZE)))
            return LinuxError(LINUX_EFAULT);
        LINUX_TTY_WINSIZE size = {0};
        if (GfxAvailable()) {
            size.Rows = (UINT16)(GfxFramebuffer.Height / GFX_FONT_H);
            size.Columns = (UINT16)(GfxFramebuffer.Width / GFX_FONT_W);
            size.PixelWidth = (UINT16)GfxFramebuffer.Width;
            size.PixelHeight = (UINT16)GfxFramebuffer.Height;
        }
        memcpy((void *)(ULONG_PTR)argument, &size, sizeof(size));
        return 0;
    }
    case LINUX_TIOCSWINSZ:
        return MmProbeForRead(argument, sizeof(LINUX_TTY_WINSIZE)) ?
               0 : LinuxError(LINUX_EFAULT);
    case LINUX_TIOCGPGRP:
        if (!MmProbeForWrite(argument, sizeof(INT32)))
            return LinuxError(LINUX_EFAULT);
        *(INT32 *)(ULONG_PTR)argument = (INT32)process->ProcessId;
        return 0;
    case LINUX_TIOCSPGRP:
        return MmProbeForRead(argument, sizeof(INT32)) ?
               0 : LinuxError(LINUX_EFAULT);
    case LINUX_TIOCSCTTY:
    case LINUX_TIOCNOTTY:
        return 0;
    default:
        return LinuxError(LINUX_ENOTTY);
    }
}
