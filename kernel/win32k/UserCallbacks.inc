/* USER objects and the kernel-to-user callback dispatcher.
 * Included by the owning translation unit. */
/* ------------------------------------------------------------------ */
/* USER objects other than windows                                     */
/*                                                                     */
/* USER32 validates every handle it is given against the shared         */
/* HANDLEENTRY table, so a cursor or an accelerator table cannot be a   */
/* fabricated number: it needs a real entry with the right type byte and */
/* an object head in the arena. Types follow the native ordering        */
/* (TYPE_WINDOW 1, TYPE_MENU 2, TYPE_CURSOR 3, ..., TYPE_ACCELTABLE 8). */
/* ------------------------------------------------------------------ */

#define USER_TYPE_CURSOR 3
#define USER_TYPE_ACCEL  8

#define USER_OBJECT_SLOTS \
    (PROCESS_USER_OBJECT_ARENA_SIZE / 0x200)
_Static_assert(USER_OBJECT_SLOTS <= PROCESS_USER_HANDLE_COUNT,
               "USER table commit does not cover the object arena");

typedef struct _USER_OBJECT_OWNER {
    PKPROCESS Process;
    UINT16 Generation;
    UINT8 Type;
    BOOLEAN Icon, SharedIcon;
} USER_OBJECT_OWNER;
static USER_OBJECT_OWNER g_user_objects[USER_OBJECT_SLOTS];

static ULONG KiUserCountProcessObjects(PKPROCESS process)
{
    ULONG count = 0;
    for (UINT16 i = 200; process && i < USER_OBJECT_SLOTS; ++i)
        if (g_user_objects[i].Process == process)
            count++;
    return count;
}

static UINT64 KiUserAllocateObject(UINT8 type, UINT64 *head_out)
{
    PKTHREAD caller = KeGetCurrentThread();
    if (!caller || !caller->Process || caller->Process->Exiting) return 0;
    UINT16 index;
    for (index = 200; index < USER_OBJECT_SLOTS; index++)
        if (!g_user_objects[index].Process && g_user_objects[index].Generation != 65535)
            break;
    if (index == USER_OBJECT_SLOTS) return 0;
    USER_OBJECT_OWNER *object = &g_user_objects[index];
    object->Generation++;
    object->Type = type;
    object->Icon = object->SharedIcon = FALSE;
    object->Process = caller->Process;
    UINT64 head = PROCESS_USER_OBJECT_ARENA_VA + (UINT64)index * 0x200;
    memset((void *)head, 0, 0x200);
    UINT64 handle = ((UINT64)object->Generation << 16) | index;
    *(UINT64 *)head = handle;          /* the head carries its own handle */
    *(UINT32 *)(head + 8) = 1;         /* lock count */

    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA,
                   PROCESS_USER_SHARED_TABLE_COMMIT_SIZE, TRUE);
    UINT8 *entry = (UINT8 *)(PROCESS_USER_SHARED_TABLE_VA +
                             (UINT64)index * 24);
    *(UINT64 *)(entry + 0) = head;     /* Object   */
    *(UINT64 *)(entry + 8) = 0;        /* Owner    */
    entry[16] = type;                  /* Type     */
    entry[17] = 0;                     /* Flags    */
    *(UINT16 *)(entry + 18) = object->Generation;
    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA,
                   PROCESS_USER_SHARED_TABLE_COMMIT_SIZE, FALSE);

    if (head_out)
        *head_out = head;
    return handle;
}

/*
 * HCURSOR NtUserFindExistingCursorIcon(PUNICODE_STRING module,
 *                                      PUNICODE_STRING resource, void *param)
 *
 * Shared stock cursors have real kernel rasters. Resource/custom cursors are
 * decoded by USER32 and captured into immutable kernel-owned image storage.
 * The cache budget matches the object table so adding a second application
 * cannot exhaust a smaller, unrelated cursor-only table.
 */
#define USER_CURSOR_CACHE USER_OBJECT_SLOTS
typedef struct _CURSOR_KEY {
    UINT64 Module, Resource, Handle;
    PKPROCESS Process;
    BOOLEAN Icon;
} CURSOR_KEY;
static CURSOR_KEY g_cursor_cache[USER_CURSOR_CACHE];

#include "UserCursor.inc"

static UINT64 KiUserLoadCursorIcon(UINT64 module, UINT64 key,
                                   BOOLEAN icon, BOOLEAN shared)
{
    PKTHREAD caller = KeGetCurrentThread();
    if (!caller || !caller->Process) return 0;
    UINT64 flags = KiIrqSave();
    CURSOR_KEY *empty = NULL;
    if (shared) {
        for (UINT32 i = 0; i < USER_CURSOR_CACHE; i++) {
            CURSOR_KEY *cache = &g_cursor_cache[i];
            if (cache->Process == caller->Process && cache->Module == module &&
                cache->Resource == key && cache->Icon == icon) {
                UINT64 existing = cache->Handle;
                KiIrqRestore(flags);
                return existing;
            }
            if (!cache->Process && !empty) empty = cache;
        }
        if (!empty) { KiIrqRestore(flags); return 0; }
    }

    UINT64 handle = KiUserAllocateObject(USER_TYPE_CURSOR, NULL);
    if (!handle) { KiIrqRestore(flags); return 0; }
    USER_OBJECT_OWNER *object = &g_user_objects[(UINT16)handle];
    object->Icon = icon;
    object->SharedIcon = shared;
    if (!icon && !module && key)
        g_cursor_images[(UINT16)handle] = W32kStockCursor(key);
    if (empty)
        *empty = (CURSOR_KEY){module, key, handle, caller->Process, icon};
    KiIrqRestore(flags);
    return handle;
}

static UINT64 NtUserFindExistingCursorIcon(UINT64 *a)
{
    UINT64 resource = a[1], key = resource;
    if (resource && MmProbeForRead(resource, 16))
        key = *(volatile UINT64 *)(resource + 8); /* UNICODE_STRING.Buffer */
    if (a[0] || !W32kStockCursor(key)) return 0;
    return KiUserLoadCursorIcon(a[0], key, FALSE, TRUE);
}

