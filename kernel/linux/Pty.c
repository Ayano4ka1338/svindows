/* Linux Unix98 pseudo terminals. The two byte streams stay in the Linux ABI. */
#include "Internal.h"
#include <svindows/Ex.h>
#include <svindows/Io.h>
#include <svindows/Rtl.h>

#define PTY_LIMIT 16u
#define PTY_CAPACITY 8192u
#define PTY_LINE_CAPACITY 4096u
#define LINUX_O_NONBLOCK 0x800u
#define LINUX_O_CLOEXEC 0x80000u
#define LINUX_ICRNL 0x100u
#define LINUX_OPOST 0x1u
#define LINUX_ONLCR 0x4u
#define LINUX_ICANON 0x2u
#define LINUX_ECHO 0x8u
#define LINUX_ECHOE 0x10u

#define LINUX_TCGETS 0x5401u
#define LINUX_TCSETS 0x5402u
#define LINUX_TCSETSW 0x5403u
#define LINUX_TCSETSF 0x5404u
#define LINUX_TCSBRK 0x5409u
#define LINUX_TCFLSH 0x540bu
#define LINUX_TIOCSCTTY 0x540eu
#define LINUX_TIOCGPGRP 0x540fu
#define LINUX_TIOCSPGRP 0x5410u
#define LINUX_TIOCOUTQ 0x5411u
#define LINUX_TIOCGWINSZ 0x5413u
#define LINUX_TIOCSWINSZ 0x5414u
#define LINUX_FIONREAD 0x541bu
#define LINUX_TIOCPKT 0x5420u
#define LINUX_FIONBIO 0x5421u
#define LINUX_TIOCNOTTY 0x5422u
#define LINUX_TIOCSETD 0x5423u
#define LINUX_TIOCGETD 0x5424u
#define LINUX_TIOCGSID 0x5429u
#define LINUX_TIOCGPTN 0x80045430u
#define LINUX_TIOCSPTLCK 0x40045431u
#define LINUX_TIOCGPTLCK 0x80045439u
#define LINUX_TIOCGPTPEER 0x5441u

typedef struct _PTY_RING {
    UINT8 Bytes[PTY_CAPACITY];
    UINT32 Head, Tail, Count;
} PTY_RING;

typedef struct _PTY_TERMIOS {
    UINT32 Input, Output, Control, Local;
    UINT8 Line, Characters[19];
} PTY_TERMIOS;

typedef struct _PTY_WINSIZE {
    UINT16 Rows, Columns, PixelWidth, PixelHeight;
} PTY_WINSIZE;

typedef struct _LINUX_PTY {
    UINT32 Number, References, MasterCount, SlaveCount;
    UINT32 ForegroundGroup, SessionId;
    BOOLEAN Locked, PacketMode, EofPending;
    PTY_TERMIOS Termios;
    PTY_WINSIZE Size;
    PTY_RING ToMaster, ToSlave;
    UINT8 Line[PTY_LINE_CAPACITY];
    UINT32 LineLength;
} LINUX_PTY;

_Static_assert(sizeof(PTY_TERMIOS) == 36, "Linux termios ABI");
_Static_assert(sizeof(PTY_WINSIZE) == 8, "Linux winsize ABI");

static LINUX_PTY *g_ptys[PTY_LIMIT];

static BOOLEAN pty_path_equals(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return FALSE;
    }
    return !*a && !*b;
}

BOOLEAN LinuxPtyControllingPath(const char *path)
{
    return pty_path_equals(path, "C:\\dev\\tty");
}

