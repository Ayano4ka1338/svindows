/* USER message queues, update regions and atom services.
 * Included by the owning translation unit. */
/* ------------------------------------------------------------------ */
/* Minimal win32k/USER services used by the genuine win32u.dll.        */
/* ------------------------------------------------------------------ */

/* Offset of USER32's `gpsi` global (the SERVERINFO pointer) in the build in
 * win/: read out of GetSysColor, which is `mov gpsi,%rax; mov
 * 0x1360(%rax,%rdx,4),%eax`. Diagnostics only. */
#define USER32_GPSI_OFFSET 0xBC8D8

typedef struct _USER_MSG_LOCAL {
    UINT64 Hwnd;
    UINT32 Message;
    UINT32 Padding;
    UINT64 WParam;
    INT64  LParam;
    UINT32 Time;
    INT32  PtX;
    INT32  PtY;
    UINT32 Private;
} USER_MSG_LOCAL;

#define USER_MESSAGE_QUEUE_CAPACITY 64
#define WM_CHAR        0x0102
#define WM_MOUSEMOVE   0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP   0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP   0x0205

/*
 * Per-thread posted-message queues. Real USER owns a queue per GUI thread;
 * cross-thread hand-offs (explorer's desktop bootstrap posts to the creating
 * thread) only work if the posted message cannot be consumed by another
 * thread's GetMessage. Threads are few, so queues are keyed by thread id.
 */
#define USER_MESSAGE_QUEUE_CAPACITY 64
#define USER_MAX_THREAD_QUEUES 64

typedef struct _USER_MSG_QUEUE {
    USER_MSG_LOCAL Msgs[USER_MESSAGE_QUEUE_CAPACITY];
    UINT32 Head;
    UINT32 Tail;
    /* The thread's USER "input event": a notification event ring 3 waits on
     * through MsgWaitForMultipleObjects. Created lazily the first time the
     * thread asks for it, signaled while the queue is non-empty. */
    PKEVENT InputEvent;
    HANDLE  InputEventHandle;
} USER_MSG_QUEUE;

static USER_MSG_QUEUE g_user_thread_queues[USER_MAX_THREAD_QUEUES];

static USER_MSG_QUEUE *KiUserQueueForThread(UINT32 thread_id)
{
    if (!thread_id || thread_id > USER_MAX_THREAD_QUEUES)
        return NULL;
    return &g_user_thread_queues[thread_id - 1];
}

/* Keep a thread's input event in step with its queue. Called with the queue
 * already updated; safe from interrupt context (KeSetEvent masks interrupts
 * and only readies threads). A pending update region counts as work: the
 * WM_PAINT it produces is synthesized by the message loop, not queued, so the
 * event has to fire for it too or the loop sleeps through the repaint. */
static UINT16 KiUserNextUpdateWindow(UINT32 thread_id);

static void KiUserSyncInputEvent(USER_MSG_QUEUE *queue)
{
    if (!queue->InputEvent)
        return;
    UINT32 tid = (UINT32)(queue - g_user_thread_queues) + 1;
    if (queue->Head != queue->Tail || KiUserNextUpdateWindow(tid))
        KeSetEvent(queue->InputEvent);
    else
        KeResetEvent(queue->InputEvent);
}

/* Owner thread of each created window slot: input goes to the queue of the
 * thread that created the window, as in win32k. */
static UINT32 g_window_owner[128];

static UINT32 KiUserWindowOwner(UINT64 hwnd)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!slot || slot >= 128)
        return 0;
    return g_window_owner[slot];
}

/*
 * Update regions and WM_PAINT.
 *
 * win32k never queues WM_PAINT: a window carries an update region, and the
 * owning thread's message loop synthesizes the message for as long as that
 * region is non-empty. The same model here, with the region simplified to
 * "the whole window": g_window_update[slot] counts the WM_PAINTs still owed,
 * BeginPaint/ValidateRect clear it, and the count bounds redelivery so a
 * client that ignores the message cannot spin its loop forever.
 */
#define USER_PAINT_SLOTS 128
#define USER_PAINT_REDELIVERY 4
#define WM_PAINT_ 0x000F

static UINT8 g_window_update[USER_PAINT_SLOTS];

/* First window owned by `tid` that still owes a WM_PAINT, in creation order
 * (parents before children, which is the order win32k paints in). */
