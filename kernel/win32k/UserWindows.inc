/* Window classes, properties, window state and framebuffer painting.
 * Included by the owning translation unit. */
/* ------------------------------------------------------------------ */
/* Window classes and the USER atom table                              */
/* ------------------------------------------------------------------ */

#define USER_MAX_CLASSES 64

typedef struct _USER_CLASS_LOCAL {
    WCHAR Name[32];
    UINT64 WndProc;
    UINT64 Instance;
    UINT32 Style;
    UINT32 WndExtra;
    UINT16 Atom;
    BOOLEAN Used;
    PKPROCESS Process;
} USER_CLASS_LOCAL;

static USER_CLASS_LOCAL g_user_classes[USER_MAX_CLASSES];

/* WNDCLASSEXW x64 offsets. */
#define WNDCLASSEX_CBSIZE      0x00
#define WNDCLASSEX_STYLE       0x04
#define WNDCLASSEX_WNDPROC     0x08
#define WNDCLASSEX_HINSTANCE   0x18
#define WNDCLASSEX_CLASSNAME   0x40

static UINT64 NtUserRegisterClassExWOW(UINT64 *a)
{
    /* Win10 signature: (WNDCLASSEXW *lpwcx, PUNICODE_STRING ClassName,
     * PCLSMENUNAME MenuName, DWORD FnId, DWORD Flags, WORD Wow). The client
     * name arrives as a captured UNICODE_STRING in a[1]; the WNDCLASSEX copy
     * in a[0] is the fallback source. */
    const UINT8 *wcex = (const UINT8 *)a[0];
    const UNICODE_STRING *ustr = (const UNICODE_STRING *)a[1];
    WCHAR local[32];
    UINT32 n = 0;
    PKTHREAD caller = KeGetCurrentThread();
    if (!caller || !caller->Process || !wcex ||
        !MmProbeForRead((UINT64)wcex, 80)) return 0;

    KeLog("[user] RegisterClassExWOW(wcex=%p, name_ustr=%p, fnid=%lu, "
          "flags=%lx)\n", (void *)wcex, (void *)ustr,
          (unsigned long)(UINT32)a[3], (unsigned long)(UINT32)a[4]);

    if (ustr && MmProbeForRead((UINT64)ustr, sizeof(*ustr))) {
        UINT32 chars = ustr->Length / 2;
        if (chars > 31)
            chars = 31;
        if (chars && ustr->Buffer &&
            MmProbeForRead((UINT64)ustr->Buffer,
                           (UINT64)chars * sizeof(WCHAR))) {
            memcpy(local, ustr->Buffer, (UINT64)chars * sizeof(WCHAR));
            local[chars] = 0;
            n = chars;
        }
    }
    if (!n && wcex && MmProbeForRead((UINT64)wcex, 80))
        n = KiCopyWideFromUser(*(const WCHAR **)(wcex + WNDCLASSEX_CLASSNAME),
                               local, 32);
    if (!n) {
        KeLog("[user] RegisterClassExWOW: no usable class name\n");
        return 0;
    }

    /* Re-registration returns the existing class atom. */
    for (int i = 0; i < USER_MAX_CLASSES; i++)
        if (g_user_classes[i].Used && g_user_classes[i].Process == caller->Process &&
            KiWideEqual(g_user_classes[i].Name, local))
            return g_user_classes[i].Atom;

    for (int i = 0; i < USER_MAX_CLASSES; i++) {
        if (g_user_classes[i].Used)
            continue;
        memcpy(g_user_classes[i].Name, local, (n + 1) * sizeof(WCHAR));
        g_user_classes[i].Style = *(const UINT32 *)(wcex + WNDCLASSEX_STYLE);
        if (MmProbeForRead((UINT64)(wcex + 0x14), sizeof(UINT32)))
            g_user_classes[i].WndExtra =
                *(const UINT32 *)(wcex + 0x14);
        g_user_classes[i].WndProc =
            *(const UINT64 *)(wcex + WNDCLASSEX_WNDPROC);
        g_user_classes[i].Instance =
            *(const UINT64 *)(wcex + WNDCLASSEX_HINSTANCE);
        /* The class name is interned in the station atom table, so
         * GetAtomName/FindAtom answer for it and no registered window message
         * can be handed the same number. */
        g_user_classes[i].Atom = KiUserAddAtom(local);
        g_user_classes[i].Used = TRUE;
        g_user_classes[i].Process = caller->Process;
        char ascii[33];
        UINT32 j = 0;
        for (; j < n; j++)
            ascii[j] = (char)local[j];
        ascii[j] = 0;
        KeLog("[user] RegisterClass('%s') -> atom 0x%x\n", ascii,
              g_user_classes[i].Atom);
        return g_user_classes[i].Atom;
    }
    return 0;
}

