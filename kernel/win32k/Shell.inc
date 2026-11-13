/* Desktop-owned shell registration, work area, task notifications and public
 * window actions. Included after Session and the window manager. */
#include <abi/WindowEnvironment.h>
#include <svindows/Session.h>

typedef struct _W32K_SHELL_HOOK {
    struct _W32K_SHELL_HOOK *Next;
    UINT64 Window;
} W32K_SHELL_HOOK;

typedef struct _W32K_SHELL_DESKTOP {
    struct _W32K_SHELL_DESKTOP *Next;
    PVOID Desktop;
    UINT64 ShellWindow;
    UINT16 Focus, Active;
    INT32 WorkArea[4];
    BOOLEAN WorkAreaSet;
    W32K_SHELL_HOOK *Hooks;
} W32K_SHELL_DESKTOP;

static W32K_SHELL_DESKTOP *g_shell_desktops;
static UINT32 g_shell_hook_message;

static void W32kEnvironmentError(UINT32 error)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (thread && thread->UserGsBase &&
        MmProbeForWrite(thread->UserGsBase + 0x68, sizeof(error)))
        *(UINT32 *)(thread->UserGsBase + 0x68) = error;
}

static W32K_SHELL_DESKTOP *W32kShellDesktop(PVOID identity, BOOLEAN create)
{
    for (W32K_SHELL_DESKTOP *desktop = g_shell_desktops; desktop; desktop = desktop->Next)
        if (desktop->Desktop == identity) return desktop;
    if (!create) return NULL;
    W32K_SHELL_DESKTOP *desktop = ExAllocatePoolWithTag(NonPagedPool,
        sizeof(*desktop), 'lhsS');
    if (!desktop) return NULL;
    memset(desktop, 0, sizeof(*desktop));
    desktop->Desktop = identity;
    desktop->Next = g_shell_desktops;
    g_shell_desktops = desktop;
    return desktop;
}

static PVOID W32kFocusTargetDesktop(UINT16 slot)
{
    if (slot) return W32kGetWindowDesktopIdentity(slot);
    PKTHREAD caller = KeGetCurrentThread();
    return caller && caller->UserMode ? W32kGetCurrentDesktopIdentity() : W32kGetInputDesktopIdentity();
}

static BOOLEAN W32kFocusHiddenDesktop(UINT16 slot)
{
    PVOID identity = W32kFocusTargetDesktop(slot);
    if (identity == W32kGetInputDesktopIdentity()) return FALSE;
    W32K_SHELL_DESKTOP *desktop = W32kShellDesktop(identity, TRUE);
    if (!desktop) { W32kEnvironmentError(8); return TRUE; }
    UINT16 previous = desktop->Focus;
    UINT16 old_active = desktop->Active, active = W32kRootSlot(slot);
    desktop->Focus = slot; desktop->Active = active;
    if (W32kWindowExists(old_active)) g_w32k[old_active].Active = FALSE;
    if (W32kWindowExists(active)) { g_w32k[active].Active = TRUE; g_w32k[active].LastFocus = slot; }
    if (previous != slot) {
        if (previous) W32kSendWindowMessage(previous, W32K_WM_KILLFOCUS, slot ? KiUserWindowHandle(slot) : 0, 0);
        if (slot) W32kSendWindowMessage(slot, W32K_WM_SETFOCUS, previous ? KiUserWindowHandle(previous) : 0, 0);
    }
    if (old_active != active) {
        if (old_active) W32kSendWindowMessage(old_active, W32K_WM_NCACTIVATE, 0, 0);
        if (active) { W32kSendWindowMessage(active, W32K_WM_NCACTIVATE, 1, 0); W32kNotifyShell(active, 4); }
    }
    return TRUE;
}

static BOOLEAN W32kActivateHiddenDesktop(UINT16 slot)
{
    PVOID identity = W32kFocusTargetDesktop(slot);
    if (identity == W32kGetInputDesktopIdentity()) return FALSE;
    W32K_SHELL_DESKTOP *desktop = W32kShellDesktop(identity, TRUE);
    if (!desktop) { W32kEnvironmentError(8); return TRUE; }
    UINT16 previous = desktop->Active;
    desktop->Active = slot;
    if (W32kWindowExists(previous)) g_w32k[previous].Active = FALSE;
    if (W32kWindowExists(slot)) g_w32k[slot].Active = TRUE;
    if (previous != slot) {
        if (previous) W32kSendWindowMessage(previous, W32K_WM_NCACTIVATE, 0, 0);
        if (slot) { W32kSendWindowMessage(slot, W32K_WM_NCACTIVATE, 1, 0); W32kNotifyShell(slot, 4); }
    }
    return TRUE;
}

