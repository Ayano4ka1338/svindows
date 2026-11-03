/*
 * linux/Input.c - evdev input devices over the native PS/2 drivers.
 *
 * Svindows has no virtual terminals and no kernel input subsystem. Xorg and
 * libinput, however, expect character devices under /dev/input that speak the
 * evdev protocol: a stream of 24-byte struct input_event records carrying a
 * timestamp, a type (EV_KEY/EV_REL/EV_SYN/...), a code, and a value. This file
 * synthesizes that protocol from the existing HAL layer:
 *
 *   event0  - keyboard   (EV_KEY + EV_SYN + EV_MSC), fed by an independent
 *             mirror of the HAL keyboard transition queue
 *   event1  - mouse      (EV_REL + EV_KEY + EV_SYN + EV_MSC), fed by
 *                          MouseReadRelEvent
 *   event2  - a synthetic "combined" node (EV_KEY|EV_REL|EV_SYN|EV_MSC) so a
 *             caller that scans for a single pointer+keyboard device finds one.
 *
 * Each event device keeps a kernel-owned ring of ready input_event records. A
 * small pump drains the HAL rings and the button-transition state into those
 * rings. It is driven lazily from read(), epoll and the syscall path, so no
 * timer support is required.
 *
 * What this deliberately does not do: no key repeat (that is an X-side policy),
 * no LEDs, no device grab, no hotplug. It gives Xorg a stable, well-formed
 * evdev face so its input probe stops failing; the driver-level plumbing is
 * identical to what a real input subsystem would present.
 */
#include "Internal.h"
#include <svindows/Input.h>
#include <svindows/Rtl.h>

/* struct input_event on x86-64: two 64-bit timeval halves, then u16/u16/s32. */
typedef struct _LINUX_INPUT_EVENT {
    INT64  TimeSec;
    INT64  TimeUsec;
    UINT16 Type;
    UINT16 Code;
    INT32  Value;
} LINUX_INPUT_EVENT;

_Static_assert(sizeof(LINUX_INPUT_EVENT) == 24,
               "x86-64 struct input_event must be 24 bytes");

/* Event types (linux/input-event-codes.h). */
#define LINUX_EV_SYN 0x00
#define LINUX_EV_KEY 0x01
#define LINUX_EV_REL 0x02
#define LINUX_EV_MSC 0x04

/* Relative axes. */
#define LINUX_REL_X 0x00
#define LINUX_REL_Y 0x01
#define LINUX_REL_WHEEL 0x08

/* Misc codes. */
#define LINUX_MSC_SCAN 0x04

/* Key codes (uapi/linux/input-event-codes.h). We only need the ones the PS/2
 * set-1 keyboard can produce plus the mouse buttons. */
#define LINUX_BTN_LEFT 0x110
#define LINUX_BTN_RIGHT 0x111
#define LINUX_BTN_MIDDLE 0x112
#define LINUX_KEY_ESC 1
#define LINUX_KEY_1 2
#define LINUX_KEY_0 11
#define LINUX_KEY_MINUS 12
#define LINUX_KEY_EQUAL 13
#define LINUX_KEY_BACKSPACE 14
#define LINUX_KEY_TAB 15
#define LINUX_KEY_Q 16
#define LINUX_KEY_P 25
#define LINUX_KEY_LEFTBRACE 26
#define LINUX_KEY_RIGHTBRACE 27
#define LINUX_KEY_ENTER 28
#define LINUX_KEY_LEFTCTRL 29
#define LINUX_KEY_A 30
#define LINUX_KEY_L 38
#define LINUX_KEY_SEMICOLON 39
#define LINUX_KEY_APOSTROPHE 40
#define LINUX_KEY_GRAVE 41
#define LINUX_KEY_LEFTSHIFT 42
#define LINUX_KEY_BACKSLASH 43
#define LINUX_KEY_Z 44
#define LINUX_KEY_SLASH 53
#define LINUX_KEY_RIGHTSHIFT 54
#define LINUX_KEY_KPASTERISK 55
#define LINUX_KEY_SPACE 57
#define LINUX_KEY_CAPSLOCK 58
#define LINUX_KEY_F1 59
#define LINUX_KEY_RIGHTCTRL 97
#define LINUX_KEY_RIGHTALT 100
#define LINUX_KEY_HOME 102
#define LINUX_KEY_UP 103
#define LINUX_KEY_PAGEUP 104
#define LINUX_KEY_LEFT 105
#define LINUX_KEY_RIGHT 106
#define LINUX_KEY_END 107
#define LINUX_KEY_DOWN 108
#define LINUX_KEY_PAGEDOWN 109
#define LINUX_KEY_INSERT 110
#define LINUX_KEY_DELETE 111
#define LINUX_KEY_LEFTMETA 125
#define LINUX_KEY_RIGHTMETA 126
#define LINUX_KEY_KPENTER 96

