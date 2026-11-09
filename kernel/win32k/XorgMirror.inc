/*
 * Win32 desktop presentation in the bundled Xorg server.
 *
 * Xorg owns the physical framebuffer while it is running, so Win32k cannot
 * draw there directly. This worker exports the already-composed Win32 desktop
 * as managed X11 clients, refreshes their contents after screen damage, and
 * routes Xorg keyboard and pointer events back into Win32k.
 */
#include "../linux/SocketPrivate.h"

typedef struct {
    UINT32 Id;
    INT32 X, Y, SourceX, SourceY, GuestX, GuestY;
    UINT16 Width, Height;
    BOOLEAN Seen;
    UINT8 Buttons;
    UINT8 Keys[16];
} W32K_XWIN;

typedef struct {
    LINUX_SOCKET *Socket;
    UINT32 Root, Visual, Gc, Base, Mask, NextId;
    UINT16 Width, Height;
    UINT8 Depth;
    UINT32 WmProtocols, WmDelete;
    BOOLEAN InputActive;
    UINT8 Event[32];
    UINT8 EventHave;
    BOOLEAN Failed;
    UINT8 *Pixels;
    UINT32 PixelCapacity;
    W32K_XWIN Windows[W32K_SLOTS];
} W32K_XORG;

static W32K_XORG g_w32k_xorg;

BOOLEAN W32kXorgInputActive(void)
{
    return g_w32k_xorg.InputActive;
}

BOOLEAN W32kXorgConnected(void)
{
    return g_w32k_xorg.Socket != NULL;
}

static UINT16 W32kXGet16(const UINT8 *p)
{
    return (UINT16)(p[0] | ((UINT16)p[1] << 8));
}

static UINT32 W32kXGet32(const UINT8 *p)
{
    return W32kXGet16(p) | ((UINT32)W32kXGet16(p + 2) << 16);
}

static void W32kXPut16(UINT8 *p, UINT16 v)
{
    p[0] = (UINT8)v;
    p[1] = (UINT8)(v >> 8);
}

static void W32kXPut32(UINT8 *p, UINT32 v)
{
    W32kXPut16(p, (UINT16)v);
    W32kXPut16(p + 2, (UINT16)(v >> 16));
}

static BOOLEAN W32kXTransfer(W32K_XORG *x, BOOLEAN write, void *buffer,
                             UINT32 bytes)
{
    UINT8 *p = (UINT8 *)buffer;
    while (bytes && !x->Failed) {
        INT64 n = (INT64)LinuxLocalTransfer(x->Socket, write, p, bytes,
                                            write ? 0x40 : 0);
        if (n == -11 || n == 0) {
            KeYield();
            continue;
        }
        if (n < 0) {
            x->Failed = TRUE;
            break;
        }
        p += (UINT32)n;
        bytes -= (UINT32)n;
    }
    return !x->Failed;
}

static BOOLEAN W32kXRequest(W32K_XORG *x, const void *buffer, UINT32 bytes)
{
    return W32kXTransfer(x, TRUE, (void *)buffer, bytes);
}

static UINT32 W32kXInternAtom(W32K_XORG *x, const char *name)
{
    UINT32 n = (UINT32)strlen(name), bytes = 8 + ((n + 3) & ~3u);
    UINT8 req[64] = {16, 0}, reply[32] = {0};
    W32kXPut16(req + 2, (UINT16)(bytes / 4));
    W32kXPut16(req + 6, (UINT16)n);
    memcpy(req + 8, name, n);
    if (!W32kXRequest(x, req, bytes) ||
        !W32kXTransfer(x, FALSE, reply, sizeof(reply)) || reply[0] != 1)
        return 0;
    return W32kXGet32(reply + 8);
}

static UINT32 W32kXResource(W32K_XORG *x)
{
    return x->Base | (++x->NextId & x->Mask);
}

static void W32kXClose(W32K_XORG *x)
{
    if (x->Socket)
        LinuxSocketClose(x->Socket);
    if (x->Pixels)
        ExFreePool(x->Pixels);
    memset(x, 0, sizeof(*x));
}