static UINT16 W32kCurrentFocusWindow(void)
{
    PVOID identity = W32kGetCurrentDesktopIdentity();
    if (identity == W32kGetInputDesktopIdentity()) return W32kFocusWindow();
    W32K_SHELL_DESKTOP *desktop = W32kShellDesktop(identity, FALSE);
    return desktop && W32kWindowExists(desktop->Focus) ? desktop->Focus : 0;
}

static UINT16 W32kCurrentActiveWindow(void)
{
    PVOID identity = W32kGetCurrentDesktopIdentity();
    if (identity == W32kGetInputDesktopIdentity()) return g_w32k_active;
    W32K_SHELL_DESKTOP *desktop = W32kShellDesktop(identity, FALSE);
    return desktop && W32kWindowExists(desktop->Active) ? desktop->Active : 0;
}

void W32kInputDesktopChanged(PVOID previous, PVOID current)
{
    UINT64 flags = KiIrqSave();
    W32K_SHELL_DESKTOP *old = W32kShellDesktop(previous, TRUE);
    if (old) { old->Focus = g_w32k_focus; old->Active = g_w32k_active; }
    W32K_SHELL_DESKTOP *next = W32kShellDesktop(current, FALSE);
    if (g_w32k_active && W32kWindowExists(g_w32k_active)) {
        g_w32k[g_w32k_active].Active = FALSE;
        W32kSendWindowMessage(g_w32k_active, W32K_WM_NCACTIVATE, 0, 0);
    }
    if (g_w32k_focus) W32kSendWindowMessage(g_w32k_focus, W32K_WM_KILLFOCUS, 0, 0);
    g_w32k_focus = next && W32kWindowExists(next->Focus) ? next->Focus : 0;
    g_w32k_active = next && W32kWindowExists(next->Active) ? next->Active : W32kRootSlot(g_w32k_focus);
    if (!g_w32k_focus) {
        for (UINT16 slot = g_w32k_top; slot; slot = g_w32k[slot].ZPrev)
            if (W32kWindowOnInputDesktop(slot) && g_w32k[slot].Visible && !g_w32k[slot].Minimized &&
                !g_w32k[slot].Parent && g_w32k[slot].Kind != W32K_KIND_TASKBAR) {
                g_w32k_focus = slot; g_w32k_active = slot; break;
            }
    }
    g_w32k_capture = g_w32k_drag = 0;
    if (g_w32k_active) {
        g_w32k[g_w32k_active].Active = TRUE;
        W32kSendWindowMessage(g_w32k_active, W32K_WM_NCACTIVATE, 1, 0);
    }
    if (g_w32k_focus) W32kSendWindowMessage(g_w32k_focus, W32K_WM_SETFOCUS, 0, 0);
    W32kScreenCompose();
    KiIrqRestore(flags);
}

static void W32kShellDeleteSession(PVOID identity, BOOLEAN station)
{
    if (station) return;
    W32K_SHELL_DESKTOP **link = &g_shell_desktops;
    while (*link && (*link)->Desktop != identity) link = &(*link)->Next;
    if (!*link) return;
    W32K_SHELL_DESKTOP *desktop = *link;
    *link = desktop->Next;
    while (desktop->Hooks) {
        W32K_SHELL_HOOK *hook = desktop->Hooks;
        desktop->Hooks = hook->Next;
        ExFreePool(hook);
    }
    ExFreePool(desktop);
}

static void W32kShellWorkArea(INT32 *area)
{
    W32K_SHELL_DESKTOP *desktop = W32kShellDesktop(W32kGetInputDesktopIdentity(), FALSE);
    if (desktop && desktop->WorkAreaSet) memcpy(area, desktop->WorkArea, 16);
    else {
        area[0] = area[1] = 0;
        area[2] = (INT32)GfxFramebuffer.Width;
        area[3] = (INT32)GfxFramebuffer.Height;
    }
}