/* ioctl requests from linux/input.h (EVIOC*). */
#define LINUX_EVIOCGID 0x80084502u
#define LINUX_EVIOCGVERSION 0x80044501u
#define LINUX_EVIOCGNAME(len) (0x80000000u | ((UINT32)(len) << 16) | \
                               (0x45u << 8) | 0x06u)
#define LINUX_EVIOCGPHYS(len) (0x80000000u | ((UINT32)(len) << 16) | \
                               (0x45u << 8) | 0x07u)
#define LINUX_EVIOCGUNIQ(len) (0x80000000u | ((UINT32)(len) << 16) | \
                               (0x45u << 8) | 0x08u)
#define LINUX_EVIOCGBIT(ev, len) ((UINT32)0x80000000u | \
                                  ((UINT32)(len) << 16) | \
                                  ((UINT32)(ev) << 8) | 0x20u)
#define LINUX_EVIOCGKEY(len) (0x80000000u | ((UINT32)(len) << 16) | \
                              (0x45u << 8) | 0x18u)
#define LINUX_EVIOC_SYNCN 0u

#define LINUX_BIT(x) (1ULL << ((x) & 63))
#define LINUX_BIT_WORD(x) ((x) >> 6)

/* ------------------------------------------------------------------ */
/* Device table                                                       */
/* ------------------------------------------------------------------ */

static LINUX_EVENT_DEVICE g_linux_event_devices[LINUX_EVENT_DEVICE_COUNT];
static BOOLEAN g_linux_input_ready;

/* Ready-event rings. Each record is one input_event ready to be copied out. */
typedef struct _LINUX_EVENT_RING {
    LINUX_INPUT_EVENT Records[LINUX_EVENT_RING_SIZE];
    UINT32 Head, Tail;
    UINT32 Dropped;
} LINUX_EVENT_RING;

static LINUX_EVENT_RING g_linux_event_ring[LINUX_EVENT_DEVICE_COUNT];

/* Keyboard decode state mirrored from the HAL stream. */
static BOOLEAN g_linux_kbd_shift_l, g_linux_kbd_shift_r;
static BOOLEAN g_linux_kbd_ctrl_l, g_linux_kbd_ctrl_r;
static BOOLEAN g_linux_kbd_alt_l, g_linux_kbd_alt_r;
static BOOLEAN g_linux_kbd_meta_l, g_linux_kbd_meta_r;
static BOOLEAN g_linux_kbd_caps;

/* Mouse button state last folded into event1, so transitions emit EV_KEY. */
static UINT8 g_linux_mouse_buttons;

static void linux_input_timestamp(INT64 *sec, INT64 *usec)
{
    /* KeQuerySystemTimeFileTime is 100 ns units since 1601; convert to a
     * Unix-epoch seconds/microseconds pair for the event timestamp. */
    UINT64 file_time = KeQuerySystemTimeFileTime();
    UINT64 unix_100ns = file_time - 116444736000000000ULL;
    *sec = (INT64)(unix_100ns / 10000000ULL);
    *usec = (INT64)((unix_100ns % 10000000ULL) / 10ULL);
}

static void linux_event_emit(UINT32 device, UINT16 type, UINT16 code,
                             INT32 value)
{
    if (device >= LINUX_EVENT_DEVICE_COUNT)
        return;
    LINUX_EVENT_RING *ring = &g_linux_event_ring[device];
    UINT32 next = (ring->Head + 1) % LINUX_EVENT_RING_SIZE;
    if (next == ring->Tail) {
        ring->Dropped++;
        return; /* drop oldest-would-be-lost: keep the stream contiguous */
    }
    INT64 sec, usec;
    linux_input_timestamp(&sec, &usec);
    ring->Records[ring->Head].TimeSec = sec;
    ring->Records[ring->Head].TimeUsec = usec;
    ring->Records[ring->Head].Type = type;
    ring->Records[ring->Head].Code = code;
    ring->Records[ring->Head].Value = value;
    ring->Head = next;
}