static UINT64 NtUserUnregisterClass(UINT64 *a)
{
    PKTHREAD caller = KeGetCurrentThread();
    if (!caller || !MmProbeForRead(a[1], sizeof(UINT64))) return 0;
    const WCHAR *name = *(const WCHAR **)a[1];
    WCHAR local[32];
    UINT32 n = KiCopyWideFromUser(name, local, 32);
    (void)n;
    for (int i = 0; i < USER_MAX_CLASSES; i++)
        if (g_user_classes[i].Used && g_user_classes[i].Process == caller->Process &&
            KiWideEqual(g_user_classes[i].Name, local)) {
            memset(&g_user_classes[i], 0, sizeof(g_user_classes[i]));
            return 1;
        }
    return 0;
}

static UINT64 NtUserGetClassInfoEx(UINT64 *a)
{
    PKTHREAD caller = KeGetCurrentThread();
    if (!caller) return 0;
    /* (HInstance, PUNICODE_STRING name, WNDCLASSEXW *out, ..., BOOL ansi) */
    const UNICODE_STRING *ustr = (const UNICODE_STRING *)a[1];
    UINT8 *out = (UINT8 *)a[2];
    if (!ustr || !MmProbeForRead((UINT64)ustr, sizeof(*ustr)) ||
        !out || !MmProbeForWrite((UINT64)out, 80))
        return 0;

    WCHAR stackname[32];
    UINT32 chars = ustr->Length / 2;
    if (chars > 31)
        chars = 31;
    for (UINT32 i = 0; i < chars; i++) {
        if (!MmProbeForRead((UINT64)(ustr->Buffer + i), sizeof(WCHAR)))
            return 0;
        stackname[i] = ustr->Buffer[i];
    }
    stackname[chars] = 0;

    for (int i = 0; i < USER_MAX_CLASSES; i++) {
        if (!g_user_classes[i].Used || g_user_classes[i].Process != caller->Process ||
            !KiWideEqual(g_user_classes[i].Name, stackname))
            continue;
        memset(out, 0, 80);
        *(UINT32 *)(out + WNDCLASSEX_STYLE) = g_user_classes[i].Style;
        *(UINT64 *)(out + WNDCLASSEX_WNDPROC) = g_user_classes[i].WndProc;
        *(UINT64 *)(out + WNDCLASSEX_HINSTANCE) = g_user_classes[i].Instance;
        *(const WCHAR **)(out + WNDCLASSEX_CLASSNAME) = ustr->Buffer;
        return g_user_classes[i].Atom;
    }
    return 0;
}

/* Window properties: a small fixed table keyed by (hwnd, name pointer). */
#define USER_MAX_PROPS 128
typedef struct _USER_PROP_LOCAL {
    UINT64 Hwnd;
    UINT64 Name;   /* MAKEINTATOM or string pointer as passed */
    UINT64 Value;
    BOOLEAN Used;
} USER_PROP_LOCAL;

static USER_PROP_LOCAL g_user_props[USER_MAX_PROPS];