static UINT64 W32kShellSystemParametersInfo(UINT64 *a)
{
    UINT32 action = (UINT32)a[0];
    if (action != 0x30 && action != 0x2f) return (UINT64)-1;
    UINT64 flags = KiIrqSave();
    if (action == 0x30) {
        if (!MmProbeForWrite(a[2], 16)) {
            KiIrqRestore(flags); W32kEnvironmentError(998); return 0;
        }
        W32K_SHELL_DESKTOP *desktop = W32kShellDesktop(W32kGetCurrentDesktopIdentity(), FALSE);
        INT32 area[4] = {0, 0, (INT32)GfxFramebuffer.Width, (INT32)GfxFramebuffer.Height};
        if (desktop && desktop->WorkAreaSet) memcpy(area, desktop->WorkArea, 16);
        memcpy((void *)a[2], area, 16);
        KiIrqRestore(flags);
        return 1;
    }
    if (!MmProbeForRead(a[2], 16)) {
        KiIrqRestore(flags); W32kEnvironmentError(998); return 0;
    }
    INT32 area[4];
    memcpy(area, (void *)a[2], 16);
    if (area[0] < 0 || area[1] < 0 || area[2] <= area[0] || area[3] <= area[1] ||
        area[2] > (INT32)GfxFramebuffer.Width || area[3] > (INT32)GfxFramebuffer.Height) {
        KiIrqRestore(flags); W32kEnvironmentError(87); return 0;
    }
    W32K_SHELL_DESKTOP *desktop = W32kShellDesktop(W32kGetCurrentDesktopIdentity(), TRUE);
    if (!desktop) { KiIrqRestore(flags); W32kEnvironmentError(8); return 0; }
    memcpy(desktop->WorkArea, area, 16);
    desktop->WorkAreaSet = TRUE;
    if ((UINT32)a[3] & 2) {
        for (UINT16 slot = 1; slot < WINPAINT_SLOTS; ++slot)
            if (g_window_owner[slot] && !W32kWindowParent(slot) && W32kWindowOnCurrentDesktop(slot))
                W32kSendWindowMessage(slot, 0x001a, action, 0); /* WM_SETTINGCHANGE */
    }
    KiIrqRestore(flags);
    return 1;
}

static void W32kNotifyShell(UINT16 slot, UINT32 event)
{
    if (!slot || slot >= WINPAINT_SLOTS || !g_window_owner[slot]) return;
    UINT64 hwnd = KiUserWindowHandle(slot);
    BOOLEAN task = W32kWindowExists(slot) && !g_w32k[slot].Parent &&
        g_w32k[slot].Kind != W32K_KIND_CHILD &&
        g_w32k[slot].Kind != W32K_KIND_DESKTOP &&
        g_w32k[slot].Kind != W32K_KIND_TASKBAR &&
        !(g_w32k[slot].ExStyle & 0x80u) &&
        (!g_w32k[slot].Owner || (g_w32k[slot].ExStyle & 0x40000u));
    for (W32K_SHELL_DESKTOP *desktop = g_shell_desktops; desktop; desktop = desktop->Next) {
        if (event == 2) {
            if (desktop->Focus == slot) desktop->Focus = 0;
            if (desktop->Active == slot) desktop->Active = 0;
        }
        if (event == 2 && desktop->ShellWindow == hwnd) {
            desktop->ShellWindow = 0;
            desktop->WorkAreaSet = FALSE;
        }
        W32K_SHELL_HOOK **link = &desktop->Hooks;
        while (*link) {
            W32K_SHELL_HOOK *hook = *link;
            if (!KiUserWindowValid(hook->Window, FALSE) || (event == 2 && hook->Window == hwnd)) {
                *link = hook->Next;
                ExFreePool(hook);
                continue;
            }
            /* The hook and target must belong to the same actual desktop. */
            if (task && W32kWindowsShareDesktop((UINT16)hook->Window, slot))
                KiUserEnqueueMessage(hook->Window, g_shell_hook_message,
                    event, (INT64)hwnd, 0, 0);
            link = &hook->Next;
        }
    }
}