static UINT64 SvNtUserCreateIconHandle(UINT64 *a)
{
    return KiUserLoadCursorIcon(a[0], a[1], TRUE, a[2] != 0);
}

/* HACCEL NtUserCreateAcceleratorTable(LPACCEL entries, ULONG count) */
static UINT64 NtUserCreateAcceleratorTable(UINT64 *a)
{
    UINT32 entries = (UINT32)a[1];
    UINT64 head = 0;
    UINT64 handle = KiUserAllocateObject(USER_TYPE_ACCEL, &head);
    if (!handle)
        return 0;
    /* The table itself is only consulted by TranslateAccelerator, which is a
     * no-op here; the handle is what the caller checks. */
    KeLog("[user] accelerator table (%lu entries) -> %p\n",
          (unsigned long)entries, (void *)handle);
    return handle;
}

static UINT64 KiUserDestroyObject(UINT64 handle, UINT8 type)
{
    UINT16 index = (UINT16)handle;
    PKTHREAD caller = KeGetCurrentThread();
    if (!caller || index < 200 || index >= USER_OBJECT_SLOTS) return 0;
    USER_OBJECT_OWNER *object = &g_user_objects[index];
    if (object->Process != caller->Process || object->Type != type ||
        handle != (((UINT64)object->Generation << 16) | index)) return 0;
    if (type == USER_TYPE_CURSOR) W32kCursorDestroy(handle);
    object->Process = NULL;
    object->Type = 0;
    for (UINT32 i = 0; i < USER_CURSOR_CACHE; i++)
        if (g_cursor_cache[i].Handle == handle) memset(&g_cursor_cache[i], 0, sizeof(CURSOR_KEY));
    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA,
                   PROCESS_USER_SHARED_TABLE_COMMIT_SIZE, TRUE);
    memset((void *)(PROCESS_USER_SHARED_TABLE_VA + (UINT64)index * 24), 0, 24);
    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA,
                   PROCESS_USER_SHARED_TABLE_COMMIT_SIZE, FALSE);
    return 1;
}
static UINT64 NtUserDestroyAcceleratorTable(UINT64 *a) { return KiUserDestroyObject(a[0], USER_TYPE_ACCEL); }
static UINT64 NtUserDestroyCursor(UINT64 *a)
{
    PKTHREAD caller = KeGetCurrentThread();
    USER_OBJECT_OWNER *object = caller ? W32kCursorObject(a[0], caller->Process) : NULL;
    if (!object || object->Icon || object->SharedIcon) return 0;
    return KiUserDestroyObject(a[0], USER_TYPE_CURSOR);
}
static UINT64 SvNtUserDestroyIcon(UINT64 *a)
{
    UINT16 index = (UINT16)a[0];
    if (index < 200 || index >= USER_OBJECT_SLOTS ||
        !g_user_objects[index].Icon || g_user_objects[index].SharedIcon)
        return 0;
    return KiUserDestroyObject(a[0], USER_TYPE_CURSOR);
}

/* PAINTSTRUCT on x64: HDC hdc (0), BOOL fErase (8), RECT rcPaint (12..28),
 * BOOL fRestore (28), BOOL fIncUpdate (32), BYTE rgbReserved[32] (36). */
#define PAINTSTRUCT_SIZE 72

static UINT64 NtUserBeginPaint(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    UINT16 slot = (UINT16)(a[0] & 0xFFFF);
    UINT8 *ps = (UINT8 *)a[1];
    if (!ps || !MmProbeForWrite((UINT64)ps, PAINTSTRUCT_SIZE))
        return 0;
    memset(ps, 0, PAINTSTRUCT_SIZE);

    /* The paint rectangle is the client area: the window manager owns the
     * caption and the borders, so a client never receives them as paintable. */
    INT32 x = 0, y = 0, w = 0, h = 0;
    if (W32kWindowExists(slot))
        W32kClientOrigin(slot, &x, &y, &w, &h);
    *(UINT32 *)(ps + 8) = 1;  /* fErase: nothing has drawn the background */
    *(INT32 *)(ps + 12) = 0;  /* rcPaint, in client coordinates */
    *(INT32 *)(ps + 16) = 0;
    *(INT32 *)(ps + 20) = w;
    *(INT32 *)(ps + 24) = h;

    UINT64 hdc = W32kAcquireWindowDc(a[0], x, y, w, h);
    if (!hdc || !W32kBeginWindowPaint(hdc, a[0])) return 0;
    KiUserValidateWindow(slot);
    *(UINT64 *)(ps + 0) = hdc;
    KeTrace("[user] BeginPaint(HWND %p) client %ld,%ld %ldx%ld -> hdc %p\n",
          (void *)a[0], (long)x, (long)y, (long)w, (long)h, (void *)hdc);
    return hdc;
}

static UINT64 NtUserEndPaint(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE) || !a[1] ||
        !MmProbeForRead(a[1], PAINTSTRUCT_SIZE)) return 0;
    UINT64 hdc = *(const UINT64 *)a[1];
    return W32kEndWindowPaint(hdc, a[0]);
}

/* ------------------------------------------------------------------ */
/* Kernel -> user callbacks (the KiUserCallbackDispatcher equivalent)  */
/* ------------------------------------------------------------------ */

#define SN_NtCallbackReturn 0xFC

/* The entry stub (SyscallEntry.asm) hardcodes these KTHREAD offsets; fail
 * the build rather than silently corrupting the wrong fields when the struct
 * changes. */