static BOOLEAN W32kXOpen(W32K_XORG *x)
{
    static const char display[] = "\0/tmp/.X11-unix/X0";
    UINT8 hello[12] = {'l', 0, 11}, prefix[8];
    UINT8 *setup = NULL;
    UINT32 size, root, at;
    BOOLEAN format = FALSE, visual = FALSE;
    UINT32 stage = 0;

    memset(x, 0, sizeof(*x));
    x->Socket = LinuxLocalConnect(display, sizeof(display) - 1);
    stage = 1;
    if (!x->Socket || !W32kXRequest(x, hello, sizeof(hello)) ||
        !W32kXTransfer(x, FALSE, prefix, sizeof(prefix)) || prefix[0] != 1)
        goto fail;
    stage = 2;
    size = (UINT32)W32kXGet16(prefix + 6) * 4;
    if (size < 80 || size > 65536)
        goto fail;
    setup = ExAllocatePoolWithTag(NonPagedPool, size, 'sXwW');
    if (!setup || !W32kXTransfer(x, FALSE, setup, size))
        goto fail;
    stage = 3;
    x->Base = W32kXGet32(setup + 4);
    x->Mask = W32kXGet32(setup + 8);
    if (!setup[20] || setup[22] != 0)
        goto fail;
    root = 32 + ((W32kXGet16(setup + 16) + 3) & ~3u) + (UINT32)setup[21] * 8;
    if (root + 40 > size)
        goto fail;
    x->Root = W32kXGet32(setup + root);
    x->Width = W32kXGet16(setup + root + 20);
    x->Height = W32kXGet16(setup + root + 22);
    x->Visual = W32kXGet32(setup + root + 32);
    x->Depth = setup[root + 38];
    if (x->Depth != 24 || !x->Width || !x->Height ||
        x->Width != GfxFramebuffer.Width || x->Height != GfxFramebuffer.Height)
        goto fail;
    for (UINT32 i = 0; i < setup[21]; ++i) {
        at = 32 + ((W32kXGet16(setup + 16) + 3) & ~3u) + i * 8;
        if (setup[at] == x->Depth && setup[at + 1] == 32 && setup[at + 2] == 32)
            format = TRUE;
    }
    at = root + 40;
    for (UINT32 d = 0; d < setup[root + 39]; ++d) {
        if (at + 8 > size)
            goto fail;
        UINT32 count = W32kXGet16(setup + at + 2);
        at += 8;
        for (UINT32 v = 0; v < count; ++v, at += 24) {
            if (at + 24 > size)
                goto fail;
            if (W32kXGet32(setup + at) == x->Visual &&
                W32kXGet32(setup + at + 8) == 0xff0000 &&
                W32kXGet32(setup + at + 12) == 0xff00 &&
                W32kXGet32(setup + at + 16) == 0xff)
                visual = TRUE;
        }
    }
    if (!format || !visual)
        goto fail;
    stage = 4;

    x->WmProtocols = W32kXInternAtom(x, "WM_PROTOCOLS");
    x->WmDelete = W32kXInternAtom(x, "WM_DELETE_WINDOW");

    x->Gc = W32kXResource(x);
    UINT8 gc[16] = {55, 0, 4, 0};
    W32kXPut32(gc + 4, x->Gc);
    W32kXPut32(gc + 8, x->Root);
    if (!W32kXRequest(x, gc, sizeof(gc)))
        goto fail;
    ExFreePool(setup);
    KeLog("[win32k-x11] connected to Xorg :0 (%ux%u)\n", x->Width, x->Height);
    return TRUE;
fail:
    KeLog("[win32k-x11] connection/setup failed at stage %u\n", stage);
    if (setup)
        ExFreePool(setup);
    W32kXClose(x);
    return FALSE;
}