static UINT64 SvNtUserShellControl(UINT64 *a)
{
    UINT32 operation = (UINT32)a[0];
    UINT64 hwnd = a[1];
    UINT64 flags = KiIrqSave();
    W32K_SHELL_DESKTOP *desktop = W32kShellDesktop(W32kGetCurrentDesktopIdentity(),
        operation == SV_SHELL_SET_WINDOW || operation == SV_SHELL_REGISTER_HOOK);
    if (operation == SV_SHELL_GET_WINDOW) {
        UINT64 result = desktop ? desktop->ShellWindow : 0;
        KiIrqRestore(flags); return result;
    }
    if (!KiUserWindowValid(hwnd, FALSE) || !W32kWindowOnCurrentDesktop((UINT16)hwnd)) {
        KiIrqRestore(flags); W32kEnvironmentError(1400); return 0;
    }
    UINT16 slot = (UINT16)hwnd;
    PKTHREAD caller = KeGetCurrentThread();
    if (operation == SV_SHELL_SET_FOREGROUND) {
        if (!g_w32k[slot].Visible) { KiIrqRestore(flags); return 0; }
        UINT16 root = W32kRootSlot(slot);
        if (g_w32k[root].Minimized) W32kShowWindow(KiUserWindowHandle(root), 9);
        W32kRaise(root);
        UINT16 focus = g_w32k[root].LastFocus;
        if (!W32kWindowExists(focus) || W32kRootSlot(focus) != root) focus = root;
        W32kSetFocus(KiUserWindowHandle(focus));
        KiIrqRestore(flags); return 1;
    }
    if (operation == SV_SHELL_IS_ENABLED) {
        BOOLEAN enabled = !(g_w32k[slot].Style & 0x08000000u);
        KiIrqRestore(flags); return enabled;
    }
    if (operation == SV_SHELL_IS_TOPMOST) {
        BOOLEAN topmost = !!(g_w32k[slot].ExStyle & 8u);
        KiIrqRestore(flags); return topmost;
    }
    if (!caller || caller->Process != g_window_process[slot]) {
        KiIrqRestore(flags); W32kEnvironmentError(5); return 0;
    }
    if (operation == SV_SHELL_ENABLE_WINDOW) {
        BOOLEAN previous_disabled = !!(g_w32k[slot].Style & 0x08000000u);
        BOOLEAN enabled = a[2] != 0;
        if (enabled == !previous_disabled) { KiIrqRestore(flags); return previous_disabled; }
        W32kSendWindowMessage(slot, 0x001f, 0, 0); /* WM_CANCELMODE */
        UINT32 style = enabled ? g_w32k[slot].Style & ~0x08000000u :
                                 g_w32k[slot].Style | 0x08000000u;
        W32kSetStyle(slot, style);
        if (!enabled && W32kRootSlot(W32kFocusWindow()) == slot) W32kSetFocus(0);
        W32kSendWindowMessage(slot, 0x000a, enabled, 0); /* WM_ENABLE */
        KiIrqRestore(flags); return previous_disabled;
    }
    if (!desktop) { KiIrqRestore(flags); W32kEnvironmentError(8); return 0; }
    if (operation == SV_SHELL_SET_WINDOW) {
        if (g_w32k[slot].Parent || (desktop->ShellWindow && desktop->ShellWindow != hwnd)) {
            KiIrqRestore(flags); W32kEnvironmentError(5); return 0;
        }
        desktop->ShellWindow = hwnd;
        g_w32k[slot].Kind = W32K_KIND_DESKTOP;
        W32kRaise(slot);
        if (!W32kFocusWindow() && W32kWindowOnInputDesktop(slot)) W32kSetFocus(hwnd);
        W32kScreenCompose();
        KiIrqRestore(flags); return 1;
    }
    if (operation == SV_SHELL_REGISTER_HOOK) {
        for (W32K_SHELL_HOOK *hook = desktop->Hooks; hook; hook = hook->Next)
            if (hook->Window == hwnd) { KiIrqRestore(flags); return 1; }
        W32K_SHELL_HOOK *hook = ExAllocatePoolWithTag(NonPagedPool, sizeof(*hook), 'khsS');
        if (!hook) { KiIrqRestore(flags); W32kEnvironmentError(8); return 0; }
        static const WCHAR name[] = {'S','H','E','L','L','H','O','O','K',0};
        if (!g_shell_hook_message) g_shell_hook_message = KiUserAddAtom(name);
        if (!g_shell_hook_message) { ExFreePool(hook); KiIrqRestore(flags); W32kEnvironmentError(8); return 0; }
        hook->Window = hwnd;
        hook->Next = desktop->Hooks;
        desktop->Hooks = hook;
        KiIrqRestore(flags); return 1;
    }
    if (operation == SV_SHELL_DEREGISTER_HOOK) {
        W32K_SHELL_HOOK **link = &desktop->Hooks;
        while (*link && (*link)->Window != hwnd) link = &(*link)->Next;
        if (!*link) { KiIrqRestore(flags); return 0; }
        W32K_SHELL_HOOK *hook = *link;
        *link = hook->Next;
        ExFreePool(hook);
        KiIrqRestore(flags); return 1;
    }
    KiIrqRestore(flags);
    W32kEnvironmentError(87);
    return 0;
}