#define KI_OFFSETOF(type, field) __builtin_offsetof(type, field)
_Static_assert(KI_OFFSETOF(KTHREAD, UserRip) == 0xAC0, "KTH_USER_RIP");
_Static_assert(KI_OFFSETOF(KTHREAD, UserRsp) == 0xAC8, "KTH_USER_RSP");
_Static_assert(KI_OFFSETOF(KTHREAD, UserRflags) == 0xAD0, "KTH_USER_RFLAGS");
_Static_assert(KI_OFFSETOF(KTHREAD, CallbackRip) == 0xAD8, "KTH_CB_RIP");
_Static_assert(KI_OFFSETOF(KTHREAD, CallbackRsp) == 0xAE0, "KTH_CB_RSP");
_Static_assert(KI_OFFSETOF(KTHREAD, CallbackFlags) == 0xAE8, "KTH_CB_FLAGS");
_Static_assert(KI_OFFSETOF(KTHREAD, RedirectPending) == 0xB29, "KTH_REDIRECT");

/* Per-thread resources, reused across callback chains, private to a process.
 * Each nesting level has a 64 KiB RW/NX stack and an unmapped guard page.
 * Image codecs and nested modal loops exceed the former two-page stacks.
 * The shared trampoline remains client-read-only RX. */
#define CALLBACK_STACK_ARENA_VA  0x0000000001400000ULL
#define CALLBACK_STACK_SLOTS     16
#define CALLBACK_MAX_DEPTH 8
#define CALLBACK_STACK_PAGES 16
#define CALLBACK_STACK_SLICE ((CALLBACK_STACK_PAGES + 1) * PAGE_SIZE)
#define CALLBACK_STACK_SLOT_SIZE (CALLBACK_MAX_DEPTH * CALLBACK_STACK_SLICE + PAGE_SIZE)
#define CALLBACK_THUNK_OFFSET (CALLBACK_STACK_SLOT_SIZE - PAGE_SIZE)
#define CALLBACK_THUNK_STRIDE 128

static void KiReleaseCallbackResources(PKTHREAD thread)
{
    if (!thread->UserCallbackBase || !thread->Process) return;
    UINT64 root = thread->Process->DirectoryTableBase;
    for (UINT64 off = PAGE_SIZE; off < CALLBACK_STACK_SLOT_SIZE; off += PAGE_SIZE) {
        UINT64 va = thread->UserCallbackBase + off;
        UINT64 pa = MmGetPhysicalAddressInAddressSpace(root, va);
        if (pa != MM_INVALID_PHYS && MmUnmapPageInAddressSpace(root, va))
            MmFreePages(PAGE_ALIGN(pa), 1);
    }
    thread->UserCallbackBase = 0;
    thread->UserCallbackDepth = 0;
    thread->CallbackActive = thread->RedirectPending = 0;
    thread->CallbackRip = thread->CallbackRsp = thread->CallbackFlags = 0;
    thread->CallbackContinue = thread->CallbackWindow = 0;
    thread->CallbackMsg = thread->CallbackLParam = 0;
    thread->OrigRip = thread->OrigRsp = thread->OrigRflags = 0;
    thread->SavedReturnAddress = 0;
}

static UINT64 KiClaimCallbackStack(void)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (!thread || !thread->Process || !thread->Process->ProcessId ||
        thread->Process->Exiting) return 0;
    if (thread->UserCallbackBase)
        return thread->UserCallbackBase + CALLBACK_THUNK_OFFSET;
    for (UINT32 slot = 0; slot < CALLBACK_STACK_SLOTS; slot++) {
        UINT64 base = CALLBACK_STACK_ARENA_VA + (UINT64)slot * CALLBACK_STACK_SLOT_SIZE;
        BOOLEAN busy = FALSE;
        for (UINT32 i = 0; i < MM_MAX_PROCESS_REGIONS; i++) {
            PMM_PROCESS_REGION region = &thread->Process->VmRegions[i];
            if (region->InUse && region->Base < base + CALLBACK_STACK_SLOT_SIZE &&
                base < region->Base + region->ReserveSize) { busy = TRUE; break; }
        }
        for (UINT64 off = 0; !busy && off < CALLBACK_STACK_SLOT_SIZE; off += PAGE_SIZE)
            if (MmGetPhysicalAddress(base + off) != MM_INVALID_PHYS) busy = TRUE;
        if (busy) continue;
        thread->UserCallbackBase = base;
        for (UINT64 off = PAGE_SIZE; off < CALLBACK_STACK_SLOT_SIZE; off += PAGE_SIZE) {
            /* Leave one unmapped page between adjacent callback stacks. */
            if (off != CALLBACK_THUNK_OFFSET &&
                (CALLBACK_THUNK_OFFSET - off) % CALLBACK_STACK_SLICE == 0)
                continue;
            UINT64 pa = MmAllocatePage();
            if (pa == MM_INVALID_PHYS) { KiReleaseCallbackResources(thread); return 0; }
            UINT64 flags = off == CALLBACK_THUNK_OFFSET ? PTE_USER : PTE_USER | PTE_WRITE | PTE_NX;
            if (!MmMapPage(base + off, pa, flags)) {
                MmFreePages(pa, 1);
                KiReleaseCallbackResources(thread);
                return 0;
            }
            memset(MmPhysToVirt(pa), 0, PAGE_SIZE);
        }
        return base + CALLBACK_THUNK_OFFSET;
    }
    return 0;
}

/*
 * Update this thread's trampoline via the kernel direct map. Loads four Win64
 * argument registers with immediates, provides home space, calls the target
 * (WndProc), and re-enters the kernel with the result:
 *   movabs rax, imm ; mov rcx/rdx/r8/r9, rax  (x4)
 *   sub rsp, 0x28 ; movabs rax, target ; call rax
 *   mov r10, rax ; mov eax, 0xFC ; syscall ; int3
 */
