/* Desktop-scoped hotkeys and physical keyboard state. Registrations are real
 * thread/window resources, removed before their owner can be reclaimed. */
typedef struct _W32K_HOTKEY {
    struct _W32K_HOTKEY *Next;
    PVOID Desktop;
    PKTHREAD Thread;
    UINT64 Window;
    INT32 Id;
    UINT32 Modifiers;
    UINT8 Key;
    BOOLEAN NoRepeat;
} W32K_HOTKEY;
static W32K_HOTKEY *g_hotkeys;
static BOOLEAN g_win_key_used;

static void W32kPhysicalKey(UINT8 key, UINT8 side, BOOLEAN release)
{
    BOOLEAN down = !!(g_physical_keys[side] & 0x80);
    if (!release && !down) {
        g_recent_keys[side] = g_recent_keys[key] = 1;
        if (key == 0x14 || key == 0x90 || key == 0x91) g_physical_keys[key] ^= 1;
    }
    g_physical_keys[side] = (g_physical_keys[side] & 1) | (release ? 0 : 0x80);
    if (side != key) {
        UINT8 other = side ^ 1;
        g_physical_keys[key] = (g_physical_keys[key] & 1) |
            ((g_physical_keys[side] | g_physical_keys[other]) & 0x80);
    }
}

static void W32kPhysicalMouse(UINT8 buttons)
{
    const UINT8 keys[] = {1, 2, 4};
    for (UINT32 i = 0; i < 3; ++i) {
        BOOLEAN down = !!(buttons & (1u << i));
        if (down && !(g_physical_keys[keys[i]] & 0x80)) g_recent_keys[keys[i]] = 1;
        g_physical_keys[keys[i]] = down ? 0x80 : 0;
    }
    if (buttons && ((g_physical_keys[0x5b] | g_physical_keys[0x5c]) & 0x80)) g_win_key_used = TRUE;
}

static void W32kSyncKeyState(UINT16 slot)
{
    USER_MSG_QUEUE *queue = slot ? KiUserQueueForThread(g_window_owner[slot]) : NULL;
    if (queue) memcpy(queue->KeyState, g_physical_keys, sizeof(queue->KeyState));
}

static UINT64 NtUserRegisterHotKey(UINT64 *a)
{
    PKTHREAD caller = KeGetCurrentThread();
    UINT32 modifiers = (UINT32)a[2], key = (UINT32)a[3];
    if (!caller || !caller->Process || (modifiers & ~0x400fu) || !key || key > 255) {
        W32kEnvironmentError(87); return 0;
    }
    if (a[0] && (!KiUserWindowValid(a[0], TRUE) || g_window_owner[(UINT16)a[0]] != caller->ThreadId ||
                   !W32kWindowOnCurrentDesktop((UINT16)a[0]))) {
        W32kEnvironmentError(1408); return 0;
    }
    if (!KiUserQueueForThread(caller->ThreadId)) { W32kEnvironmentError(8); return 0; }
    UINT64 flags = KiIrqSave();
    PVOID desktop = W32kGetCurrentDesktopIdentity();
    for (W32K_HOTKEY *hotkey = g_hotkeys; hotkey; hotkey = hotkey->Next)
        if (hotkey->Desktop == desktop && hotkey->Modifiers == (modifiers & 15) && hotkey->Key == key) {
            KiIrqRestore(flags); W32kEnvironmentError(1409); return 0;
        }
    W32K_HOTKEY *hotkey = ExAllocatePoolWithTag(NonPagedPool, sizeof(*hotkey), 'ytoH');
    if (!hotkey) { KiIrqRestore(flags); W32kEnvironmentError(8); return 0; }
    *hotkey = (W32K_HOTKEY){.Next=g_hotkeys, .Desktop=desktop, .Thread=caller,
        .Window=a[0], .Id=(INT32)a[1], .Modifiers=modifiers & 15, .Key=(UINT8)key,
        .NoRepeat=!!(modifiers & 0x4000)};
    g_hotkeys = hotkey;
    KiIrqRestore(flags); return 1;
}

