#define _GNU_SOURCE
/* Actual USER/GDI implementations, two fake processes, no guest test EXE. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <svindows/Ke.h>
static UINT64 HostIrqSave(void) { return 0; }
static void HostIrqRestore(UINT64 flags) { (void)flags; }
#define KiIrqSave HostIrqSave
#define KiIrqRestore HostIrqRestore
#include "../../kernel/ke/Syscall.c"
#include "../../kernel/win32k/Gdiobj.c"
#include "../../kernel/ob/ObHandle.c"

static KPROCESS process_a, process_b;
static KTHREAD thread_a, thread_b;
static PKTHREAD current;
static unsigned freed;
static int fail_pool_after = -1;
static BOOLEAN graphics;
UINT64 MmDirectMapBase;
typedef struct _TEST_PAGE {
    void *Physical;
    UINT64 Root, Va, Flags;
    int Fd;
    BOOLEAN Mapped;
} TEST_PAGE;
#define TEST_PAGE_CAPACITY 4096
static TEST_PAGE pages[TEST_PAGE_CAPACITY];
static int fail_allocate_after = -1, fail_map_after = -1;
static unsigned page_allocations, page_frees;
GFX_FRAMEBUFFER GfxFramebuffer;
UINT64 NtUserGetSystemMetrics(UINT64 *args) { (void)args; return 0; }
UINT64 NtUserGetSysColor(UINT64 *args) { (void)args; return 0; }
PKTHREAD KeGetCurrentThread(void) { return current; }
UINT64 KeGetTickCount(void) { return 0; }
void KeYield(void) { abort(); }
int KeLog(const char *fmt, ...) { (void)fmt; return 0; }
PVOID ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T size, ULONG tag)
{
    (void)type; (void)tag;
    if (!fail_pool_after) return NULL;
    if (fail_pool_after > 0) fail_pool_after--;
    return malloc(size);
}
void ExFreePool(PVOID p) { if (p) { freed++; free(p); } }
BOOLEAN MmProbeForRead(UINT64 va, UINT64 size) { (void)size; return va != 0; }
BOOLEAN MmProbeForWrite(UINT64 va, UINT64 size) { (void)size; return va != 0; }
void MmProtectRange(UINT64 base, UINT64 size, BOOLEAN writable)
{ (void)base; (void)size; (void)writable; }
BOOLEAN GfxAvailable(void) { return graphics; }
void HalConsoleSetGraphicsMode(BOOLEAN enabled) { (void)enabled; }
UINT32 GfxColor(UINT8 r, UINT8 g, UINT8 b) { return ((UINT32)r << 16) | ((UINT32)g << 8) | b; }
void GfxFillRect(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 color)
{ (void)x; (void)y; (void)w; (void)h; (void)color; }
void GfxDrawString(UINT32 x, UINT32 y, const char *s, UINT32 fg, UINT32 bg)
{ (void)x; (void)y; (void)s; (void)fg; (void)bg; }
/* The window manager composites through these three; this check exercises
 * window, message and handle bookkeeping, so painting is a no-op here. */
void GfxPutPixel(UINT32 x, UINT32 y, UINT32 color)
{ (void)x; (void)y; (void)color; }
int GfxUiAscent(void) { return 12; }
static const UINT8 test_glyph_bits[] = {255};
static const GFX_FONT_GLYPH test_glyph = {'A', 0, 1, 1, 0, 10, 5};
const GFX_FONT_FACE SvUiFontUiFace = {
    "fixture", 12, 12, 3, 15, test_glyph_bits, &test_glyph, 1
};
const GFX_FONT_GLYPH *GfxUiGlyph(UINT32 codepoint)
{ return codepoint == 'A' ? &test_glyph : NULL; }
UINT32 GfxUiCharWidth(UINT32 codepoint) { (void)codepoint; return 5; }
void GfxDrawUiTextW(UINT32 x, UINT32 baseline, const UINT16 *text, UINT32 color)
{ (void)x; (void)baseline; (void)text; (void)color; }
LONG KeSetEvent(PKEVENT e) { LONG old = e->Header.SignalState; e->Header.SignalState = 1; return old; }
void KeResetEvent(PKEVENT e) { e->Header.SignalState = 0; }
void ObReferenceObject(POBJECT o) { ObHeaderFromObject(o)->PointerCount++; }
void ObDereferenceObject(POBJECT o)
{ if (!--ObHeaderFromObject(o)->PointerCount) ExFreePool(ObHeaderFromObject(o)); }
void ObRemoveObjectName(POBJECT o)
{ assert(!ObHeaderFromObject(o)->ParentDirectory); }
static OBJECT_TYPE event_type;
NTSTATUS PsCreateNotificationEvent(PKEVENT *event, HANDLE *handle)
{
    POBJECT_HEADER header = calloc(1, sizeof(*header) + sizeof(KEVENT));
    assert(header);
    header->PointerCount = 1; header->Type = &event_type;
    *event = ObObjectFromHeader(header);
    NTSTATUS status = ObCreateHandle(*event, GENERIC_ALL, handle);
    if (!NT_SUCCESS(status)) ObDereferenceObject(*event);
    return status;
}
UINT64 MmAllocatePage(void)
{
    if (fail_allocate_after == 0) return MM_INVALID_PHYS;
    if (fail_allocate_after > 0) fail_allocate_after--;
    for (unsigned i = 0; i < TEST_PAGE_CAPACITY; i++) {
        TEST_PAGE *page = &pages[i];
        if (page->Physical) continue;
        page->Fd = memfd_create("svindows-callback-check", 0);
        assert(page->Fd >= 0 && !ftruncate(page->Fd, PAGE_SIZE));
        page->Physical = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, page->Fd, 0);
        assert(page->Physical != MAP_FAILED);
        page_allocations++;
        return (UINT64)page->Physical;
    }
    return MM_INVALID_PHYS;
}
BOOLEAN W32kClearSharedHandle(PKPROCESS process, BOOLEAN gdi, UINT16 index)
{ (void)process; (void)gdi; (void)index; return TRUE; }
BOOLEAN MmMapPage(UINT64 va, UINT64 pa, UINT64 flags)
{
    if (fail_map_after == 0) return FALSE;
    if (fail_map_after > 0) fail_map_after--;
    for (unsigned i = 0; i < TEST_PAGE_CAPACITY; i++) {
        TEST_PAGE *page = &pages[i];
        if ((UINT64)page->Physical != pa) continue;
        int prot = PROT_READ | ((flags & PTE_WRITE) ? PROT_WRITE : 0) |
                   ((flags & PTE_NX) ? 0 : PROT_EXEC);
        void *p = mmap((void *)va, PAGE_SIZE, prot, MAP_SHARED | MAP_FIXED_NOREPLACE, page->Fd, 0);
        if (p == MAP_FAILED) return FALSE;
        page->Root = current->Process->DirectoryTableBase;
        page->Va = va; page->Flags = flags; page->Mapped = TRUE;
        return TRUE;
    }
    return FALSE;
}
UINT64 MmGetPhysicalAddressInAddressSpace(UINT64 root, UINT64 va)
{
    for (unsigned i = 0; i < TEST_PAGE_CAPACITY; i++)
        if (pages[i].Mapped && pages[i].Root == root && pages[i].Va == PAGE_ALIGN(va))
            return (UINT64)pages[i].Physical + (va & PAGE_MASK);
    return MM_INVALID_PHYS;
}
UINT64 MmGetPhysicalAddress(UINT64 va)
{ return MmGetPhysicalAddressInAddressSpace(current->Process->DirectoryTableBase, va); }
UINT64 MmGetPageFlags(UINT64 va)
{
    if (va == 0xdead000) return PTE_PRESENT | PTE_USER | PTE_NX;
    for (unsigned i = 0; i < TEST_PAGE_CAPACITY; i++)
        if (pages[i].Mapped && pages[i].Root == current->Process->DirectoryTableBase &&
            pages[i].Va == PAGE_ALIGN(va)) return pages[i].Flags | PTE_PRESENT;
    return PTE_USER | PTE_PRESENT; /* fake executable WndProc fixture */
}
BOOLEAN MmUnmapPageInAddressSpace(UINT64 root, UINT64 va)
{
    for (unsigned i = 0; i < TEST_PAGE_CAPACITY; i++) {
        TEST_PAGE *page = &pages[i];
        if (!page->Mapped || page->Root != root || page->Va != va) continue;
        if (root == current->Process->DirectoryTableBase) assert(!munmap((void *)va, PAGE_SIZE));
        page->Mapped = FALSE;
        return TRUE;
    }
    return FALSE;
}
void MmFreePages(UINT64 pa, SIZE_T count)
{
    assert(count == 1);
    for (unsigned i = 0; i < TEST_PAGE_CAPACITY; i++) {
        TEST_PAGE *page = &pages[i];
        if ((UINT64)page->Physical != pa) continue;
        assert(!page->Mapped);
        assert(!munmap(page->Physical, PAGE_SIZE));
        close(page->Fd);
        memset(page, 0, sizeof(*page));
        page_frees++;
        return;
    }
    abort();
}