static UINT64 NtUserSetProp(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    UINT64 hwnd = a[0];
    UINT64 name = a[1];
    UINT64 value = a[2];
    for (int i = 0; i < USER_MAX_PROPS; i++) {
        if (g_user_props[i].Used && g_user_props[i].Hwnd == hwnd &&
            g_user_props[i].Name == name) {
            g_user_props[i].Value = value;
            return 1;
        }
    }
    for (int i = 0; i < USER_MAX_PROPS; i++) {
        if (g_user_props[i].Used)
            continue;
        g_user_props[i].Used = TRUE;
        g_user_props[i].Hwnd = hwnd;
        g_user_props[i].Name = name;
        g_user_props[i].Value = value;
        return 1;
    }
    return 0;
}

static UINT64 NtUserGetProp(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    UINT64 hwnd = a[0];
    UINT64 name = a[1];
    for (int i = 0; i < USER_MAX_PROPS; i++)
        if (g_user_props[i].Used && g_user_props[i].Hwnd == hwnd &&
            g_user_props[i].Name == name)
            return g_user_props[i].Value;
    return 0;
}

static UINT64 NtUserRemoveProp(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    UINT64 hwnd = a[0];
    UINT64 name = a[1];
    for (int i = 0; i < USER_MAX_PROPS; i++)
        if (g_user_props[i].Used && g_user_props[i].Hwnd == hwnd &&
            g_user_props[i].Name == name) {
            memset(&g_user_props[i], 0, sizeof(g_user_props[i]));
            return 1;
        }
    return 0;
}

/* Per-window longs live behind the window head inside its arena slot. */
#define WINDOW_LONG_BASE 0x60
#define WINDOW_LONG_COUNT 16

/* Offsets inside the client-visible window head that the genuine user32
 * dereferences directly (decoded from its GWL getter and IsWindowVisible):
 * style/+0x30, exStyle/+0x34, fnid-state/+0x42, cbWndExtra/+0xE8,
 * hWndParent/+0x100, WndProc/+0x148, and the window bytes at +0x168. */
#define WND_STYLE_OFF     0x30
#define WND_EXSTYLE_OFF   0x34
#define WND_FNID_OFF      0x42
#define WND_WND_EXTRA_OFF 0xE8
#define WND_PARENT_OFF    0x100
#define WND_WNDPROC_OFF   0x148
#define WND_WINBYTES_OFF  0x168
#define WS_VISIBLE_       0x10000000u

/*
 * NtUserCreateWindowEx receives the class name as a LARGE_STRING (not a
 * UNICODE_STRING): { ULONG Length; ULONG MaximumLength:31, bAnsi:1;
 * PVOID Buffer; }. When the caller passed an atom rather than a string,
 * user32 sets Length = 0 and stuffs the atom into Buffer, which is exactly
 * what win32k tests. Resolve either form against the class registry.
 */
static USER_CLASS_LOCAL *KiClassForCreate(UINT64 class_arg)
{
    PKTHREAD caller = KeGetCurrentThread();
    if (!caller) return NULL;
    if (!class_arg)
        return NULL;

    /* An atom may also arrive directly in the argument slot. */
    UINT16 atom = 0;
    if ((class_arg & ~0xFFFFULL) == 0) {
        atom = (UINT16)class_arg;
    } else {
        if (!MmProbeForRead(class_arg, 16))
            return NULL;
        UINT32 length = *(const UINT32 *)class_arg;
        UINT64 buffer = *(const UINT64 *)(class_arg + 8);
        UINT32 ansi = (*(const UINT32 *)(class_arg + 4)) >> 31;

        if (buffer && (buffer & ~0xFFFFULL) == 0) {
            atom = (UINT16)buffer; /* atom form: Buffer holds the atom */
        } else if (length && buffer) {
            /* String form: match the registered class by name. */
            WCHAR local[32];
            UINT32 chars = 0;
            if (ansi) {
                UINT32 n = length > 31 ? 31 : length;
                if (!MmProbeForRead(buffer, n))
                    return NULL;
                for (UINT32 i = 0; i < n; i++)
                    local[i] = (WCHAR)(UINT8)((const char *)buffer)[i];
                chars = n;
            } else {
                UINT32 n = length / 2;
                if (n > 31)
                    n = 31;
                if (!MmProbeForRead(buffer, (UINT64)n * sizeof(WCHAR)))
                    return NULL;
                memcpy(local, (const void *)buffer, (UINT64)n * sizeof(WCHAR));
                chars = n;
            }
            local[chars] = 0;
            for (int i = 0; i < USER_MAX_CLASSES; i++)
                if (g_user_classes[i].Used && g_user_classes[i].Process == caller->Process &&
                    KiWideEqual(g_user_classes[i].Name, local))
                    return &g_user_classes[i];
            return NULL;
        }
    }

    if (!atom)
        return NULL;
    for (int i = 0; i < USER_MAX_CLASSES; i++)
        if (g_user_classes[i].Used && g_user_classes[i].Process == caller->Process &&
            g_user_classes[i].Atom == atom)
            return &g_user_classes[i];
    return NULL;
}