BOOLEAN LinuxPtyPath(const char *path, UINT32 *number, BOOLEAN *master)
{
    if (pty_path_equals(path, "C:\\dev\\ptmx")) {
        *number = 0;
        *master = TRUE;
        return TRUE;
    }
    static const char prefix[] = "C:\\dev\\pts\\";
    for (UINT32 i = 0; i < sizeof(prefix) - 1; i++) {
        char x = path[i], y = prefix[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return FALSE;
    }
    const char *digit = path + sizeof(prefix) - 1;
    if (*digit < '0' || *digit > '9') return FALSE;
    UINT32 value = 0;
    for (; *digit >= '0' && *digit <= '9'; digit++) {
        value = value * 10 + (UINT32)(*digit - '0');
        if (value >= PTY_LIMIT) return FALSE;
    }
    if (*digit) return FALSE;
    *number = value;
    *master = FALSE;
    return TRUE;
}

BOOLEAN LinuxPtyExists(UINT32 number)
{
    if (number >= PTY_LIMIT) return FALSE;
    UINT64 irq = KiIrqSave();
    BOOLEAN exists = g_ptys[number] != NULL;
    KiIrqRestore(irq);
    return exists;
}

static void pty_init(LINUX_PTY *pty)
{
    memset(pty, 0, sizeof(*pty));
    pty->Locked = TRUE;
    pty->Termios.Input = LINUX_ICRNL;
    pty->Termios.Output = LINUX_OPOST | LINUX_ONLCR;
    pty->Termios.Control = 0xbf;
    pty->Termios.Local = 0x8a3b; /* ISIG, ICANON, ECHO, IEXTEN */
    pty->Termios.Characters[0] = 3;   /* VINTR */
    pty->Termios.Characters[1] = 28;  /* VQUIT */
    pty->Termios.Characters[2] = 127; /* VERASE */
    pty->Termios.Characters[3] = 21;  /* VKILL */
    pty->Termios.Characters[4] = 4;   /* VEOF */
    pty->Termios.Characters[6] = 1;   /* VMIN */
    pty->Size.Rows = 24;
    pty->Size.Columns = 80;
}

static UINT64 pty_install(PKPROCESS process, UINT64 slot, LINUX_PTY *pty,
                          BOOLEAN master, UINT64 flags)
{
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[slot];
    memset(entry, 0, sizeof(*entry));
    entry->Used = TRUE;
    entry->Kind = LINUX_FD_PTY;
    entry->Pty = pty;
    entry->PtyMaster = master;
    entry->Terminal = (UINT8)pty->Number;
    entry->Mode = (UINT8)(flags & 3u);
    entry->NonBlocking = !!(flags & LINUX_O_NONBLOCK);
    entry->CloseOnExec = !!(flags & LINUX_O_CLOEXEC);
    return slot;
}

UINT64 LinuxPtyOpen(PKPROCESS process, UINT64 slot, UINT32 number,
                    BOOLEAN master, UINT64 flags)
{
    if ((flags & 3u) == 3u || (master && (flags & 3u) != 2u) ||
        (flags & (0x40u | 0x80u | 0x200u | 0x10000u)))
        return LinuxError(LINUX_EINVAL);
    LINUX_PTY *fresh = NULL;
    if (master) {
        fresh = ExAllocatePoolWithTag(NonPagedPool, sizeof(*fresh), 'ytpL');
        if (!fresh) return LinuxError(LINUX_ENOMEM);
        pty_init(fresh);
    }
    UINT64 irq = KiIrqSave();
    LINUX_PTY *pty = NULL;
    if (master) {
        for (UINT32 i = 0; i < PTY_LIMIT; i++)
            if (!g_ptys[i]) {
                fresh->Number = i;
                g_ptys[i] = fresh;
                pty = fresh;
                break;
            }
    } else if (number < PTY_LIMIT && g_ptys[number] &&
               !g_ptys[number]->Locked) {
        pty = g_ptys[number];
    }
    if (pty) {
        pty->References++;
        if (master) pty->MasterCount++;
        else pty->SlaveCount++;
    }
    KiIrqRestore(irq);
    if (fresh && fresh != pty) ExFreePool(fresh);
    if (!pty) return LinuxError(master ? LINUX_EAGAIN : LINUX_ENOENT);
    return pty_install(process, slot, pty, master, flags);
}

UINT64 LinuxPtyOpenControlling(PKPROCESS process, UINT64 slot, UINT64 flags)
{
    if ((flags & 3u) == 3u) return LinuxError(LINUX_EINVAL);
    UINT64 irq = KiIrqSave();
    LINUX_PTY *pty = process->LinuxContext->ControllingPty;
    if (pty) {
        pty->References++;
        pty->SlaveCount++;
    }
    KiIrqRestore(irq);
    return pty ? pty_install(process, slot, pty, FALSE, flags) :
                 LinuxError(LINUX_ENXIO);
}

void LinuxPtyInherit(struct _LINUX_PTY *object, BOOLEAN master)
{
    LINUX_PTY *pty = (LINUX_PTY *)object;
    UINT64 irq = KiIrqSave();
    pty->References++;
    if (master) pty->MasterCount++;
    else pty->SlaveCount++;
    KiIrqRestore(irq);
}

void LinuxPtyControlInherit(struct _LINUX_PTY *object)
{
    LINUX_PTY *pty = (LINUX_PTY *)object;
    UINT64 irq = KiIrqSave();
    pty->References++;
    KiIrqRestore(irq);
}

static void pty_release(LINUX_PTY *pty, INT32 endpoint)
{
    BOOLEAN free_pty = FALSE;
    UINT64 irq = KiIrqSave();
    if (endpoint == 1 && pty->MasterCount) pty->MasterCount--;
    if (endpoint == 0 && pty->SlaveCount) pty->SlaveCount--;
    if (--pty->References == 0) {
        g_ptys[pty->Number] = NULL;
        free_pty = TRUE;
    }
    KiIrqRestore(irq);
    if (free_pty) ExFreePool(pty);
}

void LinuxPtyClose(struct _LINUX_PTY *object, BOOLEAN master)
{
    pty_release((LINUX_PTY *)object, master ? 1 : 0);
}

void LinuxPtyControlClose(struct _LINUX_PTY *object)
{
    pty_release((LINUX_PTY *)object, -1);
}

static BOOLEAN pty_put(PTY_RING *ring, UINT8 value)
{
    if (ring->Count == PTY_CAPACITY) return FALSE;
    ring->Bytes[ring->Head] = value;
    ring->Head = (ring->Head + 1) % PTY_CAPACITY;
    ring->Count++;
    return TRUE;
}

static UINT32 pty_take(PTY_RING *ring, UINT8 *output, UINT32 capacity)
{
    UINT32 copied = 0;
    while (copied < capacity && ring->Count) {
        output[copied++] = ring->Bytes[ring->Tail];
        ring->Tail = (ring->Tail + 1) % PTY_CAPACITY;
        ring->Count--;
    }
    return copied;
}

static void pty_pause(void)
{
    KEVENT event;
    KeInitializeEvent(&event, FALSE, FALSE);
    KeWaitForSingleObjectTimeout(&event.Header, 1);
}

static void pty_echo(LINUX_PTY *pty, UINT8 ch)
{
    if (!(pty->Termios.Local & LINUX_ECHO)) return;
    if (ch == '\n') {
        pty_put(&pty->ToMaster, '\r');
        pty_put(&pty->ToMaster, '\n');
    } else {
        pty_put(&pty->ToMaster, ch);
    }
}

static BOOLEAN pty_input(LINUX_PTY *pty, UINT8 ch)
{
    /* VINTR is a control event, not part of the canonical input line. The
     * command processor consumes it as Ctrl+C and resets its active editor. */
    if ((pty->Termios.Local & 0x0001u) &&
        ch == pty->Termios.Characters[0]) {
        pty->LineLength = 0;
        if (!pty_put(&pty->ToSlave, ch)) return FALSE;
        if (pty->Termios.Local & LINUX_ECHO) {
            pty_put(&pty->ToMaster, '^');
            pty_put(&pty->ToMaster, 'C');
            pty_put(&pty->ToMaster, '\r');
            pty_put(&pty->ToMaster, '\n');
        }
        return TRUE;
    }
    if (ch == '\r' && (pty->Termios.Input & LINUX_ICRNL)) ch = '\n';
    if (!(pty->Termios.Local & LINUX_ICANON)) {
        if (!pty_put(&pty->ToSlave, ch)) return FALSE;
        pty_echo(pty, ch);
        return TRUE;
    }
    if (ch == pty->Termios.Characters[2]) { /* VERASE */
        if (pty->LineLength) {
            pty->LineLength--;
            if (pty->Termios.Local & LINUX_ECHOE) {
                pty_put(&pty->ToMaster, '\b');
                pty_put(&pty->ToMaster, ' ');
                pty_put(&pty->ToMaster, '\b');
            }
        }
        return TRUE;
    }
    if (ch == pty->Termios.Characters[4]) { /* VEOF */
        if (!pty->LineLength) pty->EofPending = TRUE;
        else {
            if (PTY_CAPACITY - pty->ToSlave.Count < pty->LineLength)
                return FALSE;
            for (UINT32 i = 0; i < pty->LineLength; i++)
                pty_put(&pty->ToSlave, pty->Line[i]);
            pty->LineLength = 0;
        }
        return TRUE;
    }
    if (pty->LineLength == PTY_LINE_CAPACITY - 1 && ch != '\n')
        return FALSE;
    if (ch == '\n' && PTY_CAPACITY - pty->ToSlave.Count <
                      pty->LineLength + 1)
        return FALSE;
    pty->Line[pty->LineLength++] = ch;
    pty_echo(pty, ch);
    if (ch == '\n') {
        for (UINT32 i = 0; i < pty->LineLength; i++)
            pty_put(&pty->ToSlave, pty->Line[i]);
        pty->LineLength = 0;
    }
    return TRUE;
}

/* Core byte transfer shared by the Linux descriptor path and the native
 * File-object bridge. Buffers must already be validated. Returns the moved
 * byte count or a negative errno. */
static INT64 pty_stream_transfer(LINUX_PTY *pty, BOOLEAN master,
                                 BOOLEAN write, BOOLEAN nonblocking,
                                 UINT8 *data, UINT64 length)
{
    for (;;) {
        UINT64 irq = KiIrqSave();
        PTY_RING *ring = master ? &pty->ToMaster : &pty->ToSlave;
        if (!write) {
            UINT32 copied = 0;
            if (ring->Count) {
                if (master && pty->PacketMode) {
                    data[0] = 0;
                    copied = 1;
                }
                if (copied < length)
                    copied += pty_take(ring, data + copied,
                                       (UINT32)(length - copied));
            } else if (!master && pty->EofPending) {
                pty->EofPending = FALSE;
                KiIrqRestore(irq);
                return 0;
            }
            BOOLEAN hung_up = master ? !pty->SlaveCount : !pty->MasterCount;
            KiIrqRestore(irq);
            if (copied) return copied;
            if (hung_up) return master ? -LINUX_EIO : 0;
        } else {
            BOOLEAN hung_up = master ? !pty->SlaveCount : !pty->MasterCount;
            if (hung_up) {
                KiIrqRestore(irq);
                return -LINUX_EIO;
            }
            UINT32 consumed = 0;
            while (consumed < length) {
                UINT8 ch = data[consumed];
                if (master) {
                    if (!pty_input(pty, ch)) break;
                } else {
                    if (pty->Termios.Output & LINUX_OPOST &&
                        pty->Termios.Output & LINUX_ONLCR && ch == '\n') {
                        if (PTY_CAPACITY - pty->ToMaster.Count < 2) break;
                        pty_put(&pty->ToMaster, '\r');
                    }
                    if (!pty_put(&pty->ToMaster, ch)) break;
                }
                consumed++;
            }
            KiIrqRestore(irq);
            if (consumed) return consumed;
        }
        if (nonblocking) return -LINUX_EAGAIN;
        pty_pause();
    }
}

UINT64 LinuxPtyReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd,
                         UINT64 buffer, UINT64 length)
{
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    LINUX_PTY *pty = (LINUX_PTY *)entry->Pty;
    if (write ? entry->Mode == 0 : entry->Mode == 1)
        return LinuxError(LINUX_EBADF);
    if (length > LINUX_IO_MAX) return LinuxError(LINUX_EINVAL);
    if (length && !(write ? MmProbeForRead(buffer, length) :
                          MmProbeForWrite(buffer, length)))
        return LinuxError(LINUX_EFAULT);
    if (!length) return 0;
    INT64 moved = pty_stream_transfer(pty, entry->PtyMaster, write,
                                      entry->NonBlocking,
                                      (UINT8 *)(ULONG_PTR)buffer, length);
    return moved < 0 ? LinuxError((INT32)-moved) : (UINT64)moved;
}