static void switch_callback_process(PKTHREAD thread)
{
    if (current->Process == thread->Process) { current = thread; return; }
    for (unsigned i = 0; i < TEST_PAGE_CAPACITY; i++)
        if (pages[i].Mapped && pages[i].Root == current->Process->DirectoryTableBase)
            assert(!munmap((void *)pages[i].Va, PAGE_SIZE));
    current = thread;
    for (unsigned i = 0; i < TEST_PAGE_CAPACITY; i++) {
        TEST_PAGE *page = &pages[i];
        if (!page->Mapped || page->Root != current->Process->DirectoryTableBase) continue;
        int prot = PROT_READ | ((page->Flags & PTE_WRITE) ? PROT_WRITE : 0) |
                   ((page->Flags & PTE_NX) ? 0 : PROT_EXEC);
        assert(mmap((void *)page->Va, PAGE_SIZE, prot, MAP_SHARED | MAP_FIXED_NOREPLACE,
                    page->Fd, 0) == (void *)page->Va);
    }
}

static void map_test_range(UINT64 address, SIZE_T size)
{
    void *p = mmap((void *)address, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(p == (void *)address);
}

static void prepare_callback(PKTHREAD thread, UINT64 *return_slot)
{
    thread->UserRsp = (UINT64)return_slot;
    thread->UserRip = 0x1234;
    thread->UserRflags = 0x202;
    thread->RedirectPending = 0;
}

static void check_callbacks(void)
{
    current = &thread_a;
    UINT64 original = 0, original_b = 0x5678;
    prepare_callback(&thread_a, &original);
    unsigned allocations = page_allocations;
    for (unsigned i = 0; i < 1000; i++) {
        prepare_callback(&thread_a, &original);
        assert(KiStartUserCallback(0x1000, i, 2, 3, 4, CB_CONT_SIMPLE, 0, 0, 0));
        assert(thread_a.CallbackRsp % 16 == 8);
        assert(MmGetPhysicalAddress(thread_a.UserCallbackBase) == MM_INVALID_PHYS);
        assert((MmGetPageFlags(thread_a.CallbackRsp) & (PTE_WRITE | PTE_NX)) == (PTE_WRITE | PTE_NX));
        assert(!(MmGetPageFlags(thread_a.CallbackRip) & (PTE_WRITE | PTE_NX)));
        UINT8 *code = MmPhysToVirt(MmGetPhysicalAddress(thread_a.CallbackRip));
        UINT64 immediate;
        memcpy(&immediate, code + 2, 8);
        assert(immediate == i && code[76] == 0x0f && code[77] == 0x05 && code[78] == 0xcc);
        assert(!KiStartUserCallback(0x1000, 0, 0, 0, 0, CB_CONT_SIMPLE, 0, 0, 0));
        thread_a.RedirectPending = 0; /* emulate entry assembly consuming redirect */
        original = 0xdeadbeef; /* zero return slot must also be restored */
        UINT64 result[16] = {i};
        assert(NtCallbackReturn(result) == i);
        assert(!thread_a.CallbackActive && original == 0);
        assert(thread_a.CallbackRip == 0x1234 && thread_a.CallbackRsp == (UINT64)&original);
    }
    assert(page_allocations == allocations + CALLBACK_MAX_DEPTH * CALLBACK_STACK_PAGES + 1);
    prepare_callback(&thread_a, &original);
    UINT64 create_args[16] = {0}; create_args[12] = 0xabc;
    UINT64 cs = KiBuildCreateStruct(create_args, 0x10001);
    assert(cs && *(UINT64 *)cs == 0xabc && cs < thread_a.UserCallbackBase + CALLBACK_THUNK_OFFSET);
    assert(KiBuildCreateStruct(create_args, 0x10001) == cs);
    assert(page_allocations == allocations + CALLBACK_MAX_DEPTH * CALLBACK_STACK_PAGES + 1);
    assert(!KiStartUserCallback(0xdead000, 0, 0, 0, 0, CB_CONT_SIMPLE, 0, 0, 0));
    assert(!KiStartUserCallback(MM_USER_MAX, 0, 0, 0, 0, CB_CONT_SIMPLE, 0, 0, 0));
    UINT64 slots[8] = {0}, thunks[8] = {0}, stacks[8] = {0};
    for (unsigned level = 0; level < 8; ++level) {
        prepare_callback(&thread_a, &slots[level]);
        thread_a.UserRip = 0x2000 + level;
        assert(KiStartUserCallback(0x1000, level, 2, 3, 4,
                                  CB_CONT_SIMPLE, level + 1, 0, 0));
        thunks[level] = thread_a.CallbackRip; stacks[level] = thread_a.CallbackRsp;
        assert(thread_a.UserCallbackDepth == level + 1);
        UINT64 top=thread_a.UserCallbackBase+CALLBACK_THUNK_OFFSET-level*CALLBACK_STACK_SLICE;
        for(UINT32 page=1;page<=16;page++){
            UINT64 va=top-page*PAGE_SIZE;
            assert((MmGetPageFlags(va)&(PTE_WRITE|PTE_NX))==(PTE_WRITE|PTE_NX));
            *(volatile UINT64*)va=0x12340000+level;
        }
        assert(MmGetPhysicalAddress(top-17*PAGE_SIZE)==MM_INVALID_PHYS);
        if (level) {
            assert(thunks[level] - thunks[level-1] == CALLBACK_THUNK_STRIDE);
            assert(stacks[level-1] - stacks[level] == CALLBACK_STACK_SLICE);
        }
    }
    thread_a.RedirectPending = 0;
    assert(!KiStartUserCallback(0x1000, 0, 0, 0, 0, CB_CONT_SIMPLE, 0, 0, 0));
    for (unsigned level = 8; level; --level) {
        thread_a.RedirectPending = 0;
        UINT64 nested_result[16] = {level};
        assert(NtCallbackReturn(nested_result) == level);
        assert(thread_a.CallbackRip == 0x2000 + level - 1);
        assert(thread_a.CallbackRsp == (UINT64)&slots[level-1]);
        assert(thread_a.UserCallbackDepth == level - 1);
        if (level > 1) assert(thread_a.CallbackWindow == level - 1);
    }
    assert(!thread_a.CallbackActive);
    prepare_callback(&thread_a, &original);
    puts("PASS eight nested callbacks: separate thunks, guarded stacks and continuation unwind");
    puts("PASS 1000 callbacks: resource reuse, alignment, RW/NX stack, RX thunk, zero return restore");

    WCHAR class_name[] = {'C','b','C','l','a','s','s',0};
    UNICODE_STRING name = {14, 16, class_name};
    UINT64 wcex[10] = {0}; wcex[1] = 0x1000;
    UINT64 class_args[16] = {(UINT64)wcex, (UINT64)&name};
    UINT64 atom = NtUserRegisterClassExWOW(class_args); assert(atom);
    UINT64 result[16] = {0};
    for (unsigned mode = 0; mode < 4; mode++) {
        prepare_callback(&thread_a, &original);
        memset(create_args, 0, sizeof(create_args)); create_args[1] = atom;
        assert(NtUserCreateWindowEx(create_args) == 0 && thread_a.CallbackActive);
        UINT64 hwnd = thread_a.CallbackWindow;
        assert(KiUserWindowValid(hwnd, TRUE));
        thread_a.RedirectPending = 0;
        result[0] = mode == 0 ? 0 : 1;
        if (mode == 2) {
            UINT64 head = PROCESS_USER_OBJECT_ARENA_VA + (UINT64)(UINT16)hwnd * 0x200;
            *(UINT64 *)(head + WND_WNDPROC_OFF) = 0xdead000; /* non-executable next target */
        }
        assert(NtCallbackReturn(result) == 0);
        if (mode == 1 || mode == 3) {
            assert(thread_a.CallbackActive && thread_a.CallbackContinue == CB_CONT_CREATE_WMCREATE);
            thread_a.RedirectPending = 0;
            result[0] = mode == 3 ? (UINT64)-1 : 0;
            if (mode == 3) {
                assert(!NtCallbackReturn(result) && thread_a.CallbackMsg == 2);
                thread_a.RedirectPending = 0; result[0] = 0;
                assert(!NtCallbackReturn(result) && thread_a.CallbackMsg == 0x82);
                thread_a.RedirectPending = 0;
                assert(!NtCallbackReturn(result) && !KiUserWindowValid(hwnd, FALSE));
                continue;
            }
            assert(NtCallbackReturn(result) == hwnd);
            UINT64 destroy[16] = {hwnd};
            prepare_callback(&thread_a, &original);
            assert(!NtUserDestroyWindow(destroy) && thread_a.CallbackMsg == 2);
            thread_a.RedirectPending = 0;
            assert(!NtCallbackReturn(result) && thread_a.CallbackMsg == 0x82);
            thread_a.RedirectPending = 0;
            assert(NtCallbackReturn(result) == 1 && !KiUserWindowValid(hwnd, FALSE));
        } else {
            assert(!thread_a.CallbackActive && !KiUserWindowValid(hwnd, FALSE));
        }
    }
    prepare_callback(&thread_a, &original);
    assert(page_allocations == allocations + CALLBACK_MAX_DEPTH * CALLBACK_STACK_PAGES + 1);
    puts("PASS creation callback chain, refusal, failed dispatch and teardown after WM_CREATE failure");

    /* A parent creates a child inside WM_CREATE, then refuses creation.
     * Unwind the nested creation and require both teardown messages per HWND. */
    prepare_callback(&thread_a, &original);
    memset(create_args, 0, sizeof(create_args)); create_args[1] = atom;
    NtUserCreateWindowEx(create_args);
    UINT64 parent_hwnd = thread_a.CallbackWindow;
    UINT64 parent_cs = thread_a.CallbackLParam;
    thread_a.RedirectPending = 0; result[0] = 1; NtCallbackReturn(result);
    assert(thread_a.CallbackContinue == CB_CONT_CREATE_WMCREATE);
    UINT64 nested_return = 0x4567;
    prepare_callback(&thread_a, &nested_return);
    create_args[4] = WS_CHILD; create_args[9] = parent_hwnd;
    NtUserCreateWindowEx(create_args);
    UINT64 child_hwnd = thread_a.CallbackWindow;
    assert(thread_a.UserCallbackDepth == 2 && child_hwnd != parent_hwnd);
    assert(thread_a.CallbackLParam != parent_cs && *(UINT64 *)(parent_cs+0x18) == 0);
    thread_a.RedirectPending = 0; result[0] = 1; NtCallbackReturn(result);
    thread_a.RedirectPending = 0; result[0] = 0;
    assert(NtCallbackReturn(result) == child_hwnd && thread_a.UserCallbackDepth == 1);
    assert(thread_a.CallbackWindow == parent_hwnd && thread_a.CallbackContinue == CB_CONT_CREATE_WMCREATE);
    thread_a.RedirectPending = 0; result[0] = (UINT64)-1; NtCallbackReturn(result);
    UINT64 order_hwnd[] = {parent_hwnd, child_hwnd, child_hwnd, parent_hwnd};
    UINT64 order_msg[] = {2, 2, 0x82, 0x82};
    for (unsigned i = 0; i < 4; ++i) {
        assert(thread_a.CallbackWindow == order_hwnd[i] && thread_a.CallbackMsg == order_msg[i]);
        thread_a.RedirectPending = 0; result[0] = 0; NtCallbackReturn(result);
    }
    assert(!thread_a.CallbackActive && !KiUserWindowValid(parent_hwnd, FALSE) && !KiUserWindowValid(child_hwnd, FALSE));
    puts("PASS nested child creation during WM_CREATE and ordered teardown after parent refusal");

    KTHREAD *pool = calloc(16, sizeof(*pool)); assert(pool);
    for (unsigned i = 0; i < 16; i++) {
        pool[i].Process = &process_a; pool[i].ThreadId = 30000 + i;
        current = &pool[i];
        assert((KiClaimCallbackStack() != 0) == (i < 15));
    }
    current = &thread_a;
    for (unsigned i = 0; i < 16; i++) W32kCleanupThreadUser(&pool[i]);
    free(pool);
    assert(page_allocations - page_frees == CALLBACK_MAX_DEPTH * CALLBACK_STACK_PAGES + 1);
    puts("PASS per-process concurrent slot limit and thread slot reclamation");

    UINT64 base_a = thread_a.UserCallbackBase;
    switch_callback_process(&thread_b);
    prepare_callback(&thread_b, &original_b);
    assert(KiStartUserCallback(0x2000, 7, 8, 9, 10, CB_CONT_SIMPLE, 0, 0, 0));
    assert(thread_b.UserCallbackBase == base_a); /* same VA, different physical pages */
    W32kCleanupThreadUser(&thread_a); /* foreign-root unmap */
    assert(!thread_a.UserCallbackBase && thread_b.UserCallbackBase == base_a && thread_b.CallbackActive);
    UINT8 *code = MmPhysToVirt(MmGetPhysicalAddress(thread_b.CallbackRip));
    UINT64 target; memcpy(&target, code + 58, 8); assert(target == 0x2000);
    thread_b.RedirectPending = 0;
    result[0] = 42;
    assert(NtCallbackReturn(result) == 42);
    W32kCleanupThreadUser(&thread_b);
    assert(page_allocations == page_frees);
    switch_callback_process(&thread_a);
    puts("PASS two-process callback isolation and cleanup while other callback is active");

    for (unsigned i = 0; i < 40; i++) {
        prepare_callback(&thread_a, &original);
        assert(KiStartUserCallback(0x1000, 0, 0, 0, 0, CB_CONT_SIMPLE, 0, 0, 0));
        W32kCleanupThreadUser(&thread_a); /* termination during a callback */
        assert(!thread_a.UserCallbackBase && !thread_a.CallbackActive && !thread_a.RedirectPending);
    }
    assert(page_allocations == page_frees);
    fail_allocate_after = 5;
    assert(!KiClaimCallbackStack());
    assert(!thread_a.UserCallbackBase && page_allocations == page_frees);
    fail_allocate_after = -1; fail_map_after = 5;
    assert(!KiClaimCallbackStack());
    assert(!thread_a.UserCallbackBase && page_allocations == page_frees);
    fail_map_after = -1;
    process_a.VmRegions[0].InUse = TRUE;
    process_a.VmRegions[0].Base = CALLBACK_STACK_ARENA_VA;
    process_a.VmRegions[0].ReserveSize = CALLBACK_STACK_SLOT_SIZE;
    assert(KiClaimCallbackStack() == CALLBACK_STACK_ARENA_VA + CALLBACK_STACK_SLOT_SIZE + CALLBACK_THUNK_OFFSET);
    W32kCleanupThreadUser(&thread_a);
    process_a.VmRegions[0].InUse = FALSE;
    assert(page_allocations == page_frees);
    puts("PASS callback termination, repeated reuse, allocation/map rollback and reserve collision");
}

static void check_screen_z_order(void)
{
    memset(g_w32k, 0, sizeof(g_w32k));
    g_w32k_top = 0;
    g_w32k[1] = (W32K_WINDOW_STATE){.Kind = W32K_KIND_DESKTOP,
                                    .Visible = TRUE, .W = 100, .H = 100};
    g_w32k[2] = (W32K_WINDOW_STATE){.Kind = W32K_KIND_TASKBAR,
                                    .Visible = TRUE, .Y = 80, .W = 100, .H = 20};
    g_w32k[3] = (W32K_WINDOW_STATE){.Kind = W32K_KIND_TASKBAR,
                                    .Visible = TRUE, .Y = 20, .W = 60, .H = 60};
    g_w32k[4] = (W32K_WINDOW_STATE){.Kind = W32K_KIND_TOPLEVEL,
                                    .Visible = TRUE, .X = 20, .Y = 30,
                                    .W = 60, .H = 40};
    W32kRaise(1);
    W32kRaise(2);
    W32kRaise(3);
    W32kRaise(4);
    W32kRaise(3); /* raising the top entry must not link it to itself */
    assert(g_w32k_top == 3 && g_w32k[3].ZPrev == 2 &&
           g_w32k[3].ZNext == 0 && g_w32k[2].ZPrev == 4 &&
           g_w32k[4].ZPrev == 1 && g_w32k[1].ZPrev == 0);
    assert(!W32kScreenPixelVisible(4, 25, 35));
    assert(W32kScreenPixelVisible(3, 25, 35));
    assert(W32kScreenPixelVisible(2, 25, 85));
    assert(!W32kScreenRectUnobscured(4, 20, 30, 60, 40));
    assert(W32kScreenRectUnobscured(4, 65, 30, 10, 20));
    W32kRaise(2);
    assert(g_w32k_top == 2 && g_w32k[2].ZNext == 0 &&
           g_w32k[3].ZNext == 2);
    memset(g_w32k, 0, sizeof(g_w32k));
    g_w32k_top = 0;
    puts("PASS shell z-order, repeated raise and client occlusion");
}

/* A partial update must produce exactly the same pixels as rebuilding all
 * layers. Real window rasters and chrome run here, with a padded framebuffer
 * to catch pitch assumptions and sanitizer coverage for off-screen moves. */
static void check_damage_matches_full(void)
{
    SIZE_T bytes = (SIZE_T)GfxFramebuffer.Pitch * GfxFramebuffer.Height;
    UINT32 *partial = malloc(bytes);
    assert(partial);
    memcpy(partial, GfxFramebuffer.Base, bytes);
    W32kScreenCompose();
    assert(!memcmp(partial, GfxFramebuffer.Base, bytes));
    free(partial);
}

/* Rendering fixtures use an explicit server-side class; production calls
 * must no longer succeed with NULL or unregistered class arguments. */
static UINT64 create_fixture_window(UINT64 *args)
{
    if(!args[1]){
        USER_CLASS_LOCAL *cls=0;
        for(UINT32 i=0;i<USER_MAX_CLASSES;i++)if(g_user_classes[i].Used&&g_user_classes[i].Process==current->Process&&g_user_classes[i].Atom==0xdfff){cls=&g_user_classes[i];break;}
        if(!cls)for(UINT32 i=0;i<USER_MAX_CLASSES;i++)if(!g_user_classes[i].Used){cls=&g_user_classes[i];*cls=(USER_CLASS_LOCAL){.Used=TRUE,.Process=current->Process,.Atom=0xdfff};break;}
        assert(cls);args[1]=cls->Atom;
    }
    return NtUserCreateWindowEx(args);
}

static void check_retained_window_painting(void)
{
    enum { WIDTH = 240, HEIGHT = 180, STRIDE = WIDTH + 3 };
    UINT32 *screen = calloc(STRIDE * HEIGHT, sizeof(UINT32));
    assert(screen);
    GfxFramebuffer.Base = screen;
    GfxFramebuffer.Width = WIDTH; GfxFramebuffer.Height = HEIGHT;
    GfxFramebuffer.Pitch = STRIDE * 4;
    graphics = TRUE;
    UINT64 args[16] = {0};
    args[4] = WS_CAPTION | WS_SYSMENU;
    args[5] = 20; args[6] = 20; args[7] = 190; args[8] = 100;
    UINT64 hwnd = create_fixture_window(args);
    assert(hwnd);
    UINT16 slot = (UINT16)hwnd;
    W32kShowWindow(hwnd, 1);
    WCHAR title[] = {'A','A','A','A','A','A','A','A','A','A','A',0};
    W32kScreenSetTitle(slot, title);
    INT32 cx, cy, cw, ch;
    W32kClientOrigin(slot, &cx, &cy, &cw, &ch);
    UINT32 *pixels = malloc((SIZE_T)cw * ch * 4);
    UINT32 *before = malloc(STRIDE * HEIGHT * 4);
    assert(pixels && before);
    for (INT32 row = 0; row < ch; ++row)
        for (INT32 col = 0; col < cw; ++col)
            pixels[row * cw + col] = 0x406080 + ((UINT32)row << 8) + col;
    UINT8 ps[PAINTSTRUCT_SIZE];
    args[0] = hwnd; args[1] = (UINT64)ps;
    UINT64 hdc = NtUserBeginPaint(args);
    assert(hdc);
    memcpy(before, screen, STRIDE * HEIGHT * 4);
    UINT64 blit[] = {hdc, (UINT64)pixels, (UINT64)cw, (UINT64)ch,
                    0, 0, (UINT64)cw, (UINT64)ch};
    assert(NtGdiBlitUserBuffer(blit));
    /* A compositor pass during WM_PAINT must still show the previous front. */
    W32kScreenCompose();
    assert(!memcmp(before, screen, STRIDE * HEIGHT * 4));
    assert(NtUserEndPaint(args));
    assert(screen[cy * STRIDE + cx] == pixels[0]);
    assert(!NtUserEndPaint(args));
    check_damage_matches_full();
    puts("PASS complete paints publish atomically and client coordinates are retained");

    UINT8 updates[USER_PAINT_SLOTS];
    memcpy(updates, g_window_update, sizeof(updates));
    for (int i = 0; i < 30; ++i) {
        INT32 x = (i * 17) % 95 - 25, y = (i * 13) % 85 - 12;
        W32kMoveWindow(hwnd, x, y, 190, 100);
        W32kClientOrigin(slot, &cx, &cy, &cw, &ch);
        INT32 px = cx < 0 ? 0 : cx, py = cy < 0 ? 0 : cy;
        assert(screen[py * STRIDE + px] == pixels[(py - cy) * cw + px - cx]);
        check_damage_matches_full();
        assert(!memcmp(updates, g_window_update, sizeof(updates)));
    }
    /* Damage in the client must not redraw a title/glyph outside that box. */
    memcpy(before, screen, STRIDE * HEIGHT * 4);
    W32kScreenDamage(5, 100, 2, 2);
    assert(!memcmp(before, screen, STRIDE * HEIGHT * 4));
    for (int i = 0; i < 25; ++i) {
        W32kInputMouse((i * 37) % WIDTH, (i * 23) % HEIGHT, 0);
        check_damage_matches_full();
        assert(!memcmp(updates, g_window_update, sizeof(updates)));
    }
    puts("PASS moving/clipped windows, shadow bounds and cursor leave no trails or paint storm");

    args[0] = hwnd; args[1] = 0;
    hdc = NtUserGetDC(args); /* refresh after movement, still local coordinates */
    assert(hdc);
    UINT64 pixel_args[] = {hdc, 20, 4, 0x00112233};
    assert(NtGdiSetPixel(pixel_args) == 0x00112233);
    W32kClientOrigin(slot, &cx, &cy, &cw, &ch);
    assert(screen[(cy + 4) * STRIDE + cx + 20] == 0x00332211);
    check_damage_matches_full();

    /* A Shell_* popup sits over ordinary windows. Painting a covered client
     * must update its raster without overwriting the popup. */
    memset(args, 0, sizeof(args));
    args[4] = WS_POPUP; args[5] = 0; args[6] = 40;
    args[7] = 140; args[8] = 100;
    UINT64 popup = create_fixture_window(args);
    assert(popup);
    g_w32k[(UINT16)popup].Kind = W32K_KIND_TASKBAR;
    W32kShowWindow(popup, 1);
    args[0] = popup; args[1] = (UINT64)ps;
    UINT64 popup_dc = NtUserBeginPaint(args);
    assert(popup_dc);
    UINT64 fill[] = {popup_dc, 0, 0, 140, 100, 0x00000042};
    assert(NtGdiPatBlt(fill));
    assert(NtUserEndPaint(args));
    args[0] = hwnd;
    assert(NtUserBeginPaint(args));
    fill[0] = hdc; fill[3] = cw; fill[4] = ch; fill[5] = 0x00FF0062;
    assert(NtGdiPatBlt(fill));
    assert(NtUserEndPaint(args));
    assert(screen[75 * STRIDE + 60] == 0);
    W32kShowWindow(popup, 0);
    assert(screen[75 * STRIDE + 60] == 0xFFFFFF);
    check_damage_matches_full();
    W32kShowWindow(hwnd, 0); check_damage_matches_full();
    W32kShowWindow(hwnd, 1); check_damage_matches_full();
    puts("PASS GetDC drawing, covered paints, popup hide and window hide/restore");
    W32kMoveWindow(hwnd, 20, 20, 190, 100);
    W32kInputMouse(60, 30, 0);
    W32kInputMouse(60, 30, 1);
    assert(g_w32k_drag == slot);
    W32kInputMouse(220, 160, 1); /* leaves both old window bounds at once */
    assert(g_w32k[slot].X == 180 && g_w32k[slot].Y == 142);
    check_damage_matches_full();
    W32kInputMouse(220, 160, 0);
    assert(!g_w32k_drag && !g_w32k[slot].Dragging);
    puts("PASS caption drag retains its window across large pointer jumps");
    W32K_DC *retained = dc_from_handle(hdc);
    assert(retained);
    void *old_storage = retained->WindowStorage;
    W32kMoveWindow(hwnd, 12, 15, 198, 110);
    fail_pool_after = 0;
    args[0] = hwnd;
    assert(!NtUserGetDC(args));
    assert(g_window_dc[slot] == hdc && retained->WindowStorage == old_storage);
    check_damage_matches_full();
    fail_pool_after = -1;
    assert(NtUserGetDC(args) == hdc);
    assert(retained->DeviceSurface.Width == 196 && retained->DeviceSurface.Height == 79);
    assert(retained->PresentedSurface.Bits[0] == 0xFFFFFF);
    check_damage_matches_full();
    args[1] = (UINT64)ps;
    assert(NtUserBeginPaint(args) == hdc);
    fill[0] = hdc; fill[3] = 196; fill[4] = 79; fill[5] = 0x00000042;
    assert(NtGdiPatBlt(fill));
    assert(NtUserEndPaint(args));
    check_damage_matches_full();
    puts("PASS resize preserves completed pixels and allocation failure retains cached DC");
    /* A child extends outside the parent's client: neither painting nor hit
     * testing may escape the parent, including after an ancestor is moved. */
    memset(args, 0, sizeof(args));
    args[4] = WS_CHILD | 0x10000000u;
    args[5] = (UINT64)-8; args[6] = 3; args[7] = 30; args[8] = 24; args[9] = hwnd;
    UINT64 child = create_fixture_window(args); assert(child);
    assert(W32kWindowParent((UINT16)child) == slot && W32kRootSlot((UINT16)child) == slot);
    UINT64 sibling_args[16]={0};sibling_args[4]=WS_CHILD;sibling_args[7]=10;sibling_args[8]=10;sibling_args[9]=hwnd;
    UINT64 sibling=create_fixture_window(sibling_args);assert(sibling);
    UINT64 relation[]={sibling,3};assert(NtUserWindowRelation(relation)==child);
    relation[0]=child;relation[1]=2;assert(NtUserWindowRelation(relation)==sibling);
    relation[0]=hwnd;relation[1]=5;assert(NtUserWindowRelation(relation)==child);
    relation[0]=child;relation[1]=1;assert(NtUserWindowRelation(relation)==sibling);
    relation[0]=sibling;relation[1]=0;assert(NtUserWindowRelation(relation)==child);
    g_w32k[(UINT16)sibling].Visible=TRUE;g_w32k[(UINT16)sibling].X=-8;g_w32k[(UINT16)sibling].Y=3;
    g_w32k[(UINT16)sibling].W=30;g_w32k[(UINT16)sibling].H=24;g_w32k[(UINT16)sibling].IsStatic=TRUE;
    W32kRaise((UINT16)sibling);INT32 hit_x,hit_y,hit_w,hit_h;W32kClientOrigin(slot,&hit_x,&hit_y,&hit_w,&hit_h);
    assert(W32kChildAt(slot,hit_x+1,hit_y+4,0)==(UINT16)child);
    g_w32k[(UINT16)sibling].Style|=0x100;assert(W32kChildAt(slot,hit_x+1,hit_y+4,0)==(UINT16)sibling);
    sibling_args[0]=sibling;assert(NtUserDestroyWindow(sibling_args));

    args[0] = child; args[1] = (UINT64)ps;
    UINT64 child_dc = NtUserBeginPaint(args); assert(child_dc);
    UINT64 child_fill[] = {child_dc, 0, 0, 30, 24, 0x00FF0062};
    assert(NtGdiPatBlt(child_fill) && NtUserEndPaint(args));
    for (int step = 0; step < 8; ++step) {
        W32kMoveWindow(hwnd, 12 + step * 3, 15 + step * 2, 198, 110);
        W32kClientOrigin(slot, &cx, &cy, &cw, &ch);
        assert(screen[(cy+5)*STRIDE+cx+1] == 0xFFFFFF);
        assert(screen[(cy+5)*STRIDE+cx+23] == 0);
        assert(W32kWindowAt(cx+1, cy+5) == (UINT16)child);
        assert(W32kWindowAt(cx-1, cy+5) == slot);
        check_damage_matches_full();
    }
    W32kSetFocus(child);
    assert(W32kFocusWindow() == (UINT16)child && W32kActiveSlot() == slot);
    W32kShowWindow(child, 0);
    assert(W32kFocusWindow() == slot && screen[(cy+5)*STRIDE+cx+1] == 0);
    check_damage_matches_full();
    W32kShowWindow(child, 1); W32kSetFocus(child);
    UINT8 child_updates[USER_PAINT_SLOTS]; memcpy(child_updates, g_window_update, sizeof(child_updates));
    W32kMoveWindow(hwnd, 35, 30, 198, 110);
    assert(!memcmp(child_updates, g_window_update, sizeof(child_updates)));
    check_damage_matches_full();
    puts("PASS child rasters: parent clipping, hit testing, focus, ancestor moves and hidden controls");
    args[0] = popup; assert(NtUserDestroyWindow(args));
    args[0] = hwnd; assert(NtUserDestroyWindow(args));
    assert(!g_window_dc[(UINT16)popup] && !g_window_dc[slot] && !KiUserWindowValid(child, FALSE));
    assert(!gdi_object(hdc, GDI_TYPE_DC));
    graphics = FALSE;
    free(g_w32k_composite); g_w32k_composite = NULL;
    g_w32k_composite_w = g_w32k_composite_h = 0;
    g_w32k_pointer_valid = FALSE;
    free(screen); free(pixels); free(before);
    GfxFramebuffer.Base = NULL;
    GfxFramebuffer.Width = GfxFramebuffer.Height = GfxFramebuffer.Pitch = 0;
}

static void check_menu_geometry(void)
{
    BOOLEAN saved_graphics=graphics;graphics=FALSE;
    memset(g_w32k,0,sizeof(g_w32k));g_w32k_top=0;
    g_w32k[1]=(W32K_WINDOW_STATE){.Kind=W32K_KIND_TOPLEVEL,.X=10,.Y=20,.W=200,.H=100,.Style=WS_CAPTION};
    g_w32k[2]=(W32K_WINDOW_STATE){.Kind=W32K_KIND_CHILD,.Parent=1,.X=5,.Y=6,.W=10,.H=10};
    INT32 x,y,w,h;W32kClientOrigin(1,&x,&y,&w,&h);INT32 old_y=y,old_h=h;
    WCHAR labels[64]={ 'V','i','e','w',0 };
    W32kSetMenuBar(1,labels,1,TRUE);labels[0]='X';
    assert(g_w32k[1].MenuLabels[0][0]=='V');
    W32kClientOrigin(1,&x,&y,&w,&h);
    assert(y==old_y+19&&h==old_h-19);
    BOOLEAN client=TRUE;assert(W32kHitTest(1,20,old_y+5,&client)==W32K_PART_MENU&&!client);
    W32kClientOrigin(2,&x,&y,&w,&h);assert(y==old_y+19+6);
    W32kSetMenuBar(1,NULL,0,FALSE);W32kClientOrigin(1,&x,&y,&w,&h);assert(y==old_y&&h==old_h);
    graphics=saved_graphics;
    puts("PASS menu snapshots, nonclient hit testing and child/client geometry");
}

int main(void)
{
    check_screen_z_order();
    GfxFramebuffer.RedShift = 16;
    GfxFramebuffer.GreenShift = 8;
    GfxFramebuffer.BlueShift = 0;
    assert(blend_pixel(0x000000, 0xFFFFFF, 0) == 0xFFFFFF);
    assert(blend_pixel(0x000000, 0xFFFFFF, 128) == 0x7F7F7F);
    assert(blend_pixel(0x000000, 0xFFFFFF, 255) == 0x000000);
    map_test_range(PROCESS_GDI_SHARED_TABLE_VA, PROCESS_GDI_SHARED_TABLE_SIZE);
    map_test_range(PROCESS_GDI_ATTR_VA, PROCESS_GDI_ATTR_SIZE);
    map_test_range(PROCESS_USER_SHARED_TABLE_VA, PROCESS_USER_SHARED_TABLE_SIZE);
    map_test_range(PROCESS_USER_OBJECT_ARENA_VA, PROCESS_USER_OBJECT_ARENA_SIZE);
    process_a.ProcessId = 8; process_b.ProcessId = 12;
    process_a.DirectoryTableBase = 8; process_b.DirectoryTableBase = 12;
    thread_a.Process = &process_a; thread_b.Process = &process_b;
    thread_a.ThreadId = 1000; thread_b.ThreadId = 2000;
    process_a.HandleTable = ObCreateHandleTable();
    process_b.HandleTable = ObCreateHandleTable();
    current = &thread_a;
    W32kInitializeGdi();
    UINT64 args[16] = {0x123456};
    UINT64 brush_a = NtGdiCreateSolidBrush(args);
    assert(brush_a && gdi_object(brush_a, GDI_TYPE_BRUSH));
    GDICELL64 cell;
    args[0] = brush_a; args[1] = (UINT64)&cell;
    assert(NtGdiGetEntry(args) == STATUS_SUCCESS && cell.ProcessId == 8);
    current = &thread_b;
    W32kInitializeGdi();
    assert(!gdi_object(brush_a, 0));
    assert((NTSTATUS)NtGdiGetEntry(args) == STATUS_INVALID_HANDLE);
    assert(!NtGdiDeleteObjectApp(args));
    args[0] = 0x654321;
    UINT64 brush_b = NtGdiCreateSolidBrush(args);
    current = &thread_a;
    assert(gdi_object(brush_a, GDI_TYPE_BRUSH));
    assert(!gdi_object(brush_b, 0));
    args[0] = brush_a;
    assert(NtGdiDeleteObjectApp(args));
    g_gdi_next_index = (UINT16)brush_a; /* force immediate slot reuse */
    UINT64 fresh = NtGdiCreateSolidBrush(args);
    assert(fresh != brush_a && !gdi_object(brush_a, 0));
    puts("PASS GDI process ownership, initialization isolation and stale handles");

    args[0] = 2; args[1] = 2;
    UINT64 bitmap = NtGdiCreateBitmap(args);
    assert(bitmap);
    args[0] = 0;
    UINT64 dc = NtGdiCreateCompatibleDC(args);
    assert(dc);
    args[0] = dc; args[1] = bitmap;
    NtGdiSelectBitmap(args);
    args[0] = bitmap;
    assert(!NtGdiDeleteObjectApp(args));
    args[0] = dc;
    assert(NtGdiDeleteObjectApp(args));
    args[0] = bitmap;
    unsigned before = freed;
    assert(NtGdiDeleteObjectApp(args) && freed == before + 1);
    for (unsigned i = 0; i < 600; i++) {
        args[0] = 0;
        dc = NtGdiCreateCompatibleDC(args);
        assert(dc);
        args[0] = dc;
        assert(NtGdiDeleteObjectApp(args));
    }
    current = &thread_b;
    W32kCleanupProcessGdi(&process_a); /* deliberately foreign address space */
    assert(gdi_object(brush_b, 0));
    current = &thread_a;
    assert(!gdi_object(fresh, 0));
    puts("PASS selected bitmap safety, reusable DC attributes, foreign-CR3 cleanup");

    USER_MSG_QUEUE *qa = KiUserQueueForThread(thread_a.ThreadId);
    assert(qa && qa->ThreadId == 1000);
    current = &thread_b;
    USER_MSG_QUEUE *qb = KiUserQueueForThread(thread_b.ThreadId);
    assert(qb && qb != qa);
    assert(KiUserEnqueueThreadMessage(1000, 0x123, 0, 0));
    USER_MSG_LOCAL msg;
    assert(!KiUserTakeMessage(&msg, TRUE));
    current = &thread_a;
    assert(KiUserTakeMessage(&msg, TRUE) && msg.Message == 0x123);
    W32kCleanupThreadUser(&thread_a);
    assert(!qa->ThreadId && qb->ThreadId == 2000);
    for (unsigned i = 0; i < 150; i++) {
        thread_a.ThreadId = 10000 + i;
        assert(KiUserQueueForThread(thread_a.ThreadId));
        W32kCleanupThreadUser(&thread_a);
    }
    puts("PASS queues with high TIDs, isolation and 150 cleanup/reuse cycles");

    HANDLE input = (HANDLE)KiUserGetInputEvent();
    assert(input);
    qa = KiUserQueueForThread(thread_a.ThreadId);
    PKEVENT retained = qa->InputEvent;
    assert(ObHeaderFromObject(retained)->PointerCount == 2);
    assert(NT_SUCCESS(ObCloseHandle(input)));
    assert(ObHeaderFromObject(retained)->PointerCount == 1);
    PKEVENT replacement;
    HANDLE replacement_handle;
    assert(NT_SUCCESS(PsCreateNotificationEvent(&replacement, &replacement_handle)));
    ObDereferenceObject(replacement); /* fixture keeps only its user handle */
    assert(replacement_handle == input);
    assert(KiUserEnqueueThreadMessage(thread_a.ThreadId, 0x234, 0, 0));
    assert(retained->Header.SignalState == 1);
    HANDLE repaired = (HANDLE)KiUserGetInputEvent();
    assert(repaired && repaired != replacement_handle);
    assert(ObHeaderFromObject(retained)->PointerCount == 2);
    current = &thread_b;
    W32kCleanupThreadUser(&thread_a);
    current = &thread_a;
    POBJECT referenced;
    assert(NT_SUCCESS(ObReferenceObjectByHandle(replacement_handle, 0, NULL, &referenced)));
    assert(referenced == replacement);
    ObDereferenceObject(referenced);
    assert(NT_SUCCESS(ObCloseHandle(replacement_handle)));
    puts("PASS input-event retained reference, repaired handle and safe close after reuse");

    WCHAR class_name[] = {'F','i','x','t','u','r','e',0};
    UNICODE_STRING name = {14, 16, class_name};
    UINT64 wcex[10] = {0};
    wcex[1] = 0x1111;
    memset(args, 0, sizeof(args));
    args[0] = (UINT64)wcex; args[1] = (UINT64)&name;
    UINT64 atom = NtUserRegisterClassExWOW(args);
    assert(atom);
    current = &thread_b;
    wcex[1] = 0x2222;
    assert(NtUserRegisterClassExWOW(args) == atom);
    assert(KiClassForCreate(atom)->WndProc == 0x2222);
    current = &thread_a;
    assert(KiClassForCreate(atom)->WndProc == 0x1111);
    memset(args, 0, sizeof(args));
    UINT8 teb[128]={0};UINT64 saved_teb=thread_a.UserGsBase;thread_a.UserGsBase=(UINT64)teb;
    assert(!NtUserCreateWindowEx(args)&&*(UINT32*)(teb+0x68)==1407);
    args[1]=0xffff;assert(!NtUserCreateWindowEx(args));args[1]=0;thread_a.UserGsBase=saved_teb;
    UINT64 hwnd_a = create_fixture_window(args);
    assert(hwnd_a && KiUserWindowValid(hwnd_a, TRUE));
    args[0] = hwnd_a;
    graphics = TRUE;
    UINT64 window_dc = NtUserGetDC(args);
    graphics = FALSE;
    assert(window_dc);
    current = &thread_b;
    assert(!NtUserDestroyWindow(args) && !NtUserGetDC(args));
    assert(!KiUserWindowValid(hwnd_a, TRUE));
    W32kCleanupThreadUser(&thread_a);
    assert(!KiUserWindowValid(hwnd_a, FALSE));
    current = &thread_a;
    assert(!gdi_object(window_dc, 0));
    memset(args, 0, sizeof(args));
    UINT64 hwnd_fresh = create_fixture_window(args);
    assert(hwnd_fresh && hwnd_fresh != hwnd_a && (UINT16)hwnd_fresh == (UINT16)hwnd_a);
    args[0] = hwnd_a;
    assert(!NtUserDestroyWindow(args));
    args[0] = hwnd_fresh;
    assert(NtUserDestroyWindow(args));
    puts("PASS class isolation, window ownership/generation and thread window/DC cleanup");
    check_retained_window_painting();

    args[0] = args[1] = 0;
    UINT64 cursor_a = NtUserFindExistingCursorIcon(args);
    current = &thread_b;
    UINT64 cursor_b = NtUserFindExistingCursorIcon(args);
    assert(cursor_a && cursor_b && cursor_a != cursor_b);
    args[0] = cursor_a;
    assert(!NtUserDestroyCursor(args));
    current = &thread_a;
    assert(NtUserDestroyCursor(args));
    W32kCleanupProcessUser(&process_a);
    current = &thread_b;
    assert(g_user_objects[(UINT16)cursor_b].Process == &process_b);
    W32kCleanupThreadUser(&thread_b);
    W32kCleanupProcessUser(&process_b);
    W32kCleanupProcessGdi(&process_b);
    W32kCleanupThreadUser(&thread_a);
    check_callbacks();
    ObDestroyHandleTable(process_a.HandleTable);
    ObDestroyHandleTable(process_b.HandleTable);
    puts("PASS USER object ownership and process cleanup isolation");
    check_menu_geometry();
    munmap((void *)PROCESS_USER_OBJECT_ARENA_VA, PROCESS_USER_OBJECT_ARENA_SIZE);
    munmap((void *)PROCESS_USER_SHARED_TABLE_VA, PROCESS_USER_SHARED_TABLE_SIZE);
    munmap((void *)PROCESS_GDI_ATTR_VA, PROCESS_GDI_ATTR_SIZE);
    munmap((void *)PROCESS_GDI_SHARED_TABLE_VA, PROCESS_GDI_SHARED_TABLE_SIZE);
    return 0;
}