/* Publish the fields client-side user32 reads without a syscall. */
static void KiPublishWindowState(UINT64 window, UINT32 style, UINT32 exstyle,
                                 UINT64 parent, USER_CLASS_LOCAL *cls)
{
    *(UINT32 *)(window + WND_STYLE_OFF) = style;
    *(UINT32 *)(window + WND_EXSTYLE_OFF) = exstyle;
    *(UINT16 *)(window + WND_FNID_OFF) = 0;
    if (cls) {
        *(UINT32 *)(window + WND_WND_EXTRA_OFF) = cls->WndExtra;
        *(UINT64 *)(window + WND_WNDPROC_OFF) = cls->WndProc;
    } else {
        *(UINT32 *)(window + WND_WND_EXTRA_OFF) = 0x40;
    }
    *(UINT64 *)(window + WND_PARENT_OFF) = parent;
}

static BOOLEAN KiSetWindowLong(UINT64 hwnd, INT32 index, INT64 value,
                               INT64 *old)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!KiUserWindowValid(hwnd, TRUE) || index >= 0 || index < -WINDOW_LONG_COUNT)
        return FALSE;
    INT64 *longs = (INT64 *)(PROCESS_USER_OBJECT_ARENA_VA +
                             (UINT64)slot * 0x200 + WINDOW_LONG_BASE +
                             (UINT64)(-index - 1) * 8);
    if (!MmProbeForWrite((UINT64)longs, 8))
        return FALSE;
    *old = *longs;
    *longs = value;
    return TRUE;
}

static UINT64 NtUserSetWindowLong(UINT64 *a)
{
    INT64 old = 0;
    if (KiSetWindowLong(a[0], (INT32)a[1], (INT64)a[2], &old))
        return (UINT64)old;
    return 0;
}
/* ------------------------------------------------------------------ */
/* win32k-lite: paint the shell windows on the framebuffer            */
/* ------------------------------------------------------------------ */
/* There is no compositor yet; the kernel classifies the shell's own
 * windows by class name and paints simple rectangles for them. Enough to
 * make "the desktop is up" visible: WorkerW fills the screen (wallpaper),
 * Shell_TrayWnd becomes the taskbar strip, everything else is a plain
 * surface. Painting happens on create/show/hide/move so ring-3 state
 * changes become visible without any client-side drawing. */

#define WINPAINT_NONE     0
#define WINPAINT_DESKTOP  1
#define WINPAINT_TASKBAR  2
#define WINPAINT_GENERIC  3
#define WINPAINT_SLOTS    128

static UINT8  g_win_kind[WINPAINT_SLOTS];
static INT32  g_win_x[WINPAINT_SLOTS], g_win_y[WINPAINT_SLOTS];
static UINT32 g_win_w[WINPAINT_SLOTS], g_win_h[WINPAINT_SLOTS];

static BOOLEAN KiClassNameIs(const WCHAR *name, const char *ascii)
{
    UINT32 i = 0;
    for (; ascii[i]; i++)
        if (name[i] != (WCHAR)(UINT8)ascii[i])
            return FALSE;
    return name[i] == 0;
}