UINT32 LinuxPtyPollObject(struct _LINUX_PTY *object, BOOLEAN master,
                          UINT8 mode)
{
    LINUX_PTY *pty = (LINUX_PTY *)object;
    if (!pty) return 0x10u;
    UINT64 irq = KiIrqSave();
    PTY_RING *incoming = master ? &pty->ToMaster : &pty->ToSlave;
    PTY_RING *outgoing = master ? &pty->ToSlave : &pty->ToMaster;
    BOOLEAN hung_up = master ? !pty->SlaveCount : !pty->MasterCount;
    UINT32 ready = 0;
    if (mode != 1 && (incoming->Count || pty->EofPending || hung_up))
        ready |= 1u;
    if (mode != 0 && outgoing->Count < PTY_CAPACITY && !hung_up)
        ready |= 4u;
    if (hung_up) ready |= 0x10u;
    KiIrqRestore(irq);
    return ready;
}

UINT32 LinuxPtyPoll(PKPROCESS process, UINT64 fd)
{
    if (fd >= LINUX_MAX_FDS) return 0x20u;
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    if (!entry->Used || entry->Kind != LINUX_FD_PTY) return 0x20u;
    return LinuxPtyPollObject(entry->Pty, entry->PtyMaster, entry->Mode);
}

UINT64 LinuxPtyIoctl(PKPROCESS process, UINT64 fd, UINT64 command,
                     UINT64 argument)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used ||
        process->LinuxContext->Files[fd].Kind != LINUX_FD_PTY)
        return LinuxError(LINUX_ENOTTY);
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    LINUX_PTY *pty = (LINUX_PTY *)entry->Pty;
    switch (command) {
    case LINUX_TIOCGPTN:
        if (!entry->PtyMaster) return LinuxError(LINUX_ENOTTY);
        if (!MmProbeForWrite(argument, sizeof(INT32))) return LinuxError(LINUX_EFAULT);
        *(INT32 *)(ULONG_PTR)argument = (INT32)pty->Number;
        return 0;
    case LINUX_TIOCSPTLCK:
        if (!entry->PtyMaster) return LinuxError(LINUX_ENOTTY);
        if (!MmProbeForRead(argument, sizeof(INT32))) return LinuxError(LINUX_EFAULT);
        pty->Locked = *(const INT32 *)(ULONG_PTR)argument != 0;
        return 0;
    case LINUX_TIOCGPTLCK:
        if (!MmProbeForWrite(argument, sizeof(INT32))) return LinuxError(LINUX_EFAULT);
        *(INT32 *)(ULONG_PTR)argument = pty->Locked;
        return 0;
    case LINUX_TIOCGPTPEER: {
        if (!entry->PtyMaster) return LinuxError(LINUX_ENOTTY);
        UINT64 slot = 0;
        while (slot < LINUX_MAX_FDS && process->LinuxContext->Files[slot].Used)
            slot++;
        if (slot == LINUX_MAX_FDS) return LinuxError(LINUX_EMFILE);
        return LinuxPtyOpen(process, slot, pty->Number, FALSE, argument);
    }
    case LINUX_TCGETS:
        if (!MmProbeForWrite(argument, sizeof(pty->Termios))) return LinuxError(LINUX_EFAULT);
        memcpy((void *)(ULONG_PTR)argument, &pty->Termios, sizeof(pty->Termios));
        return 0;
    case LINUX_TCSETS:
    case LINUX_TCSETSW:
    case LINUX_TCSETSF:
        if (!MmProbeForRead(argument, sizeof(pty->Termios))) return LinuxError(LINUX_EFAULT);
        memcpy(&pty->Termios, (const void *)(ULONG_PTR)argument, sizeof(pty->Termios));
        return 0;
    case LINUX_TIOCGWINSZ:
        if (!MmProbeForWrite(argument, sizeof(pty->Size))) return LinuxError(LINUX_EFAULT);
        memcpy((void *)(ULONG_PTR)argument, &pty->Size, sizeof(pty->Size));
        return 0;
    case LINUX_TIOCSWINSZ:
        if (!MmProbeForRead(argument, sizeof(pty->Size))) return LinuxError(LINUX_EFAULT);
        memcpy(&pty->Size, (const void *)(ULONG_PTR)argument, sizeof(pty->Size));
        return 0;
    case LINUX_TIOCGPGRP:
    case LINUX_TIOCGSID:
        if (!MmProbeForWrite(argument, sizeof(INT32))) return LinuxError(LINUX_EFAULT);
        *(INT32 *)(ULONG_PTR)argument = (INT32)(command == LINUX_TIOCGSID ?
            pty->SessionId : pty->ForegroundGroup);
        return 0;
    case LINUX_TIOCSPGRP:
        if (!MmProbeForRead(argument, sizeof(INT32))) return LinuxError(LINUX_EFAULT);
        pty->ForegroundGroup = *(const INT32 *)(ULONG_PTR)argument;
        return 0;
    case LINUX_TIOCSCTTY:
        if (entry->PtyMaster) return LinuxError(LINUX_ENOTTY);
        if (process->LinuxContext->ControllingPty != pty) {
            LinuxPtyControlInherit(pty);
            if (process->LinuxContext->ControllingPty)
                LinuxPtyControlClose(process->LinuxContext->ControllingPty);
            process->LinuxContext->ControllingPty = pty;
        }
        pty->SessionId = process->LinuxContext->SessionId;
        pty->ForegroundGroup = process->LinuxContext->ProcessGroupId;
        return 0;
    case LINUX_TIOCNOTTY:
        if (process->LinuxContext->ControllingPty == pty) {
            process->LinuxContext->ControllingPty = NULL;
            LinuxPtyControlClose(pty);
        }
        return 0;
    case LINUX_FIONBIO:
        if (!MmProbeForRead(argument, sizeof(INT32))) return LinuxError(LINUX_EFAULT);
        entry->NonBlocking = *(const INT32 *)(ULONG_PTR)argument != 0;
        return 0;
    case LINUX_FIONREAD:
    case LINUX_TIOCOUTQ:
        if (!MmProbeForWrite(argument, sizeof(INT32))) return LinuxError(LINUX_EFAULT);
        *(INT32 *)(ULONG_PTR)argument = (INT32)(command == LINUX_FIONREAD ?
            (entry->PtyMaster ? pty->ToMaster.Count : pty->ToSlave.Count) :
            (entry->PtyMaster ? pty->ToSlave.Count : pty->ToMaster.Count));
        return 0;
    case LINUX_TIOCPKT:
        if (!entry->PtyMaster) return LinuxError(LINUX_ENOTTY);
        if (!MmProbeForRead(argument, sizeof(INT32))) return LinuxError(LINUX_EFAULT);
        pty->PacketMode = *(const INT32 *)(ULONG_PTR)argument != 0;
        return 0;
    case LINUX_TIOCGETD:
        if (!MmProbeForWrite(argument, sizeof(INT32))) return LinuxError(LINUX_EFAULT);
        *(INT32 *)(ULONG_PTR)argument = 0;
        return 0;
    case LINUX_TIOCSETD:
        if (!MmProbeForRead(argument, sizeof(INT32))) return LinuxError(LINUX_EFAULT);
        return *(const INT32 *)(ULONG_PTR)argument == 0 ? 0 :
               LinuxError(LINUX_EINVAL);
    case LINUX_TCFLSH:
        if (argument > 2) return LinuxError(LINUX_EINVAL);
        if (argument != 1) memset(&pty->ToSlave, 0, sizeof(pty->ToSlave));
        if (argument != 0) memset(&pty->ToMaster, 0, sizeof(pty->ToMaster));
        return 0;
    case LINUX_TCSBRK:
        return 0;
    default:
        return LinuxError(LINUX_ENOTTY);
    }
}