static UINT64 LdrpBuildCallbackThunk(UINT64 target, UINT64 a1, UINT64 a2,
                                     UINT64 a3, UINT64 a4)
{
    UINT64 slot = KiClaimCallbackStack(); /* stack top is also the code page VA */
    if (!slot) return 0;
    PKTHREAD thread = KeGetCurrentThread();
    slot += (thread->UserCallbackDepth - 1) * CALLBACK_THUNK_STRIDE;
    UINT64 pa = MmGetPhysicalAddress(slot);
    if (pa == MM_INVALID_PHYS) return 0;
    UINT8 *code = (UINT8 *)MmPhysToVirt(PAGE_ALIGN(pa)) + (slot & (PAGE_SIZE - 1));
    UINT64 args[4] = {a1, a2, a3, a4};
    int o = 0;
    for (int i = 0; i < 4; i++) {
        code[o++] = 0x48; code[o++] = 0xB8; /* movabs rax, imm64 */
        memcpy(code + o, &args[i], sizeof(UINT64));
        o += 8;
        /* mov rcx/rdx, rax is 48 89 C1/C2; r8/r9 need REX.B: 49 89 C0/C1. */
        code[o++] = (UINT8)(i < 2 ? 0x48 : 0x49);
        code[o++] = 0x89;
        code[o++] = (UINT8)(i < 2 ? 0xC1 + i : 0xC0 + (i - 2));
    }
    code[o++] = 0x48; code[o++] = 0x83; code[o++] = 0xEC; /* sub rsp, 0x28 */
    code[o++] = 0x28;
    code[o++] = 0x48; code[o++] = 0xB8; /* movabs rax, target */
    memcpy(code + o, &target, sizeof(target));
    o += 8;
    code[o++] = 0xFF; code[o++] = 0xD0; /* call rax */
    /* mov r10, rax: REX.W|REX.B (49) with reg=rax, rm=r10. (0x4C 0x89 0xC2
     * would be `mov rdx, r8` -- the result would never reach the kernel.) */
    code[o++] = 0x49; code[o++] = 0x89; code[o++] = 0xC2;
    UINT32 service = SN_NtCallbackReturn;
    code[o++] = 0xB8; memcpy(code + o, &service, sizeof(service)); o += 4;
    code[o++] = 0x0F; code[o++] = 0x05; /* syscall */
    code[o++] = 0xCC;
    /* Serialize instruction fetch after updating code through a physical
     * alias; chaining may immediately re-enter this same executable page. */
    UINT32 eax = 0, ebx, ecx = 0, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx) : : "memory");
    return slot;
}

/*
 * Arm a kernel->user callback for this thread: the current sysret (belonging
 * to the service calling this) is redirected into a trampoline that invokes
 * the WndProc. The first callback of a chain captures the original caller's
 * frame in Orig*; the final redirect (KiFinishCallbackToCaller) restores it
 * with the callback's result in RAX. Returns FALSE when the nesting limit is reached or the user frame is invalid.
 */
static BOOLEAN KiStartUserCallback(UINT64 target, UINT64 a1, UINT64 a2,
                                   UINT64 a3, UINT64 a4, UINT64 continuation,
                                   UINT64 window, UINT64 msg, UINT64 lparam)
{
    PKTHREAD t = KeGetCurrentThread();
    if (!t || t->UserCallbackDepth >= CALLBACK_MAX_DEPTH ||
        t->RedirectPending || !target ||
        target >= MM_USER_MAX || !MmProbeForRead(target, 1) ||
        (MmGetPageFlags(target) & PTE_NX) ||
        !MmProbeForRead(t->UserRsp, sizeof(UINT64)))
        return FALSE;

    unsigned depth = ++t->UserCallbackDepth;
    UINT64 thunk = LdrpBuildCallbackThunk(target, a1, a2, a3, a4);
    UINT64 stack = t->UserCallbackBase + CALLBACK_THUNK_OFFSET -
                   (depth - 1) * CALLBACK_STACK_SLICE;
    if (!thunk || !stack) {
        if (t->UserCallbackDepth) --t->UserCallbackDepth;
        return FALSE;
    }
    if (depth > 1) {
        __typeof__(t->UserCallbackFrames[0]) *saved = &t->UserCallbackFrames[depth - 2];
        *saved = (__typeof__(*saved)){t->OrigRip, t->OrigRsp, t->OrigRflags,
            t->SavedReturnAddress, t->CallbackContinue, t->CallbackWindow,
            t->CallbackMsg, t->CallbackLParam};
    }

    t->OrigRip = t->UserRip;
    t->OrigRsp = t->UserRsp;
    t->OrigRflags = t->UserRflags;
    /* Snapshot the caller's return address so the final redirect can put it
     * back even if ring-3 scribbled on the (now dead) stack slot meanwhile. */
    t->SavedReturnAddress =
        MmProbeForRead(t->UserRsp, sizeof(UINT64))
            ? *(volatile UINT64 *)t->UserRsp : 0;
    t->CallbackActive = 1;

    /* RSP%16==8 at the trampoline entry: its `sub rsp,0x28` + `call` then put
     * the WndProc entry at the Win64-conventional RSP%16==8 with the home
     * space mapped. */
    t->CallbackRip = thunk;
    t->CallbackRsp = ((stack - 0x100) & ~0xFULL) | 8;
    t->CallbackFlags = 0x202; /* IF set */
    t->CallbackContinue = continuation;
    t->CallbackWindow = window;
    t->CallbackMsg = msg;
    t->CallbackLParam = lparam;
    t->RedirectPending = 1;
    KeTrace("[user]   callback -> proc %p msg 0x%lx hwnd %p (thunk %p rsp %p, "
          "caller %p)\n", (void *)target, (unsigned long)a2, (void *)a1,
          (void *)thunk, (void *)t->CallbackRsp, (void *)t->OrigRip);
    return TRUE;
}

/*
 * Chain the next callback of a creation (WM_NCCREATE -> WM_CREATE) from inside
 * NtCallbackReturn's dispatch. The original frame stays captured in Orig*;
 * only the redirect target changes.
 */