static UINT64 SvNtUserHotkeyControl(UINT64 *a)
{
    UINT64 flags = KiIrqSave();
    if (a[0] == 1) {
        UINT32 key = (UINT32)a[1];
        if (key > 255 || W32kGetCurrentDesktopIdentity() != W32kGetInputDesktopIdentity()) {
            KiIrqRestore(flags); return 0;
        }
        UINT16 value = (g_physical_keys[key] & 0x80 ? 0x8000u : 0) | g_recent_keys[key];
        g_recent_keys[key] = 0;
        KiIrqRestore(flags); return value;
    }
    PKTHREAD caller = KeGetCurrentThread();
    W32K_HOTKEY **link = &g_hotkeys;
    BOOLEAN removed = FALSE;
    while (*link) {
        W32K_HOTKEY *hotkey = *link;
        if (hotkey->Thread == caller && hotkey->Window == a[1] && hotkey->Id == (INT32)a[2]) {
            *link = hotkey->Next; ExFreePool(hotkey); removed = TRUE;
        } else link = &hotkey->Next;
    }
    KiIrqRestore(flags);
    if (!removed) W32kEnvironmentError(1419);
    return removed;
}

static BOOLEAN W32kDispatchHotkey(UINT8 key, BOOLEAN release, BOOLEAN repeat)
{
    BOOLEAN win = !!((g_physical_keys[0x5b] | g_physical_keys[0x5c]) & 0x80);
    if (!release && (key == 0x5b || key == 0x5c) && !repeat) g_win_key_used = FALSE;
    else if (!release && win && key != 0x5b && key != 0x5c) g_win_key_used = TRUE;
    if (release && (key == 0x5b || key == 0x5c) && !win && !g_win_key_used) {
        W32K_SHELL_DESKTOP *desktop = W32kShellDesktop(W32kGetInputDesktopIdentity(), FALSE);
        if (desktop && KiUserWindowValid(desktop->ShellWindow, FALSE))
            KiUserEnqueueMessagePrivate(desktop->ShellWindow, W32K_WM_SYSCOMMAND,
                0xf130, 0, 0, 0, 0, TRUE); /* SC_TASKLIST */
    }
    if (release) return FALSE;
    UINT32 modifiers = (g_physical_keys[0x12] & 0x80 ? 1 : 0) |
        (g_physical_keys[0x11] & 0x80 ? 2 : 0) |
        (g_physical_keys[0x10] & 0x80 ? 4 : 0) | (win ? 8 : 0);
    for (W32K_HOTKEY *hotkey = g_hotkeys; hotkey; hotkey = hotkey->Next)
        if (hotkey->Desktop == W32kGetInputDesktopIdentity() && hotkey->Key == key &&
            hotkey->Modifiers == modifiers) {
            if (repeat && hotkey->NoRepeat) return TRUE;
            INT64 param = (INT64)(modifiers | ((UINT32)key << 16));
            if (hotkey->Window)
                KiUserEnqueueMessagePrivate(hotkey->Window, 0x0312, (UINT64)(INT64)hotkey->Id,
                    param, 0, 0, 0, TRUE);
            else KiUserEnqueueThreadMessage(hotkey->Thread->ThreadId, 0x0312,
                                            (UINT64)(INT64)hotkey->Id, param);
            return TRUE;
        }
    return FALSE;
}

static void W32kHotkeyCleanupThread(PKTHREAD thread)
{
    W32K_HOTKEY **link = &g_hotkeys;
    while (*link) {
        W32K_HOTKEY *hotkey = *link;
        if (hotkey->Thread == thread) { *link = hotkey->Next; ExFreePool(hotkey); }
        else link = &hotkey->Next;
    }
}

static void W32kHotkeyWindowDestroyed(UINT64 window)
{
    W32K_HOTKEY **link = &g_hotkeys;
    while (*link) {
        W32K_HOTKEY *hotkey = *link;
        if (hotkey->Window == window) { *link = hotkey->Next; ExFreePool(hotkey); }
        else link = &hotkey->Next;
    }
}