static BOOLEAN W32kCaptureEnvironmentString(UINT64 address, WCHAR *text, UINT32 capacity)
{
    for (UINT32 i = 0; i < capacity; ++i) {
        UINT64 source = address + (UINT64)i * sizeof(WCHAR);
        if (source < address || !MmProbeForRead(source, sizeof(WCHAR))) return FALSE;
        text[i] = *(const WCHAR *)source;
        if (!text[i]) return TRUE;
    }
    return FALSE;
}

static UINT64 SvNtUserWindowLookup(UINT64 *a)
{
    UINT64 flags = KiIrqSave();
    if (a[0] == 2) {
        if (!KiUserWindowValid(a[1], FALSE) || !W32kWindowOnCurrentDesktop((UINT16)a[1])) {
            KiIrqRestore(flags); W32kEnvironmentError(1400); return 0;
        }
        UINT64 value = (INT32)a[2] == -20 ? g_w32k[(UINT16)a[1]].ExStyle :
                       (INT32)a[2] == -16 ? W32kWindowStyle((UINT16)a[1]) : 0;
        KiIrqRestore(flags); return value;
    }
    if (a[0] == 1) {
        if (!KiUserWindowValid(a[1], FALSE) || !W32kWindowOnCurrentDesktop((UINT16)a[1]) ||
            !a[2] || !a[3] || a[3] > 0x7fffffff ||
            !MmProbeForWrite(a[2], a[3] * sizeof(WCHAR))) {
            KiIrqRestore(flags); W32kEnvironmentError(1400); return 0;
        }
        USER_CLASS_LOCAL *cls = g_window_classes[(UINT16)a[1]];
        WCHAR *out = (WCHAR *)a[2];
        UINT32 copied = 0;
        if (cls) while (copied + 1 < a[3] && cls->Name[copied]) {
            out[copied] = cls->Name[copied]; copied++;
        }
        out[copied] = 0;
        KiIrqRestore(flags); return copied;
    }
    UINT64 parent = a[1] == SV_DESKTOP_HWND ? 0 : a[1], after = a[2];
    if ((parent && (!KiUserWindowValid(parent, FALSE) || !W32kWindowOnCurrentDesktop((UINT16)parent))) ||
        (after && (!KiUserWindowValid(after, FALSE) || !W32kWindowOnCurrentDesktop((UINT16)after)))) {
        KiIrqRestore(flags); W32kEnvironmentError(1400); return 0;
    }
    WCHAR class_name[32], title[W32K_TITLE_CHARS];
    UINT16 class_atom = 0;
    if (a[3] && a[3] <= 0xffff) class_atom = (UINT16)a[3];
    else if (a[3] && !W32kCaptureEnvironmentString(a[3], class_name, 32)) {
        KiIrqRestore(flags); W32kEnvironmentError(87); return 0;
    }
    if (a[4] && !W32kCaptureEnvironmentString(a[4], title, W32K_TITLE_CHARS)) {
        KiIrqRestore(flags); W32kEnvironmentError(87); return 0;
    }
    BOOLEAN past_after = !after;
    for (UINT16 slot = g_w32k_top; slot; slot = g_w32k[slot].ZPrev) {
        if (!g_window_owner[slot] || g_w32k[slot].Parent != (UINT16)parent ||
            !W32kWindowOnCurrentDesktop(slot)) continue;
        UINT64 hwnd = KiUserWindowHandle(slot);
        if (!past_after) { if (hwnd == after) past_after = TRUE; continue; }
        USER_CLASS_LOCAL *cls = g_window_classes[slot];
        if (a[3] && (!cls || (class_atom ? cls->Atom != class_atom : !KiWideEqual(cls->Name, class_name))))
            continue;
        if (a[4] && !KiWideEqual(g_w32k[slot].Title, title)) continue;
        KiIrqRestore(flags); return hwnd;
    }
    KiIrqRestore(flags); return 0;
}
