/* Actual USER session and Ob object/handle code, deterministic process/window
 * fixtures. Checks attachment, isolation, access and lifetime, not artwork. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <svindows/Ke.h>
#include <svindows/Ob.h>
#include <svindows/Ex.h>
#include <svindows/Win32k.h>
#include <svindows/Session.h>

static UINT64 HostIrqSave(void) { return 0; }
static void HostIrqRestore(UINT64 flags) { (void)flags; }
static UINT64 HostAcquireRaw(PKSPIN_LOCK lock) { (void)lock; return 0; }
static void HostReleaseRaw(PKSPIN_LOCK lock, UINT64 flags) { (void)lock; (void)flags; }
#define KiIrqSave HostIrqSave
#define KiIrqRestore HostIrqRestore
#define KiAcquireRawSpinLock HostAcquireRaw
#define KiReleaseRawSpinLock HostReleaseRaw
#define WCHAR UINT16

static PKTHREAD current;
static KTHREAD thread_a, thread_b, thread_c;
static KPROCESS process_a, process_b, process_c;
static ULONG g_window_owner[128];
static PKPROCESS g_window_process[128];
static UINT16 parents[128];
static int fail_allocation_after = -1;
static unsigned deleted_stations, deleted_desktops, compositions;
static UINT16 active_slot;

PKTHREAD KeGetCurrentThread(void) { return current; }
PKTHREAD KeLookupThreadById(ULONG id)
{
    if (id == thread_a.ThreadId) return &thread_a;
    if (id == thread_b.ThreadId) return &thread_b;
    if (id == thread_c.ThreadId) return &thread_c;
    return NULL;
}
UINT64 KeGetTickCount(void) { return 0; }
int KeLog(const char *format, ...) { (void)format; return 0; }
NORETURN void KeBugCheck(ULONG code, const char *message)
{ fprintf(stderr, "bugcheck %u: %s\n", code, message); abort(); }
PVOID ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T bytes, ULONG tag)
{
    (void)type; (void)tag;
    if (!fail_allocation_after) return NULL;
    if (fail_allocation_after > 0) fail_allocation_after--;
    return malloc(bytes);
}
void ExFreePool(void *pointer) { free(pointer); }
BOOLEAN MmProbeForRead(UINT64 pointer, UINT64 bytes)
{ return !bytes || pointer != 0; }
BOOLEAN MmProbeForWrite(UINT64 pointer, UINT64 bytes)
{ return !bytes || pointer != 0; }
UINT16 W32kWindowParent(UINT16 slot) { return parents[slot]; }
UINT16 W32kActiveSlot(void) { return active_slot; }
void W32kSetFocus(UINT64 handle) { active_slot = (UINT16)handle; }
void W32kScreenCompose(void) { compositions++; }
void W32kInputDesktopChanged(PVOID previous, PVOID desktop)
{
    assert(previous && desktop && W32kGetInputDesktopIdentity() == desktop);
    W32kSetFocus(0); W32kScreenCompose();
}
static UINT64 KiUserWindowHandle(UINT16 slot) { return slot ? 0x10000u | slot : 0; }

#include "../../kernel/se/Security.c"
#include "../../kernel/ob/ObCore.c"
#include "../../kernel/ob/ObNamespace.c"
#include "../../kernel/ob/ObHandle.c"
#include "../../kernel/win32k/Session.inc"

static void deletion(PVOID object, BOOLEAN station)
{ assert(object); if (station) deleted_stations++; else deleted_desktops++; }

static void process_fixture(KPROCESS *process, KTHREAD *thread, ULONG pid, ULONG rid)
{
    memset(process, 0, sizeof(*process)); memset(thread, 0, sizeof(*thread));
    process->ProcessId = pid; process->SecurityUserRid = rid;
    process->HandleTable = ObCreateHandleTable(); assert(process->HandleTable);
    InitializeListHead(&process->ThreadListHead);
    thread->Process = process; thread->ThreadId = pid + 1;
    InitializeListHead(&thread->ProcessEntry);
    InsertTailList(&process->ThreadListHead, &thread->ProcessEntry);
}

static UINT64 create_desktop(const WCHAR *name, ULONG access)
{
    UINT64 args[6] = {(UINT64)name, 0, 0, 0, access, 0};
    return SvNtUserCreateDesktop(args);
}

static void window_fixture(UINT16 slot, KTHREAD *thread, UINT16 parent)
{
    assert(NT_SUCCESS(W32kBindWindowDesktop(slot, thread, parent)));
    g_window_owner[slot] = thread->ThreadId;
    g_window_process[slot] = thread->Process; parents[slot] = parent;
}
static void window_drop(UINT16 slot)
{
    g_window_owner[slot] = 0; g_window_process[slot] = NULL; parents[slot] = 0;
    W32kUnbindWindowDesktop(slot);
}

int main(void)
{
    ObInitialize();
    process_fixture(&process_a, &thread_a, 8, 1000);
    process_fixture(&process_b, &thread_b, 12, 1000);
    process_fixture(&process_c, &thread_c, 16, 2000);
    current = &thread_a;
    assert(NT_SUCCESS(W32kInitializeSessions()));
    W32kRegisterSessionDeleteNotify(deletion);
    assert(NT_SUCCESS(W32kInitializeProcessSession(&process_a, NULL, NULL)));
    assert(NT_SUCCESS(W32kInitializeThreadSession(&thread_a)));
    assert(NT_SUCCESS(W32kInitializeProcessSession(&process_b, &process_a, &thread_a)));
    assert(NT_SUCCESS(W32kInitializeThreadSession(&thread_b)));
    UINT64 args[6] = {0};
    UINT64 station = NtUserGetProcessWindowStation(args);
    UINT64 initial = NtUserGetThreadDesktop(args);
    assert(station && initial && station != initial);
    assert(thread_a.Win32Desktop == thread_b.Win32Desktop);
    WCHAR info[64]; ULONG needed = 0;
    args[0] = station; args[1] = 3; args[2] = (UINT64)info;
    args[3] = sizeof(info); args[4] = (UINT64)&needed;
    assert(NtUserGetObjectInformation(args) && info[0] == 'W' && needed == 28);
    puts("PASS executive station/desktop handles and inherited desktop identity");

    static const WCHAR work_name[] = {'W','o','r','k',0};
    UINT64 work = create_desktop(work_name, W32K_DESKTOP_ALL); assert(work);
    WCHAR names[64]; ULONG name_chars = 0;
    args[0] = station; args[1] = 0; args[2] = 0; args[3] = (UINT64)&name_chars;
    assert(!SvNtUserEnumDesktops(args) && name_chars == 14);
    args[1] = (UINT64)names; args[2] = 64;
    assert(SvNtUserEnumDesktops(args));
    assert(names[0] == 'D' && names[8] == 'W' && names[13] == 0);
    static const WCHAR mixed_case[] = {'w','O','R','k',0};
    args[0] = (UINT64)mixed_case; args[1] = args[2] = 0; args[3] = W32K_DESKTOP_ALL;
    UINT64 duplicate = NtUserOpenDesktop(args); assert(duplicate && duplicate != work);
    args[0] = duplicate; assert(NtUserCloseDesktop(args));
    puts("PASS dynamic desktop namespace, case-insensitive lookup and stable enumeration");

    window_fixture(1, &thread_a, 0); window_fixture(2, &thread_b, 0);
    args[0] = work; assert(!NtUserSetThreadDesktop(args));
    window_drop(1); assert(NtUserSetThreadDesktop(args));
    assert(!NtUserCloseDesktop(args));
    window_fixture(3, &thread_a, 0);
    assert(!NT_SUCCESS(W32kBindWindowDesktop(4, &thread_a, 2)));
    assert(!W32kWindowOnInputDesktop(3) && W32kWindowOnInputDesktop(2));
    assert(SvNtUserSwitchDesktop(args));
    assert(W32kWindowOnInputDesktop(3) && !W32kWindowOnInputDesktop(2));
    UINT64 windows[128]; args[0] = 0; args[1] = (UINT64)windows; args[2] = 128;
    assert(SvNtUserEnumDesktopWindows(args) == 1 && windows[0] == KiUserWindowHandle(3));
    args[0] = initial;
    assert(SvNtUserEnumDesktopWindows(args) == 1 && windows[0] == KiUserWindowHandle(2));
    assert(compositions == 1);
    puts("PASS window binding, cross-desktop parenting rejection and input/composition isolation");

    window_drop(3); args[0] = initial; assert(NtUserSetThreadDesktop(args));
    assert(SvNtUserSwitchDesktop(args));
    assert(!NtUserCloseDesktop(args));
    unsigned before = deleted_desktops;
    args[0] = work; assert(NtUserCloseDesktop(args));
    assert(deleted_desktops == before + 1);
    puts("PASS borrowed/busy handles and desktop destruction after final attachment");

    static const WCHAR private_name[] = {'P','r','i','v','a','t','e',0};
    args[0] = (UINT64)private_name; args[1] = 0; args[2] = W32K_STATION_ALL; args[3] = 0;
    UINT64 private_station = SvNtUserCreateWindowStation(args); assert(private_station);
    current = &thread_c;
    assert(NT_SUCCESS(W32kInitializeProcessSession(&process_c, NULL, NULL)));
    assert(!NtUserOpenWindowStation(args));
    current = &thread_a;
    args[0] = private_station;
    assert(NtUserSetProcessWindowStation(args));
    assert(!NtUserCloseWindowStation(args));
    static const WCHAR studio_name[] = {'S','t','u','d','i','o',0};
    UINT64 studio = create_desktop(studio_name, W32K_DESKTOP_ALL); assert(studio);
    args[0] = studio; assert(NtUserSetThreadDesktop(args));
    assert(!SvNtUserSwitchDesktop(args)); /* noninteractive station */
    W32kCleanupThreadSession(&thread_a);
    args[0] = studio; assert(NtUserCloseDesktop(args));
    assert(NT_SUCCESS(W32kInitializeThreadSession(&thread_a)));
    puts("PASS token-owner access, process station reassociation and noninteractive desktop protection");

    window_drop(2); W32kCleanupThreadSession(&thread_a);
    W32kCleanupThreadSession(&thread_b); W32kCleanupThreadSession(&thread_c);
    W32kCleanupProcessSession(&process_a); W32kCleanupProcessSession(&process_b); W32kCleanupProcessSession(&process_c);
    ObDestroyHandleTable(process_a.HandleTable); ObDestroyHandleTable(process_b.HandleTable); ObDestroyHandleTable(process_c.HandleTable);
    assert(deleted_stations == 1);
    puts("PASS process/thread cleanup releases user objects without lingering attachments");
    return 0;
}
