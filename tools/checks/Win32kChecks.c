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
static BOOLEAN graphics;
UINT64 MmDirectMapBase;
typedef struct _TEST_PAGE {
    void *Physical;
    UINT64 Root, Va, Flags;
    int Fd;
    BOOLEAN Mapped;
} TEST_PAGE;
static TEST_PAGE pages[1024];
static int fail_allocate_after = -1, fail_map_after = -1;
static unsigned page_allocations, page_frees;
GFX_FRAMEBUFFER GfxFramebuffer;
PKTHREAD KeGetCurrentThread(void) { return current; }
UINT64 KeGetTickCount(void) { return 0; }
void KeYield(void) { abort(); }
int KeLog(const char *fmt, ...) { (void)fmt; return 0; }
PVOID ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T size, ULONG tag)
{ (void)type; (void)tag; return malloc(size); }
void ExFreePool(PVOID p) { if (p) { freed++; free(p); } }
BOOLEAN MmProbeForRead(UINT64 va, UINT64 size) { (void)size; return va != 0; }
BOOLEAN MmProbeForWrite(UINT64 va, UINT64 size) { (void)size; return va != 0; }
void MmProtectRange(UINT64 base, UINT64 size, BOOLEAN writable)
{ (void)base; (void)size; (void)writable; }
BOOLEAN GfxAvailable(void) { return graphics; }
UINT32 GfxColor(UINT8 r, UINT8 g, UINT8 b) { return ((UINT32)r << 16) | ((UINT32)g << 8) | b; }
void GfxFillRect(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 color)
{ (void)x; (void)y; (void)w; (void)h; (void)color; }
void GfxDrawString(UINT32 x, UINT32 y, const char *s, UINT32 fg, UINT32 bg)
{ (void)x; (void)y; (void)s; (void)fg; (void)bg; }
LONG KeSetEvent(PKEVENT e) { LONG old = e->Header.SignalState; e->Header.SignalState = 1; return old; }
void KeResetEvent(PKEVENT e) { e->Header.SignalState = 0; }
void ObReferenceObject(POBJECT o) { ObHeaderFromObject(o)->PointerCount++; }
void ObDereferenceObject(POBJECT o)
{ if (!--ObHeaderFromObject(o)->PointerCount) ExFreePool(ObHeaderFromObject(o)); }
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
    for (unsigned i = 0; i < 1024; i++) {
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
    for (unsigned i = 0; i < 1024; i++) {
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
    for (unsigned i = 0; i < 1024; i++)
        if (pages[i].Mapped && pages[i].Root == root && pages[i].Va == PAGE_ALIGN(va))
            return (UINT64)pages[i].Physical + (va & PAGE_MASK);
    return MM_INVALID_PHYS;
}
UINT64 MmGetPhysicalAddress(UINT64 va)
{ return MmGetPhysicalAddressInAddressSpace(current->Process->DirectoryTableBase, va); }
UINT64 MmGetPageFlags(UINT64 va)
{
    if (va == 0xdead000) return PTE_PRESENT | PTE_USER | PTE_NX;
    for (unsigned i = 0; i < 1024; i++)
        if (pages[i].Mapped && pages[i].Root == current->Process->DirectoryTableBase &&
            pages[i].Va == PAGE_ALIGN(va)) return pages[i].Flags | PTE_PRESENT;
    return PTE_USER | PTE_PRESENT; /* fake executable WndProc fixture */
}
BOOLEAN MmUnmapPageInAddressSpace(UINT64 root, UINT64 va)
{
    for (unsigned i = 0; i < 1024; i++) {
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
    for (unsigned i = 0; i < 1024; i++) {
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
    for (unsigned i = 0; i < 1024; i++)
        if (pages[i].Mapped && pages[i].Root == current->Process->DirectoryTableBase)
            assert(!munmap((void *)pages[i].Va, PAGE_SIZE));
    current = thread;
    for (unsigned i = 0; i < 1024; i++) {
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
    assert(page_allocations == allocations + 31);
    prepare_callback(&thread_a, &original);
    UINT64 create_args[16] = {0}; create_args[12] = 0xabc;
    UINT64 cs = KiBuildCreateStruct(create_args, 0x10001);
    assert(cs && *(UINT64 *)cs == 0xabc && cs < thread_a.UserCallbackBase + CALLBACK_THUNK_OFFSET);
    assert(KiBuildCreateStruct(create_args, 0x10001) == cs);
    assert(page_allocations == allocations + 31);
    assert(!KiStartUserCallback(0xdead000, 0, 0, 0, 0, CB_CONT_SIMPLE, 0, 0, 0));
    assert(!KiStartUserCallback(MM_USER_MAX, 0, 0, 0, 0, CB_CONT_SIMPLE, 0, 0, 0));
    puts("PASS 1000 callbacks: resource reuse, alignment, RW/NX stack, RX thunk, zero return restore");

    WCHAR class_name[] = {'C','b','C','l','a','s','s',0};
    UNICODE_STRING name = {14, 16, class_name};
    UINT64 wcex[10] = {0}; wcex[1] = 0x1000;
    UINT64 class_args[16] = {(UINT64)wcex, (UINT64)&name};
    UINT64 atom = NtUserRegisterClassExWOW(class_args); assert(atom);
    UINT64 result[16] = {0};
    for (unsigned mode = 0; mode < 3; mode++) {
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
        if (mode == 1) {
            assert(thread_a.CallbackActive && thread_a.CallbackContinue == CB_CONT_CREATE_WMCREATE);
            thread_a.RedirectPending = 0;
            result[0] = 0;
            assert(NtCallbackReturn(result) == hwnd);
            UINT64 destroy[16] = {hwnd};
            assert(NtUserDestroyWindow(destroy));
        } else {
            assert(!thread_a.CallbackActive && !KiUserWindowValid(hwnd, FALSE));
        }
    }
    prepare_callback(&thread_a, &original);
    assert(page_allocations == allocations + 31);
    puts("PASS creation callback chain, WM_NCCREATE refusal and failed WM_CREATE dispatch");

    KTHREAD *pool = calloc(16, sizeof(*pool)); assert(pool);
    for (unsigned i = 0; i < 16; i++) {
        pool[i].Process = &process_a; pool[i].ThreadId = 30000 + i;
        current = &pool[i];
        assert((KiClaimCallbackStack() != 0) == (i < 15));
    }
    current = &thread_a;
    for (unsigned i = 0; i < 16; i++) W32kCleanupThreadUser(&pool[i]);
    free(pool);
    assert(page_allocations - page_frees == 31);
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

int main(void)
{
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
    UINT64 hwnd_a = NtUserCreateWindowEx(args);
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
    UINT64 hwnd_fresh = NtUserCreateWindowEx(args);
    assert(hwnd_fresh && hwnd_fresh != hwnd_a && (UINT16)hwnd_fresh == (UINT16)hwnd_a);
    args[0] = hwnd_a;
    assert(!NtUserDestroyWindow(args));
    args[0] = hwnd_fresh;
    assert(NtUserDestroyWindow(args));
    puts("PASS class isolation, window ownership/generation and thread window/DC cleanup");

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
    munmap((void *)PROCESS_USER_OBJECT_ARENA_VA, PROCESS_USER_OBJECT_ARENA_SIZE);
    munmap((void *)PROCESS_USER_SHARED_TABLE_VA, PROCESS_USER_SHARED_TABLE_SIZE);
    munmap((void *)PROCESS_GDI_ATTR_VA, PROCESS_GDI_ATTR_SIZE);
    munmap((void *)PROCESS_GDI_SHARED_TABLE_VA, PROCESS_GDI_SHARED_TABLE_SIZE);
    return 0;
}