static BOOLEAN KiChainUserCallback(UINT64 target, UINT64 a1, UINT64 a2,
                                   UINT64 a3, UINT64 a4, UINT64 continuation,
                                   UINT64 window, UINT64 msg, UINT64 lparam)
{
    PKTHREAD t = KeGetCurrentThread();
    if (!t || !t->CallbackActive || t->RedirectPending || !target ||
        target >= MM_USER_MAX || !MmProbeForRead(target, 1) ||
        (MmGetPageFlags(target) & PTE_NX))
        return FALSE;

    UINT64 thunk = LdrpBuildCallbackThunk(target, a1, a2, a3, a4);
    if (!thunk)
        return FALSE;

    t->CallbackRip = thunk;
    UINT64 stack = t->UserCallbackBase + CALLBACK_THUNK_OFFSET -
                   (t->UserCallbackDepth - 1) * CALLBACK_STACK_SLICE;
    t->CallbackRsp = ((stack - 0x100) & ~0xFULL) | 8;
    t->CallbackContinue = continuation;
    t->CallbackWindow = window;
    t->CallbackMsg = msg;
    t->CallbackLParam = lparam;
    t->RedirectPending = 1;
    return TRUE;
}

/* End of the chain: the next (and last) redirect sends the sysret back to the
 * original caller with `result` in RAX, exactly as the arming service would
 * have returned it. */
static void KiFinishCallbackToCaller(PKTHREAD t, UINT64 result)
{
    (void)result;
    t->CallbackRip = t->OrigRip;
    t->CallbackRsp = t->OrigRsp;
    t->CallbackFlags = t->OrigRflags;
    /* Enforce the win32k guarantee: the interrupted frame is returned to
     * exactly as it was left, including the slot its stub will `ret`
     * through. Ring-3 code running between arming and here (this thread's
     * own WndProc or a sibling thread's stray write) must not be able to
     * derail the return path. */
    if (MmProbeForWrite(t->OrigRsp, sizeof(UINT64)))
        *(volatile UINT64 *)t->OrigRsp = t->SavedReturnAddress;
    t->CallbackContinue = 0;
    t->CallbackActive = 0;
    if (t->UserCallbackDepth) --t->UserCallbackDepth;
    if (t->UserCallbackDepth) {
        __typeof__(t->UserCallbackFrames[0]) *saved =
            &t->UserCallbackFrames[t->UserCallbackDepth - 1];
        t->OrigRip = saved->OrigRip; t->OrigRsp = saved->OrigRsp;
        t->OrigRflags = saved->OrigRflags;
        t->SavedReturnAddress = saved->SavedReturnAddress;
        t->CallbackContinue = saved->Continue; t->CallbackWindow = saved->Window;
        t->CallbackMsg = saved->Msg; t->CallbackLParam = saved->LParam;
        t->CallbackActive = 1;
    }
    t->RedirectPending = 1;
}

/* Destruction walks the child tree between callbacks. Each window receives
 * WM_DESTROY before its children and WM_NCDESTROY immediately before release. */
#define CB_DESTROY_CREATE_FAILURE (1ULL << 63)
static UINT64 KiContinueDestroy(PKTHREAD thread, UINT16 slot, UINT64 root)
{
    UINT64 root_hwnd = root & ~CB_DESTROY_CREATE_FAILURE;
    UINT64 result = (root & CB_DESTROY_CREATE_FAILURE) ? 0 : 1;
    for (;;) {
        if (!KiUserWindowValid(root_hwnd, TRUE)) {
            KiFinishCallbackToCaller(thread, 0); return 0;
        }
        for (UINT16 child = 1; child < 128; ++child) {
            if (!g_window_owner[child] || W32kWindowParent(child) != slot) continue;
            UINT64 hwnd = KiUserWindowHandle(child), proc = KiWindowProcOf(hwnd);
            g_window_destroying[child] = TRUE;
            if (proc && KiChainUserCallback(proc, hwnd, 0x0002, 0, 0,
                    CB_CONT_DESTROY_CHILDREN, hwnd, 0x0002, root)) return 0;
            KiUserDropWindow(child);
        }
        UINT64 hwnd = KiUserWindowHandle(slot), proc = KiWindowProcOf(hwnd);
        if (proc && KiChainUserCallback(proc, hwnd, 0x0082, 0, 0,
                CB_CONT_DESTROY_FINAL, hwnd, 0x0082, root)) return 0;
        UINT16 parent = W32kWindowParent(slot);
        KiUserDropWindow(slot);
        if (hwnd == root_hwnd || !parent) {
            KiFinishCallbackToCaller(thread, result); return result;
        }
        slot = parent;
    }
}

/* NtCallbackReturn: the trampoline re-entered the kernel with the WndProc
 * result in R10. Drive the kernel-side continuation state machine. */
