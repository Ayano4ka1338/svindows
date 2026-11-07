/* X11 presentation for the DOS VGA and the Win16 HWND agent.
 * Wire requests are little endian; the guest Xorg has access control disabled
 * by startx. No user descriptors or address probes are bypassed. */
#include "../linux/SocketPrivate.h"
#include "Internal.h"
#undef r
#undef flags
#undef seg
#undef ip
#include <svindows/Linux.h>

#define X_WINDOWS 64
#define X_KEYS 128
#define X_EVENTS 64

typedef struct __attribute__((packed)) {
    U16 Op, Hwnd;
    INT16 X, Y;
    U16 Width, Height;
    char Title[64];
} X_PACKET;
_Static_assert(sizeof(X_PACKET) == DOS_X_PACKET_SIZE, "Win16 packet layout");

typedef struct {
    U32 Id;
    X_PACKET Guest;
    BOOLEAN Seen, SurfaceReady;
    U32 *Surface;
    U32 SurfaceBytes, NextRow;
    U64 RowHash[2048];
    U8 RowValid[2048];
} X_WINDOW;

typedef struct {
    LINUX_SOCKET *Socket;
    DOS_VM *ActiveVm;
    U32 References, PreviousWindow, WantedTop;
    U32 Base, Mask, NextId, Root, Visual, Gc, Cursor, DeleteAtom, ProtocolAtom;
    U8 Depth;
    BOOLEAN Agent, Failed, Capturing, ActivationAck;
    U32 ActivationsPending, SurfaceBytes;
    U16 CaptureHwnd, ClipCount;
    X_PACKET Clips[X_WINDOWS];
    U8 Event[32];
    U32 EventBytes;
    U32 Keys[X_KEYS], KeyHead, KeyTail;
    U8 Held[128];
    X_PACKET Events[X_EVENTS];
    U32 EventHead, EventTail;
    MOUSE_STATE Mouse, VisibleMouse;
    MOUSE_STATE MouseQueue[X_EVENTS];
    U32 MouseHead, MouseTail;
    X_WINDOW Windows[X_WINDOWS];
    U32 Row[2048];
    U8 PendingImage[24 + 2048 * 4];
    U32 PendingBytes, PendingOffset, NextWindow;
} DOS_X11;