static void KiPaintWindowBySlot(UINT16 slot)
{
    if (!KiUserWindowValid(KiUserWindowHandle(slot), TRUE)) return;
    UINT32 style = *(volatile UINT32 *)(PROCESS_USER_OBJECT_ARENA_VA +
                                        (UINT64)slot * 0x200 + WND_STYLE_OFF);
    if (!(style & WS_VISIBLE_))
        return;

    UINT32 sw = GfxFramebuffer.Width, sh = GfxFramebuffer.Height;

    if (g_win_kind[slot] == WINPAINT_DESKTOP) {
        GfxFillRect(0, 0, sw, sh, GfxColor(0x0e, 0x2a, 0x47));
        return;
    }
    if (g_win_kind[slot] == WINPAINT_TASKBAR) {
        UINT32 h = g_win_h[slot] ? g_win_h[slot] : 40;
        if (h > sh / 3)
            h = 40;
        UINT32 y = sh - h;
        GfxFillRect(0, y, sw, h, GfxColor(0x1f, 0x1f, 0x1f));
        GfxFillRect(0, y, sw, 2, GfxColor(0x00, 0x78, 0xd7));
        GfxFillRect(0, y + 2, 48, h - 2, GfxColor(0x2d, 0x2d, 0x2d));
        GfxDrawString(14, y + (h - GFX_FONT_H) / 2 + 1, "SVINDOWS",
                      GfxColor(0xff, 0xff, 0xff), GfxColor(0x2d, 0x2d, 0x2d));
        return;
    }

    /* Generic top-level window: a plain surface with a border. */
    {
    INT32 x = g_win_x[slot], y = g_win_y[slot];
    UINT32 w = g_win_w[slot], h = g_win_h[slot];
    if ((UINT32)x >= sw || (UINT32)y >= sh || !w || !h)
        return;
    if (x + (INT32)w > (INT32)sw)
        w = sw - (UINT32)x;
    if (y + (INT32)h > (INT32)sh)
        h = sh - (UINT32)y;
    GfxFillRect((UINT32)x, (UINT32)y, w, h, GfxColor(0xf0, 0xf0, 0xf0));
    GfxFillRect((UINT32)x, (UINT32)y, w, 1, GfxColor(0x10, 0x10, 0x10));
    GfxFillRect((UINT32)x, (UINT32)y, 1, h, GfxColor(0x10, 0x10, 0x10));
    }
}

static void KiRepaintAll(void);
static UINT64 NtUserShowWindow(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    UINT16 slot = (UINT16)(a[0] & 0xFFFF);
    UINT32 cmd = (UINT32)a[1] & 0xF;
    if (slot && slot < 256) {
        UINT64 window = PROCESS_USER_OBJECT_ARENA_VA + (UINT64)slot * 0x200;
        if (MmProbeForWrite(window + WND_STYLE_OFF, sizeof(UINT32))) {
            UINT32 *style = (UINT32 *)(window + WND_STYLE_OFF);
            if (cmd == 0) /* SW_HIDE */
                *style &= ~WS_VISIBLE_;
            else
                *style |= WS_VISIBLE_;
        }
    }
    /* Becoming visible makes the whole window invalid, which is what turns
     * into the window's first WM_PAINT. */
    if (cmd == 0)
        KiUserValidateWindow(slot);
    else
        KiUserInvalidateWindow(slot);
    KeLog("[user] ShowWindow(HWND %p, cmd %lu)\n", (void *)a[0],
          (unsigned long)cmd);
    KiRepaintAll();
    return 1;
}