/* EV_SYN/SYN_REPORT after each logical packet is what makes an evdev reader
 * flush one frame. Emit it on both the per-device and the combined node. */
static void linux_event_sync(UINT32 device)
{
    linux_event_emit(device, LINUX_EV_SYN, 0, 0);
    if (device != 2)
        linux_event_emit(2, LINUX_EV_SYN, 0, 0);
}

/* ------------------------------------------------------------------ */
/* PS/2 set-1 -> Linux KEY_* translation                              */
/* ------------------------------------------------------------------ */

/* Returns the Linux key code for a make/break code, or 0 if unmapped. The HAL
 * reports the low 7 bits of the scancode; extended keys carry 0x200. */
static UINT16 linux_key_from_scancode(UINT32 code, BOOLEAN extended)
{
    if (extended) {
        switch (code) {
        case 0x1D: return LINUX_KEY_RIGHTCTRL;
        case 0x38: return LINUX_KEY_RIGHTALT;
        case 0x5B: return LINUX_KEY_LEFTMETA;
        case 0x5C: return LINUX_KEY_RIGHTMETA;
        case 0x47: return LINUX_KEY_HOME;
        case 0x48: return LINUX_KEY_UP;
        case 0x49: return LINUX_KEY_PAGEUP;
        case 0x4B: return LINUX_KEY_LEFT;
        case 0x4D: return LINUX_KEY_RIGHT;
        case 0x4F: return LINUX_KEY_END;
        case 0x50: return LINUX_KEY_DOWN;
        case 0x51: return LINUX_KEY_PAGEDOWN;
        case 0x52: return LINUX_KEY_INSERT;
        case 0x53: return LINUX_KEY_DELETE;
        case 0x1C: return LINUX_KEY_KPENTER;
        case 0x35: return LINUX_KEY_SLASH;
        default:   return 0;
        }
    }
    switch (code) {
    case 0x01: return LINUX_KEY_ESC;
    case 0x02: return LINUX_KEY_1;
    case 0x03: return LINUX_KEY_1 + 1;   /* 2  */
    case 0x04: return LINUX_KEY_1 + 2;   /* 3  */
    case 0x05: return LINUX_KEY_1 + 3;   /* 4  */
    case 0x06: return LINUX_KEY_1 + 4;   /* 5  */
    case 0x07: return LINUX_KEY_1 + 5;   /* 6  */
    case 0x08: return LINUX_KEY_1 + 6;   /* 7  */
    case 0x09: return LINUX_KEY_1 + 7;   /* 8  */
    case 0x0A: return LINUX_KEY_1 + 8;   /* 9  */
    case 0x0B: return LINUX_KEY_0;
    case 0x0C: return LINUX_KEY_MINUS;
    case 0x0D: return LINUX_KEY_EQUAL;
    case 0x0E: return LINUX_KEY_BACKSPACE;
    case 0x0F: return LINUX_KEY_TAB;
    case 0x10: return LINUX_KEY_Q;
    case 0x11: return LINUX_KEY_Q + 1;   /* W  */
    case 0x12: return LINUX_KEY_Q + 2;   /* E  */
    case 0x13: return LINUX_KEY_Q + 3;   /* R  */
    case 0x14: return LINUX_KEY_Q + 4;   /* T  */
    case 0x15: return LINUX_KEY_Q + 5;   /* Y  */
    case 0x16: return LINUX_KEY_Q + 6;   /* U  */
    case 0x17: return LINUX_KEY_Q + 7;   /* I  */
    case 0x18: return LINUX_KEY_Q + 8;   /* O  */
    case 0x19: return LINUX_KEY_P;
    case 0x1A: return LINUX_KEY_LEFTBRACE;
    case 0x1B: return LINUX_KEY_RIGHTBRACE;
    case 0x1C: return LINUX_KEY_ENTER;
    case 0x1D: return LINUX_KEY_LEFTCTRL;
    case 0x1E: return LINUX_KEY_A;
    case 0x1F: return LINUX_KEY_A + 1;   /* S  */
    case 0x20: return LINUX_KEY_A + 2;   /* D  */
    case 0x21: return LINUX_KEY_A + 3;   /* F  */
    case 0x22: return LINUX_KEY_A + 4;   /* G  */
    case 0x23: return LINUX_KEY_A + 5;   /* H  */
    case 0x24: return LINUX_KEY_A + 6;   /* J  */
    case 0x25: return LINUX_KEY_A + 7;   /* K  */
    case 0x26: return LINUX_KEY_L;
    case 0x27: return LINUX_KEY_SEMICOLON;
    case 0x28: return LINUX_KEY_APOSTROPHE;
    case 0x29: return LINUX_KEY_GRAVE;
    case 0x2A: return LINUX_KEY_LEFTSHIFT;
    case 0x2B: return LINUX_KEY_BACKSLASH;
    case 0x2C: return LINUX_KEY_Z;
    case 0x2D: return LINUX_KEY_Z + 1;   /* X  */
    case 0x2E: return LINUX_KEY_Z + 2;   /* C  */
    case 0x2F: return LINUX_KEY_Z + 3;   /* V  */
    case 0x30: return LINUX_KEY_Z + 4;   /* B  */
    case 0x31: return LINUX_KEY_Z + 5;   /* N  */
    case 0x32: return LINUX_KEY_Z + 6;   /* M  */
    case 0x33: return LINUX_KEY_Z + 7;   /* ,  */
    case 0x34: return LINUX_KEY_Z + 8;   /* .  */
    case 0x35: return LINUX_KEY_SLASH;
    case 0x36: return LINUX_KEY_RIGHTSHIFT;
    case 0x37: return LINUX_KEY_KPASTERISK;
    case 0x39: return LINUX_KEY_SPACE;
    case 0x3A: return LINUX_KEY_CAPSLOCK;
    case 0x3B: return LINUX_KEY_F1;
    case 0x3C: return LINUX_KEY_F1 + 1;  /* F2  */
    case 0x3D: return LINUX_KEY_F1 + 2;  /* F3  */
    case 0x3E: return LINUX_KEY_F1 + 3;  /* F4  */
    case 0x3F: return LINUX_KEY_F1 + 4;  /* F5  */
    case 0x40: return LINUX_KEY_F1 + 5;  /* F6  */
    case 0x41: return LINUX_KEY_F1 + 6;  /* F7  */
    case 0x42: return LINUX_KEY_F1 + 7;  /* F8  */
    case 0x43: return LINUX_KEY_F1 + 8;  /* F9  */
    case 0x44: return LINUX_KEY_F1 + 9;  /* F10 */
    default:   return 0;
    }
}