static U16 get16(const U8 *p) { return (U16)(p[0] | (U16)p[1] << 8); }
static U32 get32(const U8 *p) { return get16(p) | (U32)get16(p + 2) << 16; }
static void put16(U8 *p, U16 v) {
    p[0] = v;
    p[1] = v >> 8;
}
static void put32(U8 *p, U32 v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

/* Keep at most one complete scanline request privately. A slow X server
 * must not suspend guest instruction execution for an entire frame. Preserve
 * partially sent requests verbatim, and finish them before any control request. */
static BOOLEAN flush_image(DOS_X11 *x) {
    while (x->PendingOffset < x->PendingBytes && !x->Failed) {
        INT64 n = (INT64)LinuxLocalTransfer(x->Socket, TRUE,
            x->PendingImage + x->PendingOffset, x->PendingBytes - x->PendingOffset, 0x40);
        if (n == -11) return FALSE;
        if (n <= 0) { x->Failed = TRUE; return FALSE; }
        x->PendingOffset += (U32)n;
    }
    x->PendingBytes = x->PendingOffset = 0;
    return !x->Failed;
}

static BOOLEAN x_transfer(DOS_X11 *x, BOOLEAN write, void *data, U32 size) {
    U8 *p = data;
    while (write && !flush_image(x) && !x->Failed) {
        DosXPoll(x->ActiveVm);
        KeYield();
    }
    while (size && !x->Failed) {
        INT64 n = (INT64)LinuxLocalTransfer(x->Socket, write, p, size, write ? 0x40 : 0);
        if (write && n == -11) {
            /* Drain X events while the server drains our image requests. Both
             * directions have bounded queues; blocking on send can deadlock. */
            DosXPoll(x->ActiveVm);
            KeYield();
            continue;
        }
        if (n <= 0) {
            x->Failed = TRUE;
            return FALSE;
        }
        p += n;
        size -= (U32)n;
    }
    return !x->Failed;
}
static BOOLEAN send(DOS_X11 *x, void *data, U32 size) { return x_transfer(x, TRUE, data, size); }
static U32 resource(DOS_X11 *x) { return x->Base | (++x->NextId & x->Mask); }
static void request(DOS_X11 *x, U8 op, U32 id) {
    U8 r[8] = {0};
    r[0] = op;
    put16(r + 2, 2);
    put32(r + 4, id);
    send(x, r, 8);
}
static U32 atom(DOS_X11 *x, const char *name, U16 len) {
    U8 r[64] = {0}, reply[32];
    r[0] = 16;
    put16(r + 2, (8 + len + 3) / 4);
    put16(r + 4, len);
    memcpy(r + 8, name, len);
    if (!send(x, r, ((8 + len + 3) / 4) * 4) || !x_transfer(x, FALSE, reply, 32) || reply[0] != 1)
        return 0;
    return get32(reply + 8);
}
static void property(DOS_X11 *x, U32 win, U32 prop, U32 type, U8 format, const void *data,
                     U32 count) {
    U8 r[128] = {0};
    U32 bytes = count * (format / 8);
    if (bytes > sizeof(r) - 24)
        return;
    r[0] = 18;
    put16(r + 2, (24 + bytes + 3) / 4);
    put32(r + 4, win);
    put32(r + 8, prop);
    put32(r + 12, type);
    r[16] = format;
    put32(r + 20, count);
    memcpy(r + 24, data, bytes);
    send(x, r, ((24 + bytes + 3) / 4) * 4);
}
/* X positions belong to the host WM; guest positions select the VGA crop. */
static void configure(DOS_X11 *x, X_WINDOW *w) {
    U8 r[20] = {12, 0, 5, 0};
    put32(r + 4, w->Id);
    put16(r + 8, 12);
    put32(r + 12, w->Guest.Width);
    put32(r + 16, w->Guest.Height);
    send(x, r, sizeof(r));
}
static void release_surface(DOS_X11 *x, X_WINDOW *w) {
    if (w->Surface) {
        ExFreePool(w->Surface);
        x->SurfaceBytes -= w->SurfaceBytes;
    }
    w->Surface = NULL;
    w->SurfaceBytes = 0;
}
static BOOLEAN resize_surface(DOS_X11 *x, X_WINDOW *w, U16 width, U16 height) {
    if (w->Surface && w->Guest.Width == width && w->Guest.Height == height)
        return TRUE;
    U32 bytes = (U32)width * height * 4;
    if (bytes > 64u * 1024u * 1024u - (x->SurfaceBytes - w->SurfaceBytes))
        return FALSE;
    U32 *pixels = ExAllocatePoolWithTag(NonPagedPool, bytes, 'sfXD');
    if (!pixels)
        return FALSE;
    memset(pixels, 0, bytes);
    if (w->Surface) {
        U32 rows = height < w->Guest.Height ? height : w->Guest.Height;
        U32 cols = width < w->Guest.Width ? width : w->Guest.Width;
        for (U32 y = 0; y < rows; y++)
            memcpy(pixels + y * width, w->Surface + y * w->Guest.Width, cols * 4);
    }
    release_surface(x, w);
    w->Surface = pixels;
    w->SurfaceBytes = bytes;
    x->SurfaceBytes += bytes;
    memset(w->RowValid, 0, sizeof(w->RowValid));
    w->SurfaceReady = FALSE;
    return TRUE;
}
static void create(DOS_X11 *x, X_WINDOW *w) {
    w->Id = resource(x);
    U8 r[44] = {1, 0, 11, 0};
    r[1] = x->Depth;
    put32(r + 4, w->Id);
    put32(r + 8, x->Root);
    put16(r + 12, w->Guest.X);
    put16(r + 14, w->Guest.Y);
    put16(r + 16, w->Guest.Width);
    put16(r + 18, w->Guest.Height);
    put16(r + 22, 1);
    put32(r + 24, x->Visual);
    put32(r + 28, (1u << 1) | (1u << 11) | (1u << 14)); /* background, events, cursor */
    put32(r + 36, 1 | 2 | 4 | 8 | 64 | 32768 | 131072 | 2097152);
    put32(r + 40, x->Cursor);
    send(x, r, sizeof(r));
    property(x, w->Id, x->ProtocolAtom, 4, 32, &x->DeleteAtom, 1);
    static const char klass[] = "svdos\0Svindows\0";
    property(x, w->Id, 67, 31, 8, klass, sizeof(klass) - 1);
    property(x, w->Id, 39, 31, 8, w->Guest.Title, 64);
    request(x, 8, w->Id);
}
static void queue_event(DOS_X11 *x, U16 op, X_WINDOW *w, INT16 a, INT16 b, U16 c, U16 d) {
    U32 next = (x->EventHead + 1) % X_EVENTS;
    if (next == x->EventTail)
        return;
    X_PACKET *p = &x->Events[x->EventHead];
    memset(p, 0, sizeof(*p));
    p->Op = op;
    p->Hwnd = w->Guest.Hwnd;
    p->X = a;
    p->Y = b;
    p->Width = c;
    p->Height = d;
    x->EventHead = next;
    if (op == 1)
        x->ActivationsPending++;
}
static void key(DOS_X11 *x, U8 scan, BOOLEAN up) {
    U32 next = (x->KeyHead + 1) % X_KEYS;
    if (next == x->KeyTail)
        return;
    x->Keys[x->KeyHead] = scan | (up ? 0x100u : 0);
    x->KeyHead = next;
}
static void queue_mouse(DOS_X11 *x) {
    U32 last = (x->MouseHead + X_EVENTS - 1) % X_EVENTS;
    /* Coalesce motion, but retain every make/break transition across the
     * asynchronous guest activation. A fast click must not become just up. */
    if (x->MouseHead != x->MouseTail && x->MouseQueue[last].Buttons == x->Mouse.Buttons) {
        x->MouseQueue[last] = x->Mouse;
        return;
    }
    U32 next = (x->MouseHead + 1) % X_EVENTS;
    if (next == x->MouseTail)
        return;
    x->MouseQueue[x->MouseHead] = x->Mouse;
    x->MouseHead = next;
}
void DosXUpdateInput(DOS_VM *vm) {
    DOS_X11 *x = vm->XDisplay;
    if (!x || x->Capturing || x->ActivationsPending || x->MouseHead == x->MouseTail)
        return;
    x->VisibleMouse = x->MouseQueue[x->MouseTail];
    x->MouseTail = (x->MouseTail + 1) % X_EVENTS;
}

void DosXPoll(DOS_VM *vm) {
    DOS_X11 *x = vm->XDisplay;
    if (!x)
        return;
    if (x->Failed) {
        vm->Running = FALSE;
        return;
    }
    flush_image(x);
    /* Bound work even if an X client floods the event stream. */
    for (U32 i = 0; i < 128; i++) {
        INT64 n = (INT64)LinuxLocalTransfer(x->Socket, FALSE, x->Event + x->EventBytes,
                                            32 - x->EventBytes, 0x40);
        if (n == -11)
            break;
        if (n <= 0) {
            x->Failed = TRUE;
            vm->Running = FALSE;
            break;
        }
        x->EventBytes += (U32)n;
        if (x->EventBytes != 32)
            continue;
        x->EventBytes = 0;
        U8 *e = x->Event, type = e[0] & 127;
        if (type == 0) {
            KeLog("[dos-x11] X error=%u opcode=%u resource=%08x\n", e[1], e[10], get32(e + 4));
            x->Failed = TRUE;
            vm->Running = FALSE;
            break;
        }
        /* FocusOut can arrive after the client record was retired. Release
         * virtual modifiers/buttons even when that old X ID no longer exists. */
        if (type == 10) {
            x->KeyTail = x->KeyHead;
            for (U32 j = 1; j < 128; j++)
                if (x->Held[j]) {
                    key(x, j, TRUE);
                    x->Held[j] = 0;
                }
            x->Mouse.Buttons = 0;
            x->Mouse.Seq++;
            x->MouseTail = x->MouseHead;
            queue_mouse(x);
            continue;
        }
        U32 id = get32(e + ((type >= 2 && type <= 8) ? 12 : type == 22 ? 8 : 4));
        X_WINDOW *w = NULL;
        for (U32 j = 0; j < X_WINDOWS; j++)
            if (x->Windows[j].Id == id) {
                w = &x->Windows[j];
                break;
            }
        if (!w)
            continue;
        if (type == 2 || type == 3) {
            /* The bundled Xorg uses evdev's standard keycode = scan + 8.
             * Extended evdev keys need the PS/2 E0 prefix. */
            U8 code = e[1], scan = code >= 8 ? code - 8 : 0;
            static const U8 extended[28] = {0x1c, 0x1d, 0x35, 0x37, 0x38, 0,    0x47, 0x48,
                                            0x49, 0x4b, 0x4d, 0x4f, 0x50, 0x51, 0x52, 0x53};
            if (code >= 104 && code < 132)
                scan = extended[code - 104];
            if (!scan || scan >= 128)
                continue;
            key(x, scan, type == 3);
            x->Held[scan] = type == 2;
        } else if (type == 4 || type == 5 || type == 6) {
            x->Mouse.X = w->Guest.X + (INT16)get16(e + 24);
            x->Mouse.Y = w->Guest.Y + (INT16)get16(e + 26);
            if (type != 6) {
                U8 mask = e[1] == 1 ? 1 : e[1] == 2 ? 4 : e[1] == 3 ? 2 : 0;
                if (type == 4)
                    x->Mouse.Buttons |= mask;
                else
                    x->Mouse.Buttons &= ~mask;
            }
            x->Mouse.Seq++;
            queue_mouse(x);
            if (type == 4 && x->Agent)
                queue_event(x, 1, w, 0, 0, 0, 0);
        } else if (type == 9 && x->Agent)
            queue_event(x, 1, w, 0, 0, 0, 0);
        else if (type == 22 && x->Agent) {
            U16 width = get16(e + 20), height = get16(e + 22);
            if (width != w->Guest.Width || height != w->Guest.Height)
                queue_event(x, 3, w, w->Guest.X, w->Guest.Y, width, height);
        } else if (type == 33 && get32(e + 8) == x->ProtocolAtom &&
                   get32(e + 12) == x->DeleteAtom) {
            if (x->Agent) {
                KeLog("[dos-x11] close HWND=%04x\n", w->Guest.Hwnd);
                queue_event(x, 2, w, 0, 0, 0, 0);
            } else
                vm->Running = FALSE;
        } else if (type == 12) {
            memset(w->RowValid, 0, sizeof(w->RowValid));
            vm->VideoDirty = TRUE;
        }
    }
}

void DosXOpen(DOS_VM *vm) {
    if (vm->XDisplay || !LinuxInputKeyboardActive())
        return;
    /* Never fall back to the physical framebuffer while X owns input. */
    vm->XSession = TRUE;
    DOS_X11 *x = ExAllocatePoolWithTag(NonPagedPool, sizeof(*x), '11XD');
    if (!x) {
        vm->Running = FALSE;
        return;
    }
    memset(x, 0, sizeof(*x));
    x->References = 1;
    x->ActiveVm = vm;
    vm->XDisplay = x;
    static const char abstract[] = "\0/tmp/.X11-unix/X0";
    x->Socket = LinuxLocalConnect(abstract, sizeof(abstract) - 1);
    if (!x->Socket) {
        x->Failed = TRUE;
        vm->Running = FALSE;
        return;
    }
    U8 hello[12] = {'l', 0, 11}, prefix[8];
    U8 *setup = ExAllocatePoolWithTag(NonPagedPool, 65536, 'suXD');
    if (!setup) {
        x->Failed = TRUE;
        vm->Running = FALSE;
        return;
    }
    if (!send(x, hello, 12) || !x_transfer(x, FALSE, prefix, 8) || prefix[0] != 1)
        goto fail;
    U32 size = (U32)get16(prefix + 6) * 4;
    if (size < 40 || size > 65536 || !x_transfer(x, FALSE, setup, size))
        goto fail;
    x->Base = get32(setup + 4);
    x->Mask = get32(setup + 8);
    if (!setup[20] || setup[22] != 0)
        goto fail; /* screens, LSB image order */
    U32 root = 32 + ((get16(setup + 16) + 3) & ~3u) + (U32)setup[21] * 8;
    if (root + 40 > size)
        goto fail;
    x->Root = get32(setup + root);
    x->Visual = get32(setup + root + 32);
    x->Depth = setup[root + 38];
    BOOLEAN format = FALSE;
    for (U32 i = 0; i < setup[21]; i++) {
        U32 at = 32 + ((get16(setup + 16) + 3) & ~3u) + i * 8;
        if (setup[at] == x->Depth && setup[at + 1] == 32 && setup[at + 2] == 32)
            format = TRUE;
    }
    if (!format || x->Depth != 24)
        goto fail;
    /* Validate the actual visual instead of assuming an arbitrary X server's masks. */
    BOOLEAN visual = FALSE;
    U32 at = root + 40;
    for (U32 d = 0; d < setup[root + 39]; d++) {
        if (at + 8 > size)
            goto fail;
        U32 count = get16(setup + at + 2);
        at += 8;
        for (U32 v = 0; v < count; v++, at += 24) {
            if (at + 24 > size)
                goto fail;
            if (get32(setup + at) == x->Visual && get32(setup + at + 8) == 0xff0000 &&
                get32(setup + at + 12) == 0xff00 && get32(setup + at + 16) == 0xff)
                visual = TRUE;
        }
    }
    if (!visual)
        goto fail;
    x->ProtocolAtom = atom(x, "WM_PROTOCOLS", 12);
    x->DeleteAtom = atom(x, "WM_DELETE_WINDOW", 16);
    if (!x->ProtocolAtom || !x->DeleteAtom)
        goto fail;
    U32 font = resource(x);
    U8 open_font[20] = {45, 0, 5, 0};
    put32(open_font + 4, font);
    put16(open_font + 8, 6);
    memcpy(open_font + 12, "cursor", 6);
    send(x, open_font, sizeof(open_font));
    x->Cursor = resource(x);
    U8 cursor[32] = {94, 0, 8, 0};
    put32(cursor + 4, x->Cursor);
    put32(cursor + 8, font);
    put32(cursor + 12, font);
    put16(cursor + 16, 68);
    put16(cursor + 18, 69); /* XC_left_ptr and mask */
    put16(cursor + 26, 0xffff);
    put16(cursor + 28, 0xffff);
    put16(cursor + 30, 0xffff);
    send(x, cursor, sizeof(cursor));
    x->Gc = resource(x);
    U8 gc[16] = {55, 0, 4, 0};
    put32(gc + 4, x->Gc);
    put32(gc + 8, x->Root);
    send(x, gc, 16);
    ExFreePool(setup);
    KeLog("[dos-x11] connected to :0\n");
    return;
fail:
    ExFreePool(setup);
    x->Failed = TRUE;
    vm->Running = FALSE;
    KeLog("[dos-x11] unsupported or unavailable X11 display\n");
}
void DosXClose(DOS_VM *vm) {
    DOS_X11 *x = vm->XDisplay;
    if (!x)
        return;
    vm->XDisplay = NULL;
    if (--x->References)
        return;
    for (U32 i = 0; i < X_WINDOWS; i++)
        release_surface(x, &x->Windows[i]);
    if (x->Socket)
        LinuxSocketClose(x->Socket);
    ExFreePool(x);
    vm->XDisplay = NULL;
}
BOOLEAN DosReadKey(DOS_VM *vm, UINT32 *event) {
    if (!vm->XSession)
        return KbdReadMirroredInputEvent(event);
    DOS_X11 *x = vm->XDisplay;
    if (!x || x->Capturing || x->ActivationsPending || x->KeyHead == x->KeyTail)
        return FALSE;
    *event = x->Keys[x->KeyTail];
    x->KeyTail = (x->KeyTail + 1) % X_KEYS;
    return TRUE;
}
BOOLEAN DosKeyAvailable(DOS_VM *vm) {
    if (!vm->XSession)
        return KbdMirroredInputEventAvailable();
    DOS_X11 *x = vm->XDisplay;
    return x && !x->Capturing && !x->ActivationsPending && x->KeyHead != x->KeyTail;
}
const MOUSE_STATE *DosHostMouse(DOS_VM *vm) {
    DOS_X11 *x = vm->XDisplay;
    return x ? &x->VisibleMouse : &MouseState;
}

/* Called only after Cpu.c validates the whole ES:DI packet for read/write. */
void DosXAgent(DOS_VM *vm, void *data) {
    X_PACKET *p = data;
    DOS_X11 *x = vm->XDisplay;
    if (!x || x->Failed) {
        p->Op = 0;
        return;
    }
    x->ActiveVm = vm;
    if (x->ActivationAck) {
        if (x->ActivationsPending)
            x->ActivationsPending--;
        x->ActivationAck = FALSE;
    }
    DosXPoll(vm);
    switch (p->Op) {
    case 0:
        KeLog("[dos-x11] Win16 agent probe\n");
        p->Op = 0x0100;
        break;
    case 1:
        x->WantedTop = 0;
        if (!x->Agent) {
            for (U32 i = 0; i < X_WINDOWS; i++)
                if (x->Windows[i].Id)
                    request(x, 4, x->Windows[i].Id);
            memset(x->Windows, 0, sizeof(x->Windows));
            x->Agent = TRUE;
            KeLog("[dos-x11] Win16 agent attached\n");
        }
        for (U32 i = 0; i < X_WINDOWS; i++)
            x->Windows[i].Seen = FALSE;
        break;
    case 2: {
        if (!x->Agent || !p->Hwnd || !p->Width || !p->Height || p->Width > 2048 || p->Height > 2048)
            break;
        X_WINDOW *w = NULL, *free = NULL;
        for (U32 i = 0; i < X_WINDOWS; i++) {
            if (x->Windows[i].Id && x->Windows[i].Guest.Hwnd == p->Hwnd) {
                w = &x->Windows[i];
                break;
            }
            if (!x->Windows[i].Id && !free)
                free = &x->Windows[i];
        }
        if (!w)
            w = free;
        if (!w)
            break;
        BOOLEAN changed = memcmp(&w->Guest, p, sizeof(*p)) != 0;
        BOOLEAN size_changed = w->Guest.Width != p->Width || w->Guest.Height != p->Height;
        if (!resize_surface(x, w, p->Width, p->Height)) {
            KeLog("[dos-x11] window surface memory exhausted\n");
            x->Failed = TRUE;
            vm->Running = FALSE;
            break;
        }
        w->Guest = *p;
        w->Guest.Title[63] = 0;
        w->Seen = TRUE;
        if (!w->Id)
            create(x, w);
        else if (changed) {
            if (size_changed)
                configure(x, w);
            property(x, w->Id, 39, 31, 8, w->Guest.Title, 64);
        }
        x->WantedTop = w->Id;
        p->Op = w->SurfaceReady ? 0 : 2;
        break;
    }
    case 3:
        /* TWM reparents clients into different frames: a sibling configure
         * would produce BadMatch. Ask the WM to raise only the guest's top
         * client when the guest activation changes. */
        if (x->WantedTop && x->WantedTop != x->PreviousWindow) {
            U8 r[16] = {12, 0, 4, 0};
            put32(r + 4, x->WantedTop);
            put16(r + 8, 64);
            put32(r + 12, 0);
            send(x, r, sizeof(r));
        }
        x->PreviousWindow = x->WantedTop;
        for (U32 i = 0; i < X_WINDOWS; i++)
            if (x->Windows[i].Id && !x->Windows[i].Seen) {
                request(x, 4, x->Windows[i].Id);
                release_surface(x, &x->Windows[i]);
                memset(&x->Windows[i], 0, sizeof(x->Windows[i]));
            }
        vm->VideoDirty = TRUE;
        break;
    case 4:
        if (x->EventTail == x->EventHead)
            p->Op = 0;
        else {
            *p = x->Events[x->EventTail];
            x->EventTail = (x->EventTail + 1) % X_EVENTS;
            if (p->Op == 1)
                x->ActivationAck = TRUE;
        }
        break;
    case 8: /* Temporarily suspend guest input while raising windows to paint. */
        x->Capturing = p->Hwnd != 0;
        break;
    case 10: /* Start this window's visible-region capture. */
        x->CaptureHwnd = p->Hwnd;
        x->ClipCount = 0;
        break;
    case 9: /* Owned popups can remain above the window being captured. */
        if (p->Hwnd == x->CaptureHwnd && x->ClipCount < X_WINDOWS && p->Width && p->Height)
            x->Clips[x->ClipCount++] = *p;
        break;
    case 7:
        for (U32 i = 0; i < X_WINDOWS; i++) {
            X_WINDOW *w = &x->Windows[i];
            if (!w->Id || w->Guest.Hwnd != p->Hwnd || !w->Surface)
                continue;
            for (U32 y = 0; y < w->Guest.Height; y++)
                for (U32 col = 0; col < w->Guest.Width; col++) {
                    INT32 gx = w->Guest.X + (INT32)col, gy = w->Guest.Y + (INT32)y;
                    BOOLEAN covered = FALSE;
                    if (x->CaptureHwnd == p->Hwnd)
                        for (U32 c = 0; c < x->ClipCount; c++) {
                            X_PACKET *r = &x->Clips[c];
                            if (gx >= r->X && gy >= r->Y && gx < (INT32)r->X + r->Width &&
                                gy < (INT32)r->Y + r->Height) {
                                covered = TRUE;
                                break;
                            }
                        }
                    if (!covered)
                        w->Surface[y * w->Guest.Width + col] = DosVideoRgbPixel(vm, gx, gy);
                }
            w->SurfaceReady = TRUE;
            vm->VideoDirty = TRUE;
            break;
        }
        break;
    }
}
BOOLEAN DosXPresent(DOS_VM *vm) {
    DOS_X11 *x = vm->XDisplay;
    if (!vm->XSession)
        return FALSE;
    if (!x || x->Failed || x->Capturing)
        return TRUE;
    x->ActiveVm = vm;
    if (!flush_image(x)) return TRUE;
    U16 sw, sh;
    DosVideoDimensions(vm, &sw, &sh);
    if (!x->Agent && !x->Windows[0].Id) {
        X_WINDOW *w = &x->Windows[0];
        w->Guest.Width = sw;
        w->Guest.Height = sh;
        memcpy(w->Guest.Title, "DOS", 4);
        create(x, w);
    }
    U32 first_window = x->NextWindow;
    for (U32 wi = 0; wi < X_WINDOWS && !x->Failed; wi++) {
        U32 i = (first_window + wi) % X_WINDOWS;
        X_WINDOW *w = &x->Windows[i];
        if (!w->Id)
            continue;
        if (!x->Agent && (w->Guest.Width != sw || w->Guest.Height != sh)) {
            w->Guest.Width = sw;
            w->Guest.Height = sh;
            configure(x, w);
        }
        U32 first_row = w->NextRow % w->Guest.Height;
        for (U32 row = 0; row < w->Guest.Height && !x->Failed; row++) {
            U32 y = (first_row + row) % w->Guest.Height;
            U8 r[24] = {72, 2, 0, 0};
            put16(r + 2, 6 + w->Guest.Width);
            put32(r + 4, w->Id);
            put32(r + 8, x->Gc);
            put16(r + 12, w->Guest.Width);
            put16(r + 14, 1);
            put16(r + 18, (U16)y);
            r[21] = x->Depth;
            U64 hash = 14695981039346656037ULL ^ w->Guest.Width;
            for (U32 col = 0; col < w->Guest.Width; col++) {
                x->Row[col] =
                    x->Agent ? (w->Surface ? w->Surface[y * w->Guest.Width + col] : 0)
                             : DosVideoRgbPixel(vm, w->Guest.X + (INT32)col, w->Guest.Y + (INT32)y);
                hash = (hash ^ x->Row[col]) * 1099511628211ULL;
            }
            if (!w->RowValid[y] || w->RowHash[y] != hash) {
                memcpy(x->PendingImage, r, 24);
                memcpy(x->PendingImage + 24, x->Row, w->Guest.Width * 4);
                x->PendingOffset = 0;
                x->PendingBytes = 24 + w->Guest.Width * 4;
                w->RowHash[y] = hash;
                w->RowValid[y] = TRUE;
                w->NextRow = (y + 1) % w->Guest.Height;
                if (!flush_image(x)) {
                    x->NextWindow = (i + 1) % X_WINDOWS;
                    vm->VideoDirty = TRUE;
                    return TRUE;
                }
            }
            if (!(y & 15))
                DosXPoll(vm);
        }
    }
    vm->VideoDirty = FALSE;
    return TRUE;
}

void DosXInherit(DOS_VM *destination, const DOS_VM *source) {
    DOS_X11 *x = source->XDisplay;
    if (destination->XDisplay == x) {
        destination->XSession = source->XSession;
        return;
    }
    DosXClose(destination);
    destination->XSession = source->XSession;
    destination->XDisplay = x;
    if (x)
        x->References++;
}