static UINT64 NtUserSetWindowPos(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    /* Genuine win32k argument order: (hwnd, hWndInsertAfter, x, y,
     * cx, cy, flags). SWP_NOMOVE=0x2, SWP_NOSIZE=0x1, SWP_SHOWWINDOW=0x40,
     * SWP_HIDEWINDOW=0x80. */
    UINT32 flags = (UINT32)a[6];
    UINT16 slot = (UINT16)(a[0] & 0xFFFF);
    if (slot && slot < WINPAINT_SLOTS && g_win_kind[slot]) {
        if (!(flags & 0x2)) {
            g_win_x[slot] = (INT32)a[2];
            g_win_y[slot] = (INT32)a[3];
        }
        if (!(flags & 0x1)) {
            g_win_w[slot] = (UINT32)a[4];
            g_win_h[slot] = (UINT32)a[5];
        }
    }
    if (flags & 0xC0) {
        if (slot && slot < 256) {
            UINT64 window = PROCESS_USER_OBJECT_ARENA_VA +
                            (UINT64)slot * 0x200;
            if (MmProbeForWrite(window + WND_STYLE_OFF, sizeof(UINT32))) {
                UINT32 *style = (UINT32 *)(window + WND_STYLE_OFF);
                if (flags & 0x40)
                    *style |= WS_VISIBLE_;
                if (flags & 0x80)
                    *style &= ~WS_VISIBLE_;
            }
        }
        if (flags & 0x80)
            KiUserValidateWindow(slot);
    }
    /* Anything but a pure no-op move invalidates: a resized or newly shown
     * window has to repaint. SWP_NOREDRAW (0x8) is the caller opting out. */
    if (!(flags & 0x8) && (flags & 0x40 || !(flags & 0x3)))
        KiUserInvalidateWindow(slot);
    KeLog("[user] SetWindowPos(HWND %p, %ld,%ld %lux%lu, flags 0x%lx)\n",
          (void *)a[0], (long)(INT32)a[2], (long)(INT32)a[3],
          (unsigned long)(UINT32)a[4], (unsigned long)(UINT32)a[5],
          (unsigned long)flags);
    KiRepaintAll();
    return 1;
}

/* Full-screen repaint in creation order, with the desktop underneath. */
static void KiRepaintAll(void)
{
    if (!GfxAvailable())
        return;
    for (UINT16 slot = 1; slot < WINPAINT_SLOTS; slot++) {
        if (KiUserWindowValid(KiUserWindowHandle(slot), TRUE) &&
            g_win_kind[slot] == WINPAINT_DESKTOP &&
            (*(volatile UINT32 *)(PROCESS_USER_OBJECT_ARENA_VA +
                                  (UINT64)slot * 0x200 + WND_STYLE_OFF)) &
                WS_VISIBLE_)
            KiPaintWindowBySlot(slot);
    }
    for (UINT16 slot = 1; slot < WINPAINT_SLOTS; slot++)
        KiPaintWindowBySlot(slot);
}


static UINT64 NtUserMoveWindow(UINT64 *a) { return 1; }

/* (hwnd, const RECT *rect, BOOL erase). The update region is whole-window, so
 * a partial rectangle just marks the window dirty. */
static UINT64 NtUserInvalidateRect(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    KiUserInvalidateWindow((UINT16)(a[0] & 0xFFFF));
    return 1;
}

/* (hwnd, const RECT *rect) */
static UINT64 NtUserValidateRect(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    KiUserValidateWindow((UINT16)(a[0] & 0xFFFF));
    return 1;
}

/* (hwnd, const RECT *rect, HRGN rgn, UINT flags): RDW_INVALIDATE=0x1,
 * RDW_VALIDATE=0x8. */
static UINT64 NtUserRedrawWindow(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    UINT32 flags = (UINT32)a[3];
    if (flags & 0x8)
        KiUserValidateWindow((UINT16)(a[0] & 0xFFFF));
    else if (flags & 0x1)
        KiUserInvalidateWindow((UINT16)(a[0] & 0xFFFF));
    return 1;
}

/* (hwnd, RECT *out, BOOL erase): the update rectangle in client coordinates,
 * empty when the window is clean. */
static UINT64 NtUserGetUpdateRect(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    UINT16 slot = (UINT16)(a[0] & 0xFFFF);
    BOOLEAN dirty = slot && slot < USER_PAINT_SLOTS && g_window_update[slot];
    INT32 *rect = (INT32 *)a[1];
    if (rect && MmProbeForWrite((UINT64)rect, 16)) {
        rect[0] = 0;
        rect[1] = 0;
        rect[2] = dirty && slot < WINPAINT_SLOTS ? (INT32)g_win_w[slot] : 0;
        rect[3] = dirty && slot < WINPAINT_SLOTS ? (INT32)g_win_h[slot] : 0;
    }
    return dirty ? 1 : 0;
}