static BOOLEAN W32kXCreateWindow(W32K_XORG *x, W32K_XWIN *out,
                                 const W32K_WINDOW_STATE *guest)
{
    UINT16 slot = (UINT16)(guest - g_w32k);
    INT32 cx, cy, cw, ch;
    W32kClientOrigin(slot, &cx, &cy, &cw, &ch);
    if (guest->MenuPresent) { cy -= W32K_MENU_HEIGHT; ch += W32K_MENU_HEIGHT; }
    out->Id = W32kXResource(x);
    out->X = guest->X;
    out->Y = guest->Y;
    out->GuestX = guest->X; out->GuestY = guest->Y;
    out->SourceX = cx; out->SourceY = cy;
    out->Width = (UINT16)cw;
    out->Height = (UINT16)ch;
    UINT8 create[36] = {1, 0, 9, 0};
    create[1] = x->Depth;
    W32kXPut32(create + 4, out->Id);
    W32kXPut32(create + 8, x->Root);
    W32kXPut16(create + 12, (UINT16)out->X);
    W32kXPut16(create + 14, (UINT16)out->Y);
    W32kXPut16(create + 16, out->Width);
    W32kXPut16(create + 18, out->Height);
    W32kXPut16(create + 22, 1); /* InputOutput */
    W32kXPut32(create + 24, x->Visual);
    W32kXPut32(create + 28, 1u << 11); /* CWEventMask */
    W32kXPut32(create + 32, (1u << 0) | (1u << 1) | (1u << 2) |
        (1u << 3) | (1u << 6) | (1u << 15) | (1u << 17) | (1u << 21));
    if (!W32kXRequest(x, create, sizeof(create)))
        return FALSE;
    /* WM_NAME and WM_CLASS make this a regular managed application window. */
    const char *title = "Calculator";
    UINT8 prop[64] = {18, 0};
    UINT32 n = (UINT32)strlen(title);
    W32kXPut16(prop + 2, (UINT16)((24 + ((n + 3) & ~3u)) / 4));
    W32kXPut32(prop + 4, out->Id); W32kXPut32(prop + 8, 39);
    W32kXPut32(prop + 12, 31); prop[16] = 8;
    W32kXPut32(prop + 20, n); memcpy(prop + 24, title, n);
    if (!W32kXRequest(x, prop, 24 + ((n + 3) & ~3u))) return FALSE;
    static const UINT8 class_name[] = "svindows\0Svindows\0";
    memset(prop, 0, sizeof(prop)); prop[0] = 18;
    W32kXPut16(prop + 2, 11); W32kXPut32(prop + 4, out->Id);
    W32kXPut32(prop + 8, 67); W32kXPut32(prop + 12, 31); prop[16] = 8;
    W32kXPut32(prop + 20, sizeof(class_name) - 1);
    memcpy(prop + 24, class_name, sizeof(class_name) - 1);
    if (!W32kXRequest(x, prop, 44)) return FALSE;
    if (x->WmProtocols && x->WmDelete) {
        UINT8 protocols[32] = {18, 0, 8, 0};
        W32kXPut32(protocols + 4, out->Id); W32kXPut32(protocols + 8, x->WmProtocols);
        W32kXPut32(protocols + 12, 4); protocols[16] = 32;
        W32kXPut32(protocols + 20, 1); W32kXPut32(protocols + 24, x->WmDelete);
        if (!W32kXRequest(x, protocols, sizeof(protocols))) return FALSE;
    }
    UINT8 map[8] = {8, 0, 2, 0};
    W32kXPut32(map + 4, out->Id);
    if (!W32kXRequest(x, map, sizeof(map)))
        return FALSE;
    KeLog("[win32k-x11] exported HWND slot %u XID %u at %d,%d %ux%u\n",
          (UINT32)(out - x->Windows), out->Id, out->X, out->Y,
          out->Width, out->Height);
    return TRUE;
}