static UINT64 NtCallbackReturn(UINT64 *a)
{
    PKTHREAD t = KeGetCurrentThread();
    if (!t || !t->CallbackActive)
        return 0;
    UINT64 result = a[0]; /* r10 */
    if ((t->CallbackContinue == CB_CONT_CREATE_NCCREATE ||
         t->CallbackContinue == CB_CONT_CREATE_WMCREATE) &&
        !KiUserWindowValid(t->CallbackWindow, TRUE)) {
        KiFinishCallbackToCaller(t, 0);
        return 0;
    }
    KeTrace("[user]   callback return: msg 0x%lx -> %p (cont %lu)\n",
          (unsigned long)t->CallbackMsg, (void *)result,
          (unsigned long)t->CallbackContinue);

    if (t->CallbackContinue == CB_CONT_DESTROY_CHILDREN)
        return KiContinueDestroy(t, (UINT16)t->CallbackWindow, t->CallbackLParam);
    if (t->CallbackContinue == CB_CONT_DESTROY_FINAL) {
        UINT64 hwnd = t->CallbackWindow, root = t->CallbackLParam;
        UINT16 parent = W32kWindowParent((UINT16)hwnd);
        KiUserDropWindow((UINT16)hwnd);
        if (hwnd != (root & ~CB_DESTROY_CREATE_FAILURE) && parent)
            return KiContinueDestroy(t, parent, root);
        UINT64 status = (root & CB_DESTROY_CREATE_FAILURE) ? 0 : 1;
        KiFinishCallbackToCaller(t, status); return status;
    }

    if (t->CallbackContinue == CB_CONT_CREATE_NCCREATE) {
        if (result == 0) {
            /* WM_NCCREATE refused: fail the creation. */
            UINT16 slot = (UINT16)(t->CallbackWindow & 0xFFFF);
            if (slot && slot < 128) KiUserDropWindow(slot);
            KiFinishCallbackToCaller(t, 0);
            return 0; /* CreateWindowExW sees NULL */
        }
        /* Continue into WM_CREATE: the window head carries the WndProc. */
        UINT16 slot = (UINT16)(t->CallbackWindow & 0xFFFF);
        UINT64 window = PROCESS_USER_OBJECT_ARENA_VA + (UINT64)slot * 0x200;
        UINT64 proc = 0;
        if (MmProbeForRead(window + WND_WNDPROC_OFF, 8))
            proc = *(volatile UINT64 *)(window + WND_WNDPROC_OFF);
        if (proc && KiChainUserCallback(proc, t->CallbackWindow, 0x0001, 0,
                                        t->CallbackLParam,
                                        CB_CONT_CREATE_WMCREATE,
                                        t->CallbackWindow, 0x0001,
                                        t->CallbackLParam))
            return 0; /* redirected again; value unused */
        /* Dispatch of WM_CREATE failed: don't pretend it ran successfully. */
        KiUserDropWindow(slot);
        KiFinishCallbackToCaller(t, 0);
        return 0;
    }

    if (t->CallbackContinue == CB_CONT_CREATE_WMCREATE) {
        UINT16 slot = (UINT16)(t->CallbackWindow & 0xFFFF);
        if ((INT64)result == -1) {
            /* A failed parent creation still tears down its controls through
             * their window procedures; CreateWindowEx must finally return NULL. */
            UINT64 hwnd = t->CallbackWindow, proc = KiWindowProcOf(hwnd);
            UINT64 root = hwnd | CB_DESTROY_CREATE_FAILURE;
            g_window_destroying[slot] = TRUE;
            if (proc && KiChainUserCallback(proc, hwnd, 0x0002, 0, 0,
                    CB_CONT_DESTROY_CHILDREN, hwnd, 0x0002, root)) return 0;
            return KiContinueDestroy(t, slot, root);
        }
        UINT64 created = t->CallbackWindow;
        KiFinishCallbackToCaller(t, created);
        return created;
    }

    KiFinishCallbackToCaller(t, result);
    return result;
}

/* Build the CREATESTRUCTW a WM_NCCREATE/WM_CREATE lParam points at, from the
 * NtUserCreateWindowEx argument block: (exStyle, class, version, name, style,
 * x, y, cx, cy, parent, menu, instance, lpParam, ...). x64 layout. */
static UINT64 KiBuildCreateStruct(const UINT64 *a, UINT64 hwnd)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (!thread || thread->UserCallbackDepth >= CALLBACK_MAX_DEPTH ||
        thread->RedirectPending) return 0;
    UINT64 top = KiClaimCallbackStack();
    if (!top) return 0;
    UINT64 p = top - thread->UserCallbackDepth * CALLBACK_STACK_SLICE - 0x80;
    UINT8 *cs = (UINT8 *)p;
    memset(cs, 0, 0x60);
    *(UINT64 *)(cs + 0x00) = a[12];      /* lpCreateParams */
    *(UINT64 *)(cs + 0x08) = a[11];      /* hInstance      */
    *(UINT64 *)(cs + 0x10) = a[10];      /* hMenu          */
    *(UINT64 *)(cs + 0x18) = a[9];       /* hwndParent     */
    *(UINT32 *)(cs + 0x20) = (UINT32)a[8]; /* cy           */
    *(UINT32 *)(cs + 0x24) = (UINT32)a[7]; /* cx           */
    *(UINT32 *)(cs + 0x28) = (UINT32)a[6]; /* y            */
    *(UINT32 *)(cs + 0x2C) = (UINT32)a[5]; /* x            */
    *(UINT32 *)(cs + 0x30) = (UINT32)a[4]; /* style        */
    *(UINT64 *)(cs + 0x38) = a[3];       /* lpszName       */
    UINT64 class_name = a[1];
    if (class_name > 0xFFFF && MmProbeForRead(class_name, 16))
        class_name = *(const UINT64 *)(class_name + 8);
    *(UINT64 *)(cs + 0x40) = class_name; /* class string or atom, never LARGE_STRING */
    *(UINT32 *)(cs + 0x48) = (UINT32)a[0]; /* dwExStyle     */
    KeTrace("[user]   CREATESTRUCT @ %p for HWND %p\n", (void *)p,
          (void *)hwnd);
    return p;
}