static UINT16 KiUserNextUpdateWindow(UINT32 tid)
{
    if (!tid)
        return 0;
    for (UINT16 slot = 1; slot < USER_PAINT_SLOTS; slot++)
        if (g_window_update[slot] && g_window_owner[slot] == tid)
            return slot;
    return 0;
}

static BOOLEAN KiUserEnqueueMessage(UINT64 hwnd, UINT32 message,
                                    UINT64 wparam, INT64 lparam,
                                    INT32 x, INT32 y)
{
    /* A posted message is delivered to its window's owner thread; a thread
     * message (hwnd == 0) goes to the posting thread's own target queue via
     * KiUserEnqueueThreadMessage. */
    UINT32 tid = hwnd ? KiUserWindowOwner(hwnd) : 0;
    if (!tid) {
        PKTHREAD thread = KeGetCurrentThread();
        tid = thread ? thread->ThreadId : 0;
    }
    USER_MSG_QUEUE *queue = KiUserQueueForThread(tid);
    if (!queue)
        return FALSE;
    UINT64 flags = KiIrqSave();
    UINT32 next = (queue->Head + 1) % USER_MESSAGE_QUEUE_CAPACITY;
    if (next == queue->Tail) {
        KiIrqRestore(flags);
        return FALSE;
    }

    USER_MSG_LOCAL *msg = &queue->Msgs[queue->Head];
    memset(msg, 0, sizeof(*msg));
    msg->Hwnd = hwnd;
    msg->Message = message;
    msg->WParam = wparam;
    msg->LParam = lparam;
    msg->Time = (UINT32)(KeGetTickCount() * 10);
    msg->PtX = x;
    msg->PtY = y;
    queue->Head = next;
    KiUserSyncInputEvent(queue);
    KiIrqRestore(flags);
    return TRUE;
}

static BOOLEAN KiUserEnqueueThreadMessage(UINT32 target_tid, UINT32 message,
                                          UINT64 wparam, INT64 lparam)
{
    USER_MSG_QUEUE *queue = KiUserQueueForThread(target_tid);
    if (!queue)
        return FALSE;
    UINT64 flags = KiIrqSave();
    UINT32 next = (queue->Head + 1) % USER_MESSAGE_QUEUE_CAPACITY;
    if (next == queue->Tail) {
        KiIrqRestore(flags);
        return FALSE;
    }
    USER_MSG_LOCAL *msg = &queue->Msgs[queue->Head];
    memset(msg, 0, sizeof(*msg));
    msg->Message = message;
    msg->WParam = wparam;
    msg->LParam = lparam;
    msg->Time = (UINT32)(KeGetTickCount() * 10);
    queue->Head = next;
    KiUserSyncInputEvent(queue);
    KiIrqRestore(flags);
    return TRUE;
}

static UINT64 g_user_active_hwnd;
static UINT8 g_user_mouse_buttons;

void KiUserQueueCharacter(UINT16 character)
{
    if (!g_user_active_hwnd)
        return;
    KiUserEnqueueMessage(g_user_active_hwnd, WM_CHAR, character, 1, 0, 0);
}

void KiUserQueueMouse(INT32 x, INT32 y, UINT8 buttons)
{
    if (!g_user_active_hwnd) {
        g_user_mouse_buttons = buttons;
        return;
    }
    UINT64 wparam = 0;
    if (buttons & 1) wparam |= 0x0001; /* MK_LBUTTON */
    if (buttons & 2) wparam |= 0x0002; /* MK_RBUTTON */
    INT64 lparam = (INT64)(UINT32)(((UINT16)y << 16) | (UINT16)x);

    KiUserEnqueueMessage(g_user_active_hwnd, WM_MOUSEMOVE, wparam, lparam, x, y);
    if ((buttons ^ g_user_mouse_buttons) & 1)
        KiUserEnqueueMessage(g_user_active_hwnd,
                             (buttons & 1) ? WM_LBUTTONDOWN : WM_LBUTTONUP,
                             wparam, lparam, x, y);
    if ((buttons ^ g_user_mouse_buttons) & 2)
        KiUserEnqueueMessage(g_user_active_hwnd,
                             (buttons & 2) ? WM_RBUTTONDOWN : WM_RBUTTONUP,
                             wparam, lparam, x, y);
    g_user_mouse_buttons = buttons;
}