static BOOLEAN W32kXConfigureWindow(W32K_XORG *x, W32K_XWIN *out,
                                    const W32K_WINDOW_STATE *guest)
{
    UINT16 slot = (UINT16)(guest - g_w32k);
    INT32 cx, cy, cw, ch;
    W32kClientOrigin(slot, &cx, &cy, &cw, &ch);
    if (guest->MenuPresent) { cy -= W32K_MENU_HEIGHT; ch += W32K_MENU_HEIGHT; }
    UINT8 r[20] = {12, 0, 5, 0};
    W32kXPut32(r + 4, out->Id);
    W32kXPut16(r + 8, (1u << 2) | (1u << 3)); /* width, height */
    W32kXPut16(r + 12, (UINT16)cw);
    W32kXPut16(r + 14, (UINT16)ch);
    if (!W32kXRequest(x, r, sizeof(r)))
        return FALSE;
    out->GuestX = guest->X; out->GuestY = guest->Y;
    out->SourceX = cx; out->SourceY = cy;
    out->Width = (UINT16)cw; out->Height = (UINT16)ch;
    return TRUE;
}

static BOOLEAN W32kXPutWindow(W32K_XORG *x, W32K_XWIN *out,
                              const W32K_WINDOW_STATE *guest)
{
    INT32 left = out->SourceX < 0 ? 0 : out->SourceX;
    INT32 top = out->SourceY < 0 ? 0 : out->SourceY;
    INT32 dst_x = out->SourceX < 0 ? -out->SourceX : 0;
    INT32 dst_y = out->SourceY < 0 ? -out->SourceY : 0;
    if (dst_x >= (INT32)out->Width || dst_y >= (INT32)out->Height)
        return TRUE;
    if (left >= (INT32)x->Width || top >= (INT32)x->Height)
        return TRUE;
    UINT32 width = out->Width - (UINT32)dst_x;
    UINT32 height = out->Height - (UINT32)dst_y;
    if (width > x->Width - (UINT32)left) width = x->Width - (UINT32)left;
    if (height > x->Height - (UINT32)top) height = x->Height - (UINT32)top;
    if (!width || !height)
        return TRUE;
    const UINT32 max_rows = 20;
    UINT32 bytes = width * max_rows * 4;
    if (x->PixelCapacity < bytes) {
        UINT8 *pixels = ExAllocatePoolWithTag(NonPagedPool, bytes, 'pXwW');
        if (!pixels)
            return FALSE;
        if (x->Pixels) ExFreePool(x->Pixels);
        x->Pixels = pixels;
        x->PixelCapacity = bytes;
    }
    UINT32 stride = g_w32k_composite_w;
    if (!g_w32k_composite || stride < GfxFramebuffer.Width)
        return FALSE;
    for (UINT32 y = 0; y < height; y += max_rows) {
        UINT32 rows = height - y < max_rows ? height - y : max_rows;
        UINT32 image_bytes = width * rows * 4;
        for (UINT32 row = 0; row < rows; ++row) {
            const UINT32 *source = g_w32k_composite +
                (top + (INT32)y + row) * stride + left;
            UINT32 *target = (UINT32 *)x->Pixels + row * width;
            for (UINT32 col = 0; col < width; ++col) {
                UINT32 p = source[col];
                target[col] = (((p >> GfxFramebuffer.RedShift) & 255) << 16) |
                              (((p >> GfxFramebuffer.GreenShift) & 255) << 8) |
                              ((p >> GfxFramebuffer.BlueShift) & 255);
            }
        }
        UINT8 header[24] = {72, 2, 0, 0}; /* PutImage, ZPixmap */
        W32kXPut16(header + 2, (UINT16)((24 + image_bytes) / 4));
        W32kXPut32(header + 4, out->Id);
        W32kXPut32(header + 8, x->Gc);
        W32kXPut16(header + 12, (UINT16)width);
        W32kXPut16(header + 14, (UINT16)rows);
        W32kXPut16(header + 16, (UINT16)(dst_x));
        W32kXPut16(header + 18, (UINT16)(dst_y + y));
        header[21] = x->Depth;
        if (!W32kXRequest(x, header, sizeof(header)) ||
            !W32kXRequest(x, x->Pixels, image_bytes))
            return FALSE;
    }
    return TRUE;
}