static UINT64 NtUserCreateWindowEx(UINT64 *a)
{
    typedef struct _USER_HANDLE_ENTRY_LOCAL {
        UINT64 Object;
        UINT64 Owner;
        UINT8 Type;
        UINT8 Flags;
        UINT16 Generation;
        UINT32 Padding;
    } USER_HANDLE_ENTRY_LOCAL;

    typedef struct _USER_WINDOW_HEAD_LOCAL {
        UINT64 Handle;
        UINT32 LockCount;
        UINT32 Flags;
        UINT64 ThreadInfo;
        UINT64 Desktop;
        UINT64 Self;
    } USER_WINDOW_HEAD_LOCAL;


    /* Modern win32u passes class-version separately from the window name:
     * exStyle, class, classVersion, name, style, x, y, cx, cy, ... */
    KeTrace("[user] NtUserCreateWindowEx(exStyle=0x%lx, class=%p, version=%p, name=%p, style=0x%lx, xy=%ld,%ld size=%ldx%ld)\n",
          (unsigned long)a[0], (void *)a[1], (void *)a[2], (void *)a[3],
          (unsigned long)a[4], (long)a[5], (long)a[6], (long)a[7],
          (long)a[8]);

    PKTHREAD creator = KeGetCurrentThread();
    if (!creator || !creator->Process || creator->Process->Exiting) return 0;
    if ((a[4] & 0x40000000u) && !a[9]) {
        KeLog("[user]   rejected child window with NULL parent\n");
        return 0;
    }
    if (a[9] && !KiUserWindowValid(a[9], TRUE)) return 0;
    USER_CLASS_LOCAL *cls = KiClassForCreate(a[1]);
    if(!cls){
        if(creator->UserGsBase&&MmProbeForWrite(creator->UserGsBase+0x68,4))
            *(UINT32*)(creator->UserGsBase+0x68)=1407; /* ERROR_CANNOT_FIND_WND_CLASS */
        KeLog("[user]   rejected window with unregistered class\n");return 0;
    }
    if (!KiUserQueueForThread(creator->ThreadId)) return 0;
    UINT16 index;
    for (index = 1; index < 128; index++)
        if (!g_window_owner[index] && g_window_generation[index] != 65535) break;
    if (index == 128) return 0;
    if (!NT_SUCCESS(W32kBindWindowDesktop(index, creator, (UINT16)a[9]))) return 0;
    UINT16 generation = ++g_window_generation[index];
    UINT64 hwnd = ((UINT64)generation << 16) | index;
    USER_HANDLE_ENTRY_LOCAL *entries =
        (USER_HANDLE_ENTRY_LOCAL *)PROCESS_USER_SHARED_TABLE_VA;
    USER_WINDOW_HEAD_LOCAL *window = (USER_WINDOW_HEAD_LOCAL *)(
        PROCESS_USER_OBJECT_ARENA_VA + (UINT64)index * 0x200);

    memset(window, 0, 0x200);
    window->Handle = hwnd;
    window->LockCount = 1;
    /* Flip the shared handle table writable for the kernel-side fill; ring 3
     * normally sees it read-only (see process environment setup). */
    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA,
                   PROCESS_USER_SHARED_TABLE_COMMIT_SIZE,
                   TRUE);
    entries[index].Object = (UINT64)window;
    entries[index].Owner = 0;
    entries[index].Type = 1; /* TYPE_WINDOW */
    entries[index].Flags = 0;
    entries[index].Generation = generation;
    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA,
                   PROCESS_USER_SHARED_TABLE_COMMIT_SIZE,
                   FALSE);

    g_window_owner[index] = creator ? creator->ThreadId : 0;
    g_window_process[index] = creator->Process;

    g_window_classes[index] = cls;
    KiPublishWindowState((UINT64)window, (UINT32)a[4], (UINT32)a[0],
                         a[9], cls);
    {
        char ascii[33];
        UINT32 j = 0;
        if (cls)
            for (; j < 32 && cls->Name[j]; j++)
                ascii[j] = (char)cls->Name[j];
        ascii[j] = 0;
        KeTrace("[user]   created HWND %p head %p class '%s' atom 0x%x "
              "wndproc %p extra %lu\n", (void *)hwnd, (void *)window,
              cls ? ascii : "<unresolved>", cls ? cls->Atom : 0,
              (void *)(cls ? cls->WndProc : 0),
              (unsigned long)(cls ? cls->WndExtra : 0));
    }
    /* The window exists: hand it to the window manager, which places it,
     * decides its kind from the class name and owns its frame, z-order and
     * input from here on. */
    {
        WCHAR title[W32K_TITLE_CHARS];
        UINT32 length = 0;
        if (a[3] && MmProbeForRead(a[3], sizeof(WCHAR))) {
            const WCHAR *name = (const WCHAR *)a[3];
            for (; length < W32K_TITLE_CHARS - 1; ++length) {
                if (!MmProbeForRead((UINT64)(name + length), sizeof(WCHAR)))
                    break;
                title[length] = name[length];
                if (!title[length]) break;
            }
        }
        title[length] = 0;
        UINT8 kind = W32kClassifyWindow(cls ? cls->Name : NULL, (UINT32)a[4]);
        W32kScreenWindowCreated(index, title, (UINT32)a[4], (INT32)a[5],
                                (INT32)a[6], (UINT32)a[7], (UINT32)a[8], kind);
        static const WCHAR static_class[]={'S','T','A','T','I','C',0};
        W32kScreenSetStatic(index,KiWideEqual(cls->Name,static_class));
        W32kScreenSetExStyle(index, (UINT32)a[0]);
        if (a[9]) W32kScreenSetParent(index, (UINT16)a[9]);
    }
    g_window_menus[index] = ((UINT32)a[4] & 0x40000000u) ? 0 : a[10];
    if(g_window_menus[index])W32kSetMenuBar(index,NULL,0,TRUE);
    /* A window that is created visible owes its first WM_PAINT immediately. */
    if ((UINT32)a[4] & WS_VISIBLE_)
        KiUserInvalidateWindow(index);
    if (cls && cls->WndProc) {
        UINT64 cs = KiBuildCreateStruct(a, hwnd);
        if (cs && KiStartUserCallback(cls->WndProc, hwnd, 0x0081 /* WM_NCCREATE */,
                                0, cs, CB_CONT_CREATE_NCCREATE, hwnd,
                                0x0081, cs))
            return 0; /* value unused; the callback path returns the HWND */
        KiUserDropWindow(index);
        return 0; /* allocation/dispatch failure must not report successful creation */
    }

    return hwnd;
}

static UINT64 NtUserChangeWindowMessageFilterEx(UINT64 *a)
{
    /* (hwnd, message, action, pCHANGEFILTERSTRUCT): the shell asserts the
     * taskbar/desktop messages through this gate right after creating the
     * WorkerW. Grant the request and report the filtered-allowed state. */
    UINT64 *out = (UINT64 *)a[3];
    if (out && MmProbeForWrite((UINT64)out, sizeof(UINT64))) {
        out[0] = 4; /* cbSize (DWORD) + status STATUS_SUCCESS (DWORD) */
    }
    return 1;
}

static UINT64 NtUserDestroyWindow(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    UINT16 slot = (UINT16)a[0];
    if (g_window_destroying[slot]) return 0;
    UINT64 proc = KiWindowProcOf(a[0]);
    if (proc) {
        g_window_destroying[slot] = TRUE;
        if (KiStartUserCallback(proc, a[0], 0x0002, 0, 0,
                CB_CONT_DESTROY_CHILDREN, a[0], 0x0002, a[0])) return 0;
        g_window_destroying[slot] = FALSE;
        return 0;
    }
    KiUserDropWindow(slot);
    return 1;
}