static UINT64 NtUserUpdateWindow(UINT64 *a) { return 1; }
static UINT64 NtUserPostMessage(UINT64 *a)
{
    return KiUserEnqueueMessage(a[0], (UINT32)a[1], a[2], (INT64)a[3],
                                0, 0);
}

/* Continuation codes carried in KTHREAD.CallbackContinue. */
#define CB_CONT_CREATE_NCCREATE 1 /* next: WM_NCCREATE result handling   */
#define CB_CONT_CREATE_WMCREATE 2 /* next: WM_CREATE result handling     */
#define CB_CONT_SIMPLE          3 /* hand the WndProc result to the caller */

/* The kernel->user callback machinery is defined further down (it needs the
 * loader's stub arena); the message services here are its first users. */
static BOOLEAN KiStartUserCallback(UINT64 target, UINT64 a1, UINT64 a2,
                                   UINT64 a3, UINT64 a4, UINT64 continuation,
                                   UINT64 window, UINT64 msg, UINT64 lparam);

/*
 * Synchronous message delivery: SendMessage (NtUserMessageCall) and the message
 * pump's DispatchMessage both have to run the target window's procedure and
 * hand its LRESULT back to the caller. Both used to return 0 without calling
 * anything, which silently voided every SendMessage the shell makes -- and a
 * message pump whose DispatchMessage does nothing can never advance a window's
 * state machine. Both now go through the kernel->user callback path that
 * window creation already uses.
 */
static UINT64 KiWindowProcOf(UINT64 hwnd)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!KiUserWindowValid(hwnd, TRUE))
        return 0;
    UINT64 window = PROCESS_USER_OBJECT_ARENA_VA + (UINT64)slot * 0x200;
    if (!MmProbeForRead(window + WND_WNDPROC_OFF, sizeof(UINT64)))
        return 0;
    return *(volatile UINT64 *)(window + WND_WNDPROC_OFF);
}

/* Returns TRUE when the callback was armed; the result then reaches the caller
 * through NtCallbackReturn, so the service's own return value is unused. */
static BOOLEAN KiStartUserCallback(UINT64 target, UINT64 a1, UINT64 a2,
                                   UINT64 a3, UINT64 a4, UINT64 continuation,
                                   UINT64 window, UINT64 msg, UINT64 lparam);
static BOOLEAN KiSendToWindowProc(UINT64 hwnd, UINT32 message, UINT64 wparam,
                                  INT64 lparam)
{
    UINT64 proc = KiWindowProcOf(hwnd);
    if (!proc)
        return FALSE;
    return KiStartUserCallback(proc, hwnd, message, wparam, (UINT64)lparam,
                               CB_CONT_SIMPLE, hwnd, message, (UINT64)lparam);
}

static UINT64 NtUserMessageCall(UINT64 *a)
{
    UINT64 hwnd = a[0];
    UINT32 message = (UINT32)a[1];
    UINT32 type = (UINT32)a[5];

    static UINT8 logged;
    if (logged < 32) {
        logged++;
        KeLog("[user] MessageCall(hwnd=%p, msg=0x%x, wp=%p, lp=%p, "
              "type=0x%lx)\n", (void *)hwnd, (unsigned)message, (void *)a[2],
              (void *)a[3], (unsigned long)type);
    }
    if (KiSendToWindowProc(hwnd, message, a[2], (INT64)a[3]))
        return 0; /* the WndProc's result is returned by NtCallbackReturn */
    return 0;
}