static BOOLEAN W32kXPresent(W32K_XORG *x)
{
    BOOLEAN seen[W32K_SLOTS] = {0};
    UINT32 count = 0;
    for (UINT16 slot = g_w32k_top; slot && count < W32K_SLOTS;
         slot = g_w32k[slot].ZPrev) {
        W32K_WINDOW_STATE *guest = &g_w32k[slot];
        if ((guest->Kind != W32K_KIND_TOPLEVEL && guest->Kind != W32K_KIND_TASKBAR) || !guest->Visible ||
            guest->Minimized || guest->Parent || !guest->W || !guest->H ||
            guest->W > 2048 || guest->H > 2048)
            continue;
        count++;
        seen[slot] = TRUE;
        W32K_XWIN *out = &x->Windows[slot];
        out->Seen = TRUE;
        if (!out->Id) {
            if (!W32kXCreateWindow(x, out, guest))
                return FALSE;
        } else {
            INT32 cx, cy, cw, ch;
            W32kClientOrigin(slot, &cx, &cy, &cw, &ch);
            if (guest->MenuPresent) { cy -= W32K_MENU_HEIGHT; ch += W32K_MENU_HEIGHT; }
            out->GuestX = guest->X; out->GuestY = guest->Y;
            out->SourceX = cx; out->SourceY = cy;
            if (out->Width != cw || out->Height != ch) {
            if (!W32kXConfigureWindow(x, out, guest))
                return FALSE;
            }
        }
        if (!W32kXPutWindow(x, out, guest))
            return FALSE;
    }
    for (UINT32 slot = 1; slot < W32K_SLOTS; ++slot) {
        W32K_XWIN *out = &x->Windows[slot];
        if (out->Id && !seen[slot]) {
            UINT8 destroy[8] = {4, 0, 2, 0};
            W32kXPut32(destroy + 4, out->Id);
            if (!W32kXRequest(x, destroy, sizeof(destroy)))
                return FALSE;
            memset(out, 0, sizeof(*out));
        }
    }
    /* TWM owns stacking and focus for managed clients. */
    x->InputActive = count != 0;
    return TRUE;
}