static void KiUserDropOneWindow(UINT16 slot)
{
    UINT64 handle = KiUserWindowHandle(slot);
    UINT32 owner = g_window_owner[slot];
    (void)W32kClearSharedHandle(g_window_process[slot], FALSE, slot);
    W32kScreenWindowDestroyed(slot);
    W32kUnbindWindowDesktop(slot);
    W32kCleanupWindowDc(g_window_process[slot], handle);
    for (UINT32 i = 0; i < USER_MAX_PROPS; i++)
        if (g_user_props[i].Used && g_user_props[i].Hwnd == handle)
            memset(&g_user_props[i], 0, sizeof(g_user_props[i]));
    g_window_destroying[slot] = FALSE;
    g_window_owner[slot] = 0;
    g_window_process[slot] = NULL;
    g_window_update[slot] = 0;
    if (g_user_active_hwnd == handle) g_user_active_hwnd = 0;
    USER_MSG_QUEUE *queue = KiUserQueueForThread(owner);
    if (queue) {
        UINT32 write = queue->Tail;
        for (UINT32 read = queue->Tail; read != queue->Head;
             read = (read + 1) % USER_MESSAGE_QUEUE_CAPACITY) {
            if (queue->Msgs[read].Hwnd == handle) continue;
            queue->Msgs[write] = queue->Msgs[read];
            write = (write + 1) % USER_MESSAGE_QUEUE_CAPACITY;
        }
        queue->Head = write;
        for (UINT32 i = 0; i < USER_MAX_QUEUE_TIMERS; ++i)
            if (queue->Timers[i].Window == handle)
                queue->Timers[i] = (USER_WINDOW_TIMER){0};
        KiUserSyncInputEvent(queue);
    }
}

static void KiUserDropWindow(UINT16 root)
{
    UINT16 slot = root;
    for (;;) {
        UINT16 child;
        for (child = 1; child < 128; ++child)
            if (g_window_owner[child] && W32kWindowParent(child) == slot) break;
        if (child < 128) { slot = child; continue; }
        UINT16 parent = W32kWindowParent(slot);
        KiUserDropOneWindow(slot);
        if (slot == root || !parent) return;
        slot = parent;
    }
}

void W32kCleanupThreadUser(PKTHREAD thread)
{
    W32kHotkeyCleanupThread(thread);
    W32kClipboardCleanupThread(thread);
    W32kCursorCleanupThread(thread);
    KiReleaseCallbackResources(thread);
    for (UINT16 slot = 1; slot < 128; slot++)
        if (g_window_owner[slot] == thread->ThreadId && g_window_process[slot] == thread->Process)
            KiUserDropWindow(slot);
    W32kCleanupThreadSession(thread);
    for (UINT32 i = 0; i < USER_MAX_THREAD_QUEUES; i++) {
        USER_MSG_QUEUE *queue = &g_user_thread_queues[i];
        if (queue->ThreadId != thread->ThreadId || queue->Process != thread->Process) continue;
        if (queue->InputEvent) {
            ObCloseHandleForObject(queue->Process->HandleTable, queue->InputEventHandle, queue->InputEvent);
            ObDereferenceObject(queue->InputEvent);
        }
        memset(queue, 0, sizeof(*queue));
    }
}

void W32kCleanupProcessUser(PKPROCESS process)
{
    W32kCursorCleanupProcess(process);
    for (UINT16 i = 200; i < USER_OBJECT_SLOTS; ++i)
        if (g_user_objects[i].Process == process && g_user_objects[i].Type == USER_TYPE_CURSOR)
            W32kCursorDestroy(((UINT64)g_user_objects[i].Generation << 16) | i);
    for (UINT32 i = 0; i < USER_MAX_CLASSES; i++)
        if (g_user_classes[i].Process == process) memset(&g_user_classes[i], 0, sizeof(g_user_classes[i]));
    for (UINT32 i = 200; i < USER_OBJECT_SLOTS; i++)
        if (g_user_objects[i].Process == process) {
            g_user_objects[i].Process = NULL;
            g_user_objects[i].Type = 0;
        }
    for (UINT32 i = 0; i < USER_CURSOR_CACHE; i++)
        if (g_cursor_cache[i].Process == process) memset(&g_cursor_cache[i], 0, sizeof(CURSOR_KEY));
}

static UINT64 NtUserDdeInitialize(UINT64 *a)
{
    /* DDE is optional for the shell startup path. Zero is the native success
     * status used by its USER wrapper. */
    return 0;
}

static UINT64 NtUserSetWinEventHook(UINT64 *a)
{
    /* No hook object/callback delivery exists yet. A NULL hook is the valid
     * failure sentinel and prevents USER32 from treating an NTSTATUS as an
     * H-WINEVENTHOOK. */
    return 0;
}

static UINT64 NtUserUnhookWinEvent(UINT64 *a)
{
    return 0;
}

static UINT64 NtUserGetMessage(UINT64 *a)
{
    USER_MSG_LOCAL *user_msg = (USER_MSG_LOCAL *)a[0];
    if (!user_msg || !MmProbeForWrite((UINT64)user_msg, sizeof(*user_msg)))
        return (UINT64)-1;

    USER_MSG_LOCAL msg;
    PKTHREAD caller = KeGetCurrentThread();
    UINT32 tid = caller ? caller->ThreadId : 0;
    while (!KiUserTakeMessage(&msg, TRUE))
        if (!KiUserWaitForInput()) return (UINT64)-1;
    if (!MmProbeForWrite((UINT64)user_msg, sizeof(*user_msg)))
        return (UINT64)-1;
    *user_msg = msg;
    KeTrace("[user] GetMessage -> msg 0x%x hwnd %p (tid %lu)\n",
          (unsigned)msg.Message, (void *)msg.Hwnd, (unsigned long)tid);
    return msg.Message == 0x0012 /* WM_QUIT */ ? 0 : 1;
}