/* ------------------------------------------------------------------ */
/* Native File-object bridge                                           */
/* ------------------------------------------------------------------ */

/* A PTY endpoint published as an NT File object. PE programs (cmd.exe under
 * xterm) and Linux programs then share one terminal through their own native
 * handle kinds, and a handle can travel both directions across exec. */

/* Windows consoles deliver backspace as BS; xterm sends DEL. */
static void pty_bridge_windows_bytes(LINUX_PTY_BRIDGE *bridge, void *buffer,
                                     INT64 moved)
{
    if (!bridge->Windows || bridge->Master) return;
    UINT8 *bytes = buffer;
    for (INT64 i = 0; i < moved; i++)
        if (bytes[i] == 0x7f) bytes[i] = '\b';
}

static SIZE_T pty_bridge_read(void *raw, void *buffer, SIZE_T length)
{
    LINUX_PTY_BRIDGE *bridge = raw;
    INT64 moved = pty_stream_transfer(bridge->Pty, bridge->Master, FALSE,
                                      FALSE, buffer, length);
    if (moved <= 0) return 0;
    pty_bridge_windows_bytes(bridge, buffer, moved);
    return (SIZE_T)moved;
}

static SIZE_T pty_bridge_try_read(void *raw, void *buffer, SIZE_T length)
{
    LINUX_PTY_BRIDGE *bridge = raw;
    INT64 moved = pty_stream_transfer(bridge->Pty, bridge->Master, FALSE,
                                      TRUE, buffer, length);
    if (moved <= 0) return 0;
    pty_bridge_windows_bytes(bridge, buffer, moved);
    return (SIZE_T)moved;
}