static UINT64 NtUserDispatchMessage(UINT64 *a)
{
    const USER_MSG_LOCAL *msg = (const USER_MSG_LOCAL *)a[0];
    if (!msg || !MmProbeForRead((UINT64)msg, sizeof(*msg)))
        return 0;
    /* A thread message (hwnd == 0) has no window procedure to run. */
    if (!msg->Hwnd)
        return 0;
    if (KiSendToWindowProc(msg->Hwnd, msg->Message, msg->WParam, msg->LParam))
        return 0;
    return 0;
}
static UINT64 NtUserTranslateMessage(UINT64 *a) { return 0; }
static UINT64 NtUserTranslateAccelerator(UINT64 *a) { return 0; }
static UINT64 NtUserSetTimer(UINT64 *a) { return 1; }
static UINT64 NtUserKillTimer(UINT64 *a) { return 1; }
static UINT64 NtUserSetActiveWindow(UINT64 *a)
{
    UINT64 previous = g_user_active_hwnd;
    if (a[0] && !KiUserWindowValid(a[0], TRUE))
        return 0;
    g_user_active_hwnd = a[0];
    return previous;
}
static UINT64 NtUserSetCapture(UINT64 *a) { return 0; }
static UINT64 NtUserReleaseCapture(UINT64 *a) { return 1; }
static UINT64 NtUserGetForegroundWindow(UINT64 *a)
{
    return g_user_active_hwnd;
}
static UINT64 NtUserQueryWindow(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], FALSE)) return 0;
    /* QueryWindow(hwnd, type): type 4 = window thread process id. */
    if ((UINT32)a[1] == 4) {
        return g_window_process[(UINT16)a[0]]->ProcessId;
    }
    return 0;
}
static UINT64 NtUserGetAncestor(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    /* GA_ROOT(2)/GA_ROOTOWNER(3): the single top-level window is itself. */
    return a[0];
}
static UINT64 NtUserWaitMessage(UINT64 *a)
{
    USER_MSG_LOCAL msg;
    while (!KiUserTakeMessage(&msg, FALSE))
        KeYield();
    return 0;
}
static UINT64 NtUserGetProcessWindowStation(UINT64 *a) { return 0x1; }
static UINT64 NtUserGetThreadDesktop(UINT64 *a) { return 0x2; }
static UINT64 NtUserSetThreadDesktop(UINT64 *a) { return 1; }
static UINT64 NtUserOpenInputDesktop(UINT64 *a) { return 0x3; }
static UINT64 NtUserCloseDesktop(UINT64 *a) { return 1; }
static UINT64 NtUserOpenDesktop(UINT64 *a) { return 0x3; }
static UINT64 NtUserOpenWindowStation(UINT64 *a) { return 0x4; }
static UINT64 NtUserSetProcessWindowStation(UINT64 *a) { return 1; }
static UINT64 NtUserCloseWindowStation(UINT64 *a) { return 1; }
static UINT64 NtUserNotifyWinEvent(UINT64 *a) { return 0; }
static UINT64 NtUserCallHwnd(UINT64 *a) { return 0; }
static UINT64 NtUserCallHwndLock(UINT64 *a) { return 0; }
static UINT64 NtUserCallHwndParam(UINT64 *a) { return 0; }
static UINT64 NtUserCallTwoParam(UINT64 *a) { return 0; }
static UINT64 NtUserEnumDisplayMonitors(UINT64 *a) { return 0; }
static UINT64 NtUserGetDC(UINT64 *a)
{
    if (a[0] && !KiUserWindowValid(a[0], TRUE)) return 0;
    /* GetDC(NULL) means the whole screen; GetDC(hwnd) is clipped to the
     * window. Non-client areas are not modelled, so the client rectangle is
     * the window rectangle. */
    UINT64 hwnd = a[0];
    INT32 x = 0, y = 0;
    INT32 w = (INT32)GfxFramebuffer.Width, h = (INT32)GfxFramebuffer.Height;
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (hwnd && slot && slot < WINPAINT_SLOTS && g_win_kind[slot]) {
        x = g_win_x[slot];
        y = g_win_y[slot];
        w = (INT32)g_win_w[slot];
        h = (INT32)g_win_h[slot];
    }
    return W32kAcquireWindowDc(hwnd, x, y, w, h);
}

/* GetDCEx(hwnd, hrgnClip, flags) and GetWindowDC(hwnd) reach the same cached
 * device context; the clipping region and the non-client distinction are not
 * modelled yet. */
static UINT64 NtUserGetDCEx(UINT64 *a)
{
    return NtUserGetDC(a);
}