static void W32kXEvents(W32K_XORG *x)
{
    UINT8 e[32];
    while (x->Socket) {
        INT64 got = (INT64)LinuxLocalTransfer(x->Socket, FALSE,
            x->Event + x->EventHave, sizeof(e) - x->EventHave, 0x40);
        if (got <= 0) break;
        x->EventHave += (UINT8)got;
        if (x->EventHave < sizeof(e)) break;
        memcpy(e, x->Event, sizeof(e));
        x->EventHave = 0;
        UINT8 type = e[0] & 0x7f;
        UINT32 id = W32kXGet32(e + ((type >= 2 && type <= 8) ? 12 :
                                   type == 22 ? 8 : 4));
        W32K_XWIN *w = NULL;
        UINT16 slot = 0;
        for (UINT16 i = 1; i < W32K_SLOTS; ++i)
            if (x->Windows[i].Id == id) { w = &x->Windows[i]; slot = i; break; }
        if (!w) continue;
        if (type == 2 || type == 3) {
            UINT8 code = e[1], scan = code >= 8 ? code - 8 : 0;
            BOOLEAN ext = FALSE;
            static const UINT8 extended[28] = {0x1c,0x1d,0x35,0x37,0x38,0,0x47,0x48,
                0x49,0x4b,0x4d,0x4f,0x50,0x51,0x52,0x53};
            if (code >= 104 && code < 132) { scan = extended[code - 104]; ext = TRUE; }
            if (!scan) continue;
            UINT16 ev = scan | (type == 3 ? 0x100 : 0) | (ext ? 0x200 : 0);
            UINT16 ch = 0;
            if (type == 2) {
                UINT8 vk = W32kVirtualKey(scan);
                BOOLEAN shift = (W32kXGet16(e + 28) & 1) != 0;
                if (vk >= 'A' && vk <= 'Z') ch = (UINT16)(shift ? vk : vk + 32);
                else if (vk >= '0' && vk <= '9') ch = vk;
                else if (scan == 0x1c) ch = '\r';
                else if (scan == 0x0e) ch = '\b';
                else if (scan == 0x39) ch = ' ';
            }
            W32kInputKey(ev, ch);
            if (type == 2) w->Keys[scan >> 3] |= (UINT8)(1u << (scan & 7));
            else w->Keys[scan >> 3] &= (UINT8)~(1u << (scan & 7));
        } else if (type == 4 || type == 5 || type == 6) {
            INT32 mx = w->SourceX + (INT16)W32kXGet16(e + 24);
            INT32 my = w->SourceY + (INT16)W32kXGet16(e + 26);
            if (type != 6) {
                UINT8 mask = e[1] == 1 ? 1 : e[1] == 2 ? 4 : e[1] == 3 ? 2 : 0;
                if (type == 4) w->Buttons |= mask; else w->Buttons &= (UINT8)~mask;
            }
            W32kInputMouse(mx, my, w->Buttons);
        } else if (type == 10) {
            W32kInputMouse(w->SourceX, w->SourceY, 0); w->Buttons = 0;
            for (UINT16 scan = 1; scan < 128; ++scan)
                if (w->Keys[scan >> 3] & (1u << (scan & 7))) {
                    W32kInputKey((UINT16)(scan | 0x100), 0);
                    w->Keys[scan >> 3] &= (UINT8)~(1u << (scan & 7));
                }
        } else if (type == 12) {
            UINT32 seq = __sync_fetch_and_add(&g_w32k_xorg_frame_sequence, 0);
            (void)seq;
            W32K_WINDOW_STATE *guest = &g_w32k[slot];
            (void)W32kXPutWindow(x, w, guest);
        } else if (type == 22) {
            UINT16 width = W32kXGet16(e + 20), height = W32kXGet16(e + 22);
            if (width && height && (width != w->Width || height != w->Height))
                W32kMoveWindow(KiUserWindowHandle(slot), g_w32k[slot].X,
                    g_w32k[slot].Y, width + 2 * W32K_BORDER,
                    height + W32K_CAPTION_HEIGHT + W32K_BORDER);
        } else if (type == 33 && W32kXGet32(e + 8) == x->WmProtocols &&
                   W32kXGet32(e + 12) == x->WmDelete) {
            W32kSendWindowMessage(slot, 0x0010, 0, 0);
        }
    }
}

void W32kXorgWorker(PVOID context)
{
    (void)context;
    static const char display[] = "\0/tmp/.X11-unix/X0";
    W32K_XORG *x = &g_w32k_xorg;
    UINT32 last_sequence = (UINT32)-1;
    UINT64 retry_at = 0;
    for (;;) {
        if (!LinuxLocalConnectable(display, sizeof(display) - 1)) {
            if (x->Socket)
                W32kXClose(x);
            last_sequence = (UINT32)-1;
            KeYield();
            continue;
        }
        if (!x->Socket) {
            UINT64 now = KeGetTickCount();
            if (now < retry_at) {
                KeYield();
                continue;
            }
            retry_at = now + 50; /* retry twice per second while Xorg starts */
            if (!W32kXOpen(x)) {
                KeYield();
                continue;
            }
            last_sequence = (UINT32)-1;
        }
        W32kXEvents(x);
        UINT32 sequence = __sync_fetch_and_add(&g_w32k_xorg_frame_sequence, 0);
        if (sequence != last_sequence) {
            if (!W32kXPresent(x)) {
                KeLog("[win32k-x11] Xorg presentation stopped\n");
                W32kXClose(x);
                last_sequence = (UINT32)-1;
            } else {
                last_sequence = sequence;
            }
        }
        KeYield();
    }
}