static BOOLEAN pty_bridge_input_available(void *raw)
{
    LINUX_PTY_BRIDGE *bridge = raw;
    LINUX_PTY *pty = bridge->Pty;
    UINT64 irq = KiIrqSave();
    PTY_RING *incoming = bridge->Master ? &pty->ToMaster : &pty->ToSlave;
    BOOLEAN ready = incoming->Count != 0 || pty->EofPending ||
                    (bridge->Master ? !pty->SlaveCount : !pty->MasterCount);
    KiIrqRestore(irq);
    return ready;
}

static SIZE_T pty_bridge_write(void *raw, const void *buffer, SIZE_T length)
{
    LINUX_PTY_BRIDGE *bridge = raw;
    INT64 moved = pty_stream_transfer(bridge->Pty, bridge->Master, TRUE,
                                      FALSE, (UINT8 *)buffer, length);
    return moved > 0 ? (SIZE_T)moved : 0;
}

static void pty_bridge_prepare_windows(void *raw)
{
    LINUX_PTY_BRIDGE *bridge = raw;
    if (bridge->Master) return;
    bridge->Windows = TRUE;
    UINT64 irq = KiIrqSave();
    /* Windows-console semantics: the program owns echo and line editing, so
     * the slave delivers raw keystrokes. ICRNL stays enabled: xterm sends CR
     * for Enter while cmd.exe expects LF. */
    bridge->Pty->Termios.Local &=
        ~(LINUX_ICANON | LINUX_ECHO | LINUX_ECHOE);
    KiIrqRestore(irq);
}