/* Mark a window as needing repaint and wake its owner thread's message loop. */
static void KiUserInvalidateWindow(UINT16 slot)
{
    if (!slot || slot >= USER_PAINT_SLOTS)
        return;
    UINT64 flags = KiIrqSave();
    g_window_update[slot] = USER_PAINT_REDELIVERY;
    USER_MSG_QUEUE *queue = KiUserQueueForThread(g_window_owner[slot]);
    if (queue)
        KiUserSyncInputEvent(queue);
    KiIrqRestore(flags);
}

/* Drop a window's update region: it has been painted (or explicitly
 * validated), so no further WM_PAINT is owed. */
static void KiUserValidateWindow(UINT16 slot)
{
    if (!slot || slot >= USER_PAINT_SLOTS)
        return;
    UINT64 flags = KiIrqSave();
    g_window_update[slot] = 0;
    USER_MSG_QUEUE *queue = KiUserQueueForThread(g_window_owner[slot]);
    if (queue)
        KiUserSyncInputEvent(queue);
    KiIrqRestore(flags);
}

static BOOLEAN KiUserTakeMessage(USER_MSG_LOCAL *out, BOOLEAN remove)
{
    PKTHREAD thread = KeGetCurrentThread();
    USER_MSG_QUEUE *queue =
        KiUserQueueForThread(thread ? thread->ThreadId : 0);
    if (!queue)
        return FALSE;
    UINT64 flags = KiIrqSave();
    if (queue->Tail == queue->Head) {
        /* Nothing posted. An invalid window becomes WM_PAINT right here, which
         * is where win32k generates it: paint messages live in the window's
         * update region, never in the queue, so they are always produced last
         * and always after everything that was actually posted. */
        UINT16 slot = KiUserNextUpdateWindow(thread ? thread->ThreadId : 0);
        if (!slot) {
            KiIrqRestore(flags);
            return FALSE;
        }
        memset(out, 0, sizeof(*out));
        out->Hwnd = 0x10000u | slot;
        out->Message = WM_PAINT_;
        out->Time = (UINT32)(KeGetTickCount() * 10);
        /* Windows keeps producing WM_PAINT until the window is validated.
         * Count every delivery, not just the removing ones, so a client that
         * neither paints nor validates cannot livelock its own message loop
         * on a window we hand it forever. */
        if (g_window_update[slot] && !--g_window_update[slot]) {
            KeLog("[user] WM_PAINT for HWND %p went unpainted; validating the "
                  "window to keep the message loop live\n", (void *)out->Hwnd);
            KiUserSyncInputEvent(queue);
        }
        KiIrqRestore(flags);
        return TRUE;
    }
    *out = queue->Msgs[queue->Tail];
    if (remove) {
        queue->Tail = (queue->Tail + 1) % USER_MESSAGE_QUEUE_CAPACITY;
        KiUserSyncInputEvent(queue);
    }
    KiIrqRestore(flags);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* The USER atom table                                                */
/* ------------------------------------------------------------------ */
/*
 * Window classes, registered window messages and GlobalAddAtom-style names all
 * draw from one 0xC000..0xFFFF atom space per window station, exactly as in
 * win32k: RegisterClassEx interns the class name, RegisterWindowMessage interns
 * the message name (so two components asking for the same string get the same
 * id), and GetAtomName maps an atom back to its string. Keeping them in
 * separate counters -- which is what this file did -- hands the same number to
 * a class and a message, and leaves GetAtomName unable to answer at all, which
 * is what stopped the shell from creating its worker window.
 */
#define USER_MAX_ATOMS   192
#define USER_ATOM_CHARS  64
#define USER_ATOM_FIRST  0xC000

typedef struct _USER_ATOM_ENTRY {
    WCHAR  Name[USER_ATOM_CHARS];
    UINT16 Atom;
} USER_ATOM_ENTRY;

static USER_ATOM_ENTRY g_user_atoms[USER_MAX_ATOMS];
static UINT32 g_user_atom_count;

/* Copy a NUL-terminated wide string from user memory (bounded). */
static UINT32 KiCopyWideFromUser(const WCHAR *src, WCHAR *dst, UINT32 cap)
{
    if (!src || !MmProbeForRead((UINT64)src, sizeof(WCHAR)))
        return 0;
    for (UINT32 i = 0; i + 1 < cap; i++) {
        if (!MmProbeForRead((UINT64)(src + i), sizeof(WCHAR))) {
            dst[i] = 0;
            return i;
        }
        dst[i] = src[i];
        if (!src[i])
            return i;
    }
    dst[cap - 1] = 0;
    return cap - 1;
}

static BOOLEAN KiWideEqual(const WCHAR *a, const WCHAR *b)
{
    while (*a && *b) {
        WCHAR ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return FALSE;
    }
    return *a == *b;
}

/* Intern `name`, returning its (stable) atom. 0 when the table is full. */
static UINT16 KiUserAddAtom(const WCHAR *name)
{
    if (!name || !name[0])
        return 0;
    for (UINT32 i = 0; i < g_user_atom_count; i++)
        if (KiWideEqual(g_user_atoms[i].Name, name))
            return g_user_atoms[i].Atom;
    if (g_user_atom_count >= USER_MAX_ATOMS)
        return 0;
    USER_ATOM_ENTRY *entry = &g_user_atoms[g_user_atom_count];
    UINT32 n = 0;
    for (; n + 1 < USER_ATOM_CHARS && name[n]; n++)
        entry->Name[n] = name[n];
    entry->Name[n] = 0;
    entry->Atom = (UINT16)(USER_ATOM_FIRST + g_user_atom_count);
    g_user_atom_count++;
    return entry->Atom;
}

static const WCHAR *KiUserAtomName(UINT16 atom)
{
    for (UINT32 i = 0; i < g_user_atom_count; i++)
        if (g_user_atoms[i].Atom == atom)
            return g_user_atoms[i].Name;
    return NULL;
}

/* Capture the atom name a win32u service was handed: modern win32u passes a
 * counted UNICODE_STRING, older forms a bare pointer. */
static UINT32 KiCaptureAtomName(UINT64 arg, WCHAR *out, UINT32 cap)
{
    if (!arg)
        return 0;
    /* A UNICODE_STRING starts with two USHORTs whose values are small and
     * whose Buffer is a valid pointer; a bare string starts with characters. */
    if (MmProbeForRead(arg, sizeof(UNICODE_STRING))) {
        const UNICODE_STRING *ustr = (const UNICODE_STRING *)arg;
        UINT32 chars = ustr->Length / sizeof(WCHAR);
        if (chars && chars < cap && ustr->MaximumLength >= ustr->Length &&
            ustr->Buffer &&
            MmProbeForRead((UINT64)ustr->Buffer,
                           (UINT64)chars * sizeof(WCHAR))) {
            memcpy(out, ustr->Buffer, (UINT64)chars * sizeof(WCHAR));
            out[chars] = 0;
            return chars;
        }
    }
    return KiCopyWideFromUser((const WCHAR *)arg, out, cap);
}

static UINT64 NtUserGetThreadState(UINT64 *a)
{
    /* No USER message queue/window state exists yet.  A zero state is the
     * correct empty-subsystem answer and, unlike STATUS_NOT_IMPLEMENTED, is a
     * valid value for every query currently made during process startup. */
    return 0;
}

static UINT64 NtUserRegisterWindowMessage(UINT64 *a)
{
    /* NtUserRegisterWindowMessage(PUNICODE_STRING MessageName): the id must be
     * the same for every caller that asks for the same string, which is what
     * makes cross-process shell messages (Shell_TrayWnd's "TaskbarCreated" and
     * friends) work at all. Interning gives that for free. */
    WCHAR name[USER_ATOM_CHARS];
    UINT32 n = KiCaptureAtomName(a[0], name, USER_ATOM_CHARS);
    if (!n)
        return 0;
    UINT16 atom = KiUserAddAtom(name);
    char ascii[USER_ATOM_CHARS];
    UINT32 j = 0;
    for (; j < n && j + 1 < sizeof(ascii); j++)
        ascii[j] = (char)name[j];
    ascii[j] = 0;
    KeLog("[user] RegisterWindowMessage('%s') -> 0x%x\n", ascii, atom);
    return atom;
}

static UINT64 NtUserSystemParametersInfo(UINT64 *a)
{
    KeLog("[user] NtUserSystemParametersInfo(action=0x%lx, param=%lu, out=%p, flags=0x%lx)\n",
          (unsigned long)a[0], (unsigned long)a[1], (void *)a[2],
          (unsigned long)a[3]);
    /* SPI_GETDRAGFULLWINDOWS (0x26) is Explorer's first query. Supply the
     * disabled policy value through pvParam. This is a genuine empty USER
     * policy state, not framebuffer desktop emulation. */
    if ((UINT32)a[0] == 0x26 &&
        MmProbeForWrite(a[2], sizeof(UINT32))) {
        *(UINT32 *)a[2] = 0;
        return 1;
    }

    /* SPI_GETCLIENTAREAANIMATION (0x1042). Explorer's SHCore worker asks for
     * this after its startup hand-off. We currently have no compositor
     * animations, so publish the disabled BOOL as a successful query. */
    if ((UINT32)a[0] == 0x1042 &&
        MmProbeForWrite(a[2], sizeof(UINT32))) {
        *(UINT32 *)a[2] = 0;
        return 1;
    }

    /* SPI_GETHIGHCONTRAST (0x42), HIGHCONTRASTW is 16 bytes on x64.
     * Accessibility high-contrast mode is disabled and no scheme is active. */
    if ((UINT32)a[0] == 0x42 && a[1] >= 16 &&
        MmProbeForWrite(a[2], 16)) {
        UINT32 *hc = (UINT32 *)a[2];
        hc[0] = 16; /* cbSize */
        hc[1] = 0;  /* dwFlags */
        *(UINT64 *)((UINT8 *)hc + 8) = 0; /* lpszDefaultScheme */
        return 1;
    }

    /* Report other settings as unavailable until the USER system metrics and
     * settings block is backed by win32k. This is a Win32 BOOL, not an
     * NTSTATUS. */
    return 0;
}

static UINT64 NtUserFindWindowEx(UINT64 *a)
{
    /* Empty window tree: no matching HWND. */
    return 0;
}

static UINT64 NtUserEnableMouseInPointer(UINT64 *a)
{
    /* Input is already delivered by the kernel PS/2 path. Accepting the mode
     * switch lets USER32 complete initialization without inventing windows. */
    return 1;
}

static UINT64 NtUserSetProcessUIAccessZorder(UINT64 *a)
{
    /* The single interactive process owns the only desktop and therefore has
     * no cross-integrity z-order boundary to enforce. */
    return 1;
}

static UINT64 NtUserPeekMessage(UINT64 *a)
{
    USER_MSG_LOCAL *user_msg = (USER_MSG_LOCAL *)a[0];
    USER_MSG_LOCAL msg;
    BOOLEAN remove = ((UINT32)a[4] & 1) != 0; /* PM_REMOVE */
    if (!user_msg || !MmProbeForWrite((UINT64)user_msg, sizeof(*user_msg)))
        return 0;
    if (!KiUserTakeMessage(&msg, remove))
        return 0;
    *user_msg = msg;
    return 1;
}

static UINT64 NtUserPostThreadMessage(UINT64 *a)
{
    UINT32 thread_id = (UINT32)a[0];
    UINT32 message = (UINT32)a[1];
    const char *mod = 0;
    UINT64 base = 0, ret = 0;
    if (MmProbeForRead(g_service_user_rsp, sizeof(UINT64)))
        ret = *(volatile UINT64 *)g_service_user_rsp;
    if (ret && LdrDescribeUserAddress(ret, &mod, &base))
        KeLog("[user] NtUserPostThreadMessage(tid=%u, msg=0x%x, wp=%p, lp=%p) from %s+0x%lx\n",
              thread_id, message, (void *)a[2], (void *)a[3], mod,
              (unsigned long)(ret - base));
    else
        KeLog("[user] NtUserPostThreadMessage(tid=%u, msg=0x%x, wp=%p, lp=%p)\n",
              thread_id, message, (void *)a[2], (void *)a[3]);

    /* The message is delivered to the target thread's own queue; only that
     * thread's GetMessage/PeekMessage can consume it, preserving per-thread
     * message semantics for explorer's desktop hand-off. */
    return KiUserEnqueueThreadMessage(thread_id, message, a[2],
                                      (INT64)a[3]);
}

/* NtUserCallOneParam routine indices used by the shell (the win32u
 * apfnSimpleCall table of the supplied build). */
#define ONEPARAM_GETINPUTEVENT 0x32

/*
 * The thread's USER input event. user32's MsgWaitForMultipleObjectsEx checks
 * the client-side wake bits first and, when nothing is pending, asks win32k
 * for the handle of the event that fires when a message matching the wake mask
 * arrives, then waits on it together with the caller's handles. Returning NULL
 * (the old stub) makes that function fail, and the shell retries forever.
 *
 * The wake mask is packed into the argument as (flags << 16) | mask; the
 * per-thread queue is not filtered by message class yet, so any queued message
 * wakes the wait -- a superset, which a message loop handles correctly because
 * it re-checks its own queue after waking.
 */
static UINT64 KiUserGetInputEvent(void)
{
    PKTHREAD thread = KeGetCurrentThread();
    USER_MSG_QUEUE *queue =
        KiUserQueueForThread(thread ? thread->ThreadId : 0);
    if (!queue)
        return 0;
    if (!queue->InputEvent) {
        PKEVENT event = NULL;
        HANDLE handle = NULL;
        if (!NT_SUCCESS(PsCreateNotificationEvent(&event, &handle)))
            return 0;
        queue->InputEvent = event;
        queue->InputEventHandle = handle;
        KeLog("[user] input event for thread %lu -> handle %p\n",
              (unsigned long)(thread ? thread->ThreadId : 0), (void *)handle);
    }
    /* Publish the current queue state before the caller waits, so a message
     * that arrived before this call is not missed. */
    UINT64 flags = KiIrqSave();
    KiUserSyncInputEvent(queue);
    KiIrqRestore(flags);
    return (UINT64)queue->InputEventHandle;
}

/* NtUserCallNoParam(routine): the routine index is the only argument, so it
 * cannot share NtUserCallOneParam's handler (which reads it from a[1]). */
static UINT64 NtUserCallNoParam(UINT64 *a)
{
    static UINT8 seen[256];
    UINT64 routine = a[0];
    if (routine >= 256 || !seen[routine]) {
        if (routine < 256)
            seen[routine] = 1;
        KeLog("[user] NtUserCallNoParam(routine=0x%lx) -> 0\n",
              (unsigned long)routine);
    }
    return 0;
}

static UINT64 NtUserCallOneParam(UINT64 *a)
{
    UINT64 routine = a[1];

    if (routine == ONEPARAM_GETINPUTEVENT)
        return KiUserGetInputEvent();

    /* Log each unimplemented routine once: repeated calls come from polling
     * loops and would bury the rest of the log. */
    static UINT8 seen[256];
    if (routine >= 256 || !seen[routine]) {
        if (routine < 256)
            seen[routine] = 1;
        KeLog("[user] NtUserCallOneParam(value=%p, routine=0x%lx) -> 0\n",
              (void *)a[0], (unsigned long)routine);
    }
    return 0;
}

static UINT64 NtUserGetCaretBlinkTime(UINT64 *a)
{
    /* Conventional Windows default in milliseconds. This is a scalar query;
     * no caret object needs to exist yet. */
    return 530;
}

static UINT64 NtUserGetAtomName(UINT64 *a)
{
    /* (atom, PUNICODE_STRING out) in the modern form; the buffer/capacity pair
     * is the older one. The shell resolves its own class atom back to a name
     * before creating the worker window, so answering with an empty string
     * (what this used to do) silently loses that window. */
    UINT16 atom = (UINT16)a[0];
    const WCHAR *name = KiUserAtomName(atom);
    if (!name) {
        KeLog("[user] GetAtomName(0x%x) -> unknown\n", atom);
        return 0;
    }
    UINT32 chars = 0;
    while (name[chars])
        chars++;

    WCHAR *buffer = (WCHAR *)a[1];
    UINT32 capacity = (UINT32)a[2];
    /* UNICODE_STRING form: a[1] points at { Length, MaximumLength, Buffer }
     * and the count comes from MaximumLength. */
    if (buffer && !capacity && MmProbeForRead(a[1], sizeof(UNICODE_STRING))) {
        UNICODE_STRING *ustr = (UNICODE_STRING *)a[1];
        if (ustr->Buffer && ustr->MaximumLength >= sizeof(WCHAR) &&
            MmProbeForWrite(a[1], sizeof(UNICODE_STRING))) {
            UINT32 room = ustr->MaximumLength / sizeof(WCHAR);
            UINT32 n = chars + 1 > room ? room - 1 : chars;
            if (!MmProbeForWrite((UINT64)ustr->Buffer,
                                 (UINT64)(n + 1) * sizeof(WCHAR)))
                return 0;
            for (UINT32 i = 0; i < n; i++)
                ustr->Buffer[i] = name[i];
            ustr->Buffer[n] = 0;
            ustr->Length = (UINT16)(n * sizeof(WCHAR));
            return n;
        }
    }

    if (!buffer || !capacity)
        return 0;
    UINT32 n = chars + 1 > capacity ? capacity - 1 : chars;
    if (!MmProbeForWrite((UINT64)buffer, (UINT64)(n + 1) * sizeof(WCHAR)))
        return 0;
    for (UINT32 i = 0; i < n; i++)
        buffer[i] = name[i];
    buffer[n] = 0;
    return n;
}