static void linux_key_state_track(UINT16 key, BOOLEAN down)
{
    switch (key) {
    case LINUX_KEY_LEFTSHIFT: g_linux_kbd_shift_l = down; break;
    case LINUX_KEY_RIGHTSHIFT: g_linux_kbd_shift_r = down; break;
    case LINUX_KEY_LEFTCTRL: g_linux_kbd_ctrl_l = down; break;
    case LINUX_KEY_RIGHTCTRL: g_linux_kbd_ctrl_r = down; break;
    case 56: g_linux_kbd_alt_l = down; break;
    case LINUX_KEY_RIGHTALT: g_linux_kbd_alt_r = down; break;
    case LINUX_KEY_LEFTMETA: g_linux_kbd_meta_l = down; break;
    case LINUX_KEY_RIGHTMETA: g_linux_kbd_meta_r = down; break;
    case LINUX_KEY_CAPSLOCK:
        if (down) g_linux_kbd_caps = !g_linux_kbd_caps;
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Pumps: HAL rings -> evdev rings                                    */
/* ------------------------------------------------------------------ */

static void linux_pump_keyboard(void)
{
    UINT32 event;
    while (KbdReadMirroredInputEvent(&event)) {
        /* The HAL event packs: low 7 bits scancode, bit8 = release,
         * bit9 = extended (0xE0-prefixed). Character events (bit16) are not
         * relevant to evdev. */
        if (event & KBD_INPUT_CHARACTER)
            continue;
        BOOLEAN extended = (event & 0x200u) != 0;
        BOOLEAN release = (event & 0x100u) != 0;
        UINT32 code = event & 0x7Fu;
        UINT16 key = linux_key_from_scancode(code, extended);
        if (!key)
            continue;
        INT32 value = release ? 0 : 1;
        linux_event_emit(0, LINUX_EV_MSC, LINUX_MSC_SCAN, (INT32)event);
        linux_event_emit(0, LINUX_EV_KEY, key, value);
        linux_key_state_track(key, !release);
        linux_event_sync(0);
    }
}

static void linux_pump_mouse(void)
{
    INT32 dx, dy;
    UINT8 buttons;
    while (MouseReadRelEvent(&dx, &dy, &buttons)) {
        if (dx || dy) {
            linux_event_emit(1, LINUX_EV_REL, LINUX_REL_X, dx);
            /* PS/2 Y is positive-up; evdev REL_Y is positive-down. */
            linux_event_emit(1, LINUX_EV_REL, LINUX_REL_Y, -dy);
            linux_event_emit(2, LINUX_EV_REL, LINUX_REL_X, dx);
            linux_event_emit(2, LINUX_EV_REL, LINUX_REL_Y, -dy);
        }
        UINT8 changed = buttons ^ g_linux_mouse_buttons;
        if (changed & 0x01) {
            INT32 value = (buttons & 0x01) ? 1 : 0;
            linux_event_emit(1, LINUX_EV_KEY, LINUX_BTN_LEFT, value);
            linux_event_emit(2, LINUX_EV_KEY, LINUX_BTN_LEFT, value);
        }
        if (changed & 0x02) {
            INT32 value = (buttons & 0x02) ? 1 : 0;
            linux_event_emit(1, LINUX_EV_KEY, LINUX_BTN_RIGHT, value);
            linux_event_emit(2, LINUX_EV_KEY, LINUX_BTN_RIGHT, value);
        }
        if (changed & 0x04) {
            INT32 value = (buttons & 0x04) ? 1 : 0;
            linux_event_emit(1, LINUX_EV_KEY, LINUX_BTN_MIDDLE, value);
            linux_event_emit(2, LINUX_EV_KEY, LINUX_BTN_MIDDLE, value);
        }
        g_linux_mouse_buttons = buttons;
        linux_event_sync(1);
    }
}

/* Drain every HAL source, then report whether any device has a ready record.
 * Called from read/epoll, so it reflects the state at the moment a caller
 * checks for input. */
void LinuxInputPump(void)
{
    linux_pump_keyboard();
    linux_pump_mouse();
}

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */

static void linux_event_device_common(LINUX_EVENT_DEVICE *device,
                                      const char *name)
{
    SIZE_T n = strlen(name);
    if (n >= LINUX_EVENT_NAME_LENGTH) n = LINUX_EVENT_NAME_LENGTH - 1;
    memcpy(device->Name, name, n);
    device->Name[n] = 0;
    device->Bus = 0x11; /* BUS_I8042: a PS/2 controller */
    device->Vendor = 0x0001;
    device->Product = 0x0001;
    device->Version = 0x0100;
}

void LinuxInputInitialize(void)
{
    if (g_linux_input_ready)
        return;
    memset(g_linux_event_devices, 0, sizeof(g_linux_event_devices));
    memset(g_linux_event_ring, 0, sizeof(g_linux_event_ring));
    g_linux_mouse_buttons = 0;

    /* event0: keyboard. */
    LINUX_EVENT_DEVICE *kbd = &g_linux_event_devices[0];
    kbd->Present = TRUE;
    linux_event_device_common(kbd, "AT Translated Set 2 keyboard");
    kbd->Capabilities[LINUX_BIT_WORD(LINUX_EV_SYN)] |= LINUX_BIT(LINUX_EV_SYN);
    kbd->Capabilities[LINUX_BIT_WORD(LINUX_EV_KEY)] |= LINUX_BIT(LINUX_EV_KEY);
    kbd->Capabilities[LINUX_BIT_WORD(LINUX_EV_MSC)] |= LINUX_BIT(LINUX_EV_MSC);
    for (UINT32 scancode = 0; scancode < 128; scancode++) {
        for (UINT32 extended = 0; extended < 2; extended++) {
            UINT16 key = linux_key_from_scancode(scancode, extended);
            if (key && key < sizeof(kbd->KeyBits) * 8)
                kbd->KeyBits[LINUX_BIT_WORD(key)] |= LINUX_BIT(key);
        }
    }

    /* event1: mouse. */
    LINUX_EVENT_DEVICE *mouse = &g_linux_event_devices[1];
    mouse->Present = TRUE;
    linux_event_device_common(mouse, "PS/2 Generic Mouse");
    mouse->Product = 0x0002;
    mouse->Capabilities[LINUX_BIT_WORD(LINUX_EV_SYN)] |= LINUX_BIT(LINUX_EV_SYN);
    mouse->Capabilities[LINUX_BIT_WORD(LINUX_EV_KEY)] |= LINUX_BIT(LINUX_EV_KEY);
    mouse->Capabilities[LINUX_BIT_WORD(LINUX_EV_REL)] |= LINUX_BIT(LINUX_EV_REL);
    mouse->Capabilities[LINUX_BIT_WORD(LINUX_EV_MSC)] |= LINUX_BIT(LINUX_EV_MSC);
    mouse->RelBits[LINUX_BIT_WORD(LINUX_REL_X)] |= LINUX_BIT(LINUX_REL_X);
    mouse->RelBits[LINUX_BIT_WORD(LINUX_REL_Y)] |= LINUX_BIT(LINUX_REL_Y);
    mouse->RelBits[LINUX_BIT_WORD(LINUX_REL_WHEEL)] |= LINUX_BIT(LINUX_REL_WHEEL);
    mouse->KeyBits[LINUX_BIT_WORD(LINUX_BTN_LEFT)] |= LINUX_BIT(LINUX_BTN_LEFT);
    mouse->KeyBits[LINUX_BIT_WORD(LINUX_BTN_RIGHT)] |= LINUX_BIT(LINUX_BTN_RIGHT);
    mouse->KeyBits[LINUX_BIT_WORD(LINUX_BTN_MIDDLE)] |= LINUX_BIT(LINUX_BTN_MIDDLE);

    /* event2: combined pointer+keyboard node. */
    LINUX_EVENT_DEVICE *both = &g_linux_event_devices[2];
    both->Present = TRUE;
    linux_event_device_common(both, "svindows combined input");
    both->Product = 0x0003;
    both->Capabilities[LINUX_BIT_WORD(LINUX_EV_SYN)] |= LINUX_BIT(LINUX_EV_SYN);
    both->Capabilities[LINUX_BIT_WORD(LINUX_EV_KEY)] |= LINUX_BIT(LINUX_EV_KEY);
    both->Capabilities[LINUX_BIT_WORD(LINUX_EV_REL)] |= LINUX_BIT(LINUX_EV_REL);
    both->Capabilities[LINUX_BIT_WORD(LINUX_EV_MSC)] |= LINUX_BIT(LINUX_EV_MSC);
    for (UINT32 i = 0; i < 2; i++)
        both->RelBits[i] = mouse->RelBits[i];
    for (UINT32 i = 0; i < 12; i++)
        both->KeyBits[i] = kbd->KeyBits[i] | mouse->KeyBits[i];

    g_linux_input_ready = TRUE;
}

/* ------------------------------------------------------------------ */
/* Path / open / read / ioctl / poll                                  */
/* ------------------------------------------------------------------ */

/* Match C:\dev\input\eventN. Returns the device index in *index_out. */
BOOLEAN LinuxEventPath(const char *dos_path, UINT32 *index_out)
{
    static const char prefix[] = "C:\\dev\\input\\event";
    UINT32 i = 0;
    for (; prefix[i]; i++) {
        char a = dos_path[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return FALSE;
    }
    char digit = dos_path[i];
    if (digit < '0' || digit >= (char)('0' + LINUX_EVENT_DEVICE_COUNT))
        return FALSE;
    if (dos_path[i + 1]) return FALSE;
    if (!g_linux_input_ready) return FALSE;
    *index_out = (UINT32)(digit - '0');
    return g_linux_event_devices[*index_out].Present;
}

UINT64 LinuxEventOpen(PKPROCESS process, UINT64 slot, UINT32 index,
                      UINT64 flags)
{
    if (index >= LINUX_EVENT_DEVICE_COUNT ||
        !g_linux_event_devices[index].Present)
        return LinuxError(LINUX_ENODEV);
    if (flags & (0x40u | 0x80u | 0x200u)) /* O_CREAT | O_EXCL | O_TRUNC */
        return LinuxError(LINUX_EINVAL);
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[slot];
    memset(entry, 0, sizeof(*entry));
    entry->Used = TRUE;
    entry->Mode = (UINT8)(flags & 3u);
    entry->CloseOnExec = !!(flags & 0x80000u);
    entry->NonBlocking = !!(flags & 0x800u); /* O_NONBLOCK */
    entry->Kind = LINUX_FD_EVENT;
    entry->EventDevice = (UINT8)index;
    LinuxEventSetOpen(index, TRUE);
    return slot;
}

UINT64 LinuxEventRead(PKPROCESS process, UINT64 fd, UINT64 buffer,
                      UINT64 length)
{
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    if (entry->Mode == 1) return LinuxError(LINUX_EBADF); /* opened O_WRONLY */
    if (!length) return 0;
    if (!MmProbeForWrite(buffer, length)) return LinuxError(LINUX_EFAULT);

    LinuxInputPump();

    UINT32 index = entry->EventDevice;
    if (index >= LINUX_EVENT_DEVICE_COUNT) return LinuxError(LINUX_ENODEV);
    LINUX_EVENT_RING *ring = &g_linux_event_ring[index];

    if (ring->Head == ring->Tail) {
        if (entry->NonBlocking) return LinuxError(LINUX_EAGAIN);
        /* Blocking read with no data: report EAGAIN rather than deadlock.
         * A caller that wants to wait should poll the descriptor first; Xorg
         * always registers input via epoll before its read loop. */
        return LinuxError(LINUX_EAGAIN);
    }

    UINT64 copied = 0;
    while (copied + sizeof(LINUX_INPUT_EVENT) <= length &&
           ring->Head != ring->Tail) {
        LINUX_INPUT_EVENT record = ring->Records[ring->Tail];
        ring->Tail = (ring->Tail + 1) % LINUX_EVENT_RING_SIZE;
        memcpy((void *)(ULONG_PTR)(buffer + copied), &record, sizeof(record));
        copied += sizeof(record);
    }
    return copied;
}

UINT64 LinuxEventIoctl(PKPROCESS process, UINT64 fd, UINT64 command,
                       UINT64 argument)
{
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    if (entry->Kind != LINUX_FD_EVENT) return LinuxError(LINUX_ENOTTY);
    UINT32 index = entry->EventDevice;
    if (index >= LINUX_EVENT_DEVICE_COUNT ||
        !g_linux_event_devices[index].Present)
        return LinuxError(LINUX_ENODEV);
    const LINUX_EVENT_DEVICE *device = &g_linux_event_devices[index];

    if ((UINT32)command == LINUX_EVIOCGVERSION) {
        if (!MmProbeForWrite(argument, sizeof(UINT32)))
            return LinuxError(LINUX_EFAULT);
        *(UINT32 *)(ULONG_PTR)argument = 0x010001; /* EV_VERSION 1.0.1 */
        return 0;
    }
    if ((UINT32)command == LINUX_EVIOCGID) {
        /* struct input_id { u16 bustype, vendor, product, version; } */
        if (!MmProbeForWrite(argument, 8))
            return LinuxError(LINUX_EFAULT);
        UINT16 id[4] = {device->Bus, device->Vendor, device->Product,
                        device->Version};
        memcpy((void *)(ULONG_PTR)argument, id, sizeof(id));
        return 0;
    }

    UINT32 request = (UINT32)command;
    UINT32 type = (request >> 8) & 0xffu;
    UINT32 op = request & 0xffu;
    UINT32 size = (request >> 16) & 0x3fffu;
    if (type != 0x45u) return LinuxError(LINUX_ENOTTY);

    if (op == 0x09u || op == 0x19u || op == 0x1au || op == 0x1bu) {
        /* EVIOCGPROP/LED/SND/SW: none of these properties or states are
         * provided by the PS/2 adapter. libevdev queries them even when
         * the corresponding event type is absent. */
        if (!MmProbeForWrite(argument, size))
            return LinuxError(LINUX_EFAULT);
        memset((void *)(ULONG_PTR)argument, 0, size);
        return size;
    }

    if (op == 0x06u || op == 0x07u || op == 0x08u) {
        /* EVIOCGNAME / EVIOCGPHYS / EVIOCGUNIQ: return a NUL-terminated name.
         * EVIOCGPHYS and EVIOCGUNIQ both resolve to an empty string, matching
         * the kernel behaviour when no phys/uniq string is present. */
        const char *source = op == 0x06u ? device->Name : "";
        SIZE_T len = strlen(source) + 1;
        if (len > size) len = size;
        if (len && !MmProbeForWrite(argument, len))
            return LinuxError(LINUX_EFAULT);
        if (len) memcpy((void *)(ULONG_PTR)argument, source, len);
        return (UINT64)len;
    }

    if (op >= 0x20u && op <= 0x3fu) {
        /* EVIOCGBIT(ev, len): copy the capability bitmask for event type ev
         * (or the EV_* type set when ev == 0). */
        if (!MmProbeForWrite(argument, size))
            return LinuxError(LINUX_EFAULT);
        UINT8 scratch[128];
        if (size > sizeof(scratch)) size = sizeof(scratch);
        memset(scratch, 0, sizeof(scratch));
        UINT32 ev = op - 0x20u;
        if (ev == 0) {
            memcpy(scratch, device->Capabilities,
                   sizeof(device->Capabilities) < size ?
                   sizeof(device->Capabilities) : size);
        } else if (ev == LINUX_EV_KEY) {
            memcpy(scratch, device->KeyBits,
                   sizeof(device->KeyBits) < size ?
                   sizeof(device->KeyBits) : size);
        } else if (ev == LINUX_EV_REL) {
            memcpy(scratch, device->RelBits,
                   sizeof(device->RelBits) < size ?
                   sizeof(device->RelBits) : size);
        }
        memcpy((void *)(ULONG_PTR)argument, scratch, size);
        return size;
    }

    if (op == 0x18u) {
        /* EVIOCGKEY(len): current pressed-key bitmap, also used as the evdev
         * "sync request"; clear on write support is not modelled, so report
         * the tracked modifier state. */
        if (!MmProbeForWrite(argument, size))
            return LinuxError(LINUX_EFAULT);
        UINT8 scratch[96];
        if (size > sizeof(scratch)) size = sizeof(scratch);
        memset(scratch, 0, sizeof(scratch));
        if (g_linux_kbd_shift_l) scratch[LINUX_KEY_LEFTSHIFT / 8] |= 1u << (LINUX_KEY_LEFTSHIFT % 8);
        if (g_linux_kbd_shift_r) scratch[LINUX_KEY_RIGHTSHIFT / 8] |= 1u << (LINUX_KEY_RIGHTSHIFT % 8);
        if (g_linux_kbd_ctrl_l) scratch[LINUX_KEY_LEFTCTRL / 8] |= 1u << (LINUX_KEY_LEFTCTRL % 8);
        if (g_linux_kbd_ctrl_r) scratch[LINUX_KEY_RIGHTCTRL / 8] |= 1u << (LINUX_KEY_RIGHTCTRL % 8);
        memcpy((void *)(ULONG_PTR)argument, scratch, size);
        return size;
    }

    /* Mode-changing ioctls (EVIOCS*, EVIOCGRAB, ...) are accepted as no-ops so
     * a probing client does not abort; unknown requests report ENOTTY. */
    if (op >= 0x40u) return 0;
    return LinuxError(LINUX_ENOTTY);
}

/* Readiness for epoll: 1 == readable, 0x10 == hangup. Evdev nodes are
 * read-only and must not advertise POLLOUT. */
UINT32 LinuxEventPoll(PKPROCESS process, UINT64 fd)
{
    (void)process;
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    if (entry->Kind != LINUX_FD_EVENT) return 0;
    LinuxInputPump();
    UINT32 index = entry->EventDevice;
    if (index >= LINUX_EVENT_DEVICE_COUNT) return 0x10;
    LINUX_EVENT_RING *ring = &g_linux_event_ring[index];
    return ring->Head != ring->Tail ? 1u : 0u;
}

const LINUX_EVENT_DEVICE *LinuxEventDevice(UINT32 index)
{
    if (index >= LINUX_EVENT_DEVICE_COUNT) return NULL;
    return g_linux_event_devices[index].Present ?
           &g_linux_event_devices[index] : NULL;
}

/* Track whether an event device currently has an opener. Close clears it;
 * fork marks the child's inherited descriptor as still open. */
void LinuxEventSetOpen(UINT32 index, BOOLEAN open)
{
    if (index >= LINUX_EVENT_DEVICE_COUNT) return;
    BOOLEAN changed = FALSE;
    if (open) {
        __sync_add_and_fetch(&g_linux_event_devices[index].OpenReferences, 1);
        changed = TRUE;
    } else {
        UINT32 references;
        do {
            references = __sync_fetch_and_add(
                &g_linux_event_devices[index].OpenReferences, 0);
            if (!references) break;
        } while (!__sync_bool_compare_and_swap(
            &g_linux_event_devices[index].OpenReferences, references,
            references - 1));
        changed = references != 0;
    }
    g_linux_event_devices[index].Open =
        __sync_fetch_and_add(&g_linux_event_devices[index].OpenReferences,
                             0) != 0;
    if (index == 0 || index == 2) {
        if (changed && open) KbdInputMirrorAcquire();
        else if (changed) KbdInputMirrorRelease();
    }
}

BOOLEAN LinuxInputKeyboardActive(void)
{
    return __sync_fetch_and_add(&g_linux_event_devices[0].OpenReferences, 0) ||
           __sync_fetch_and_add(&g_linux_event_devices[2].OpenReferences, 0);
}