static void pty_bridge_close(void *raw)
{
    LINUX_PTY_BRIDGE *bridge = raw;
    LinuxPtyClose(bridge->Pty, bridge->Master);
    ExFreePool(bridge);
}

static const IO_CHAR_DEVICE_OPS g_pty_bridge_ops = {
    .Read = pty_bridge_read,
    .TryRead = pty_bridge_try_read,
    .InputAvailable = pty_bridge_input_available,
    .Write = pty_bridge_write,
    .PrepareWindows = pty_bridge_prepare_windows,
    .Close = pty_bridge_close,
};

NTSTATUS LinuxPtyCreateBridgeFile(struct _LINUX_PTY *object, BOOLEAN master,
                                  HANDLE *out)
{
    LINUX_PTY *pty = (LINUX_PTY *)object;
    LINUX_PTY_BRIDGE *bridge = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*bridge), 'gbpL');
    if (!bridge) return STATUS_NO_MEMORY;
    LinuxPtyInherit(pty, master);
    bridge->Pty = pty;
    bridge->Master = master;
    bridge->Windows = FALSE;
    NTSTATUS status = IoCreateCharDeviceFile(IO_CHAR_DEVICE_TAG_PTY, bridge,
                                             &g_pty_bridge_ops, out);
    if (!NT_SUCCESS(status)) {
        LinuxPtyClose(pty, master);
        ExFreePool(bridge);
    }
    return status;
}
