/* USER window stations/desktops are executive objects, rather than tokens in
 * a process's cursor table. Included after UserWindows and before callbacks. */
#include <svindows/Session.h>

#define W32K_STATION_KIND 1u
#define W32K_DESKTOP_KIND 2u
#define W32K_STATION_ALL 0x000F037Fu
#define W32K_DESKTOP_ALL 0x000F01FFu
#define W32K_READ_CONTROL 0x00020000u
#define W32K_WRITE_DAC 0x00040000u
#define W32K_WINSTA_ENUMDESKTOPS 0x0001u
#define W32K_WINSTA_READATTRIBUTES 0x0002u
#define W32K_WINSTA_CREATEDESKTOP 0x0008u
#define W32K_DESKTOP_READOBJECTS 0x0001u
#define W32K_DESKTOP_CREATEWINDOW 0x0002u
#define W32K_DESKTOP_ENUMERATE 0x0040u
#define W32K_DESKTOP_SWITCH 0x0100u

typedef struct _W32K_SESSION_OBJECT {
    ULONG Kind, OwnerRid, Flags, NameChars;
    WCHAR *Name;
    char *NamespaceName;
    LIST_ENTRY Entry;
} W32K_SESSION_OBJECT;

typedef struct _W32K_WINDOW_STATION {
    W32K_SESSION_OBJECT Common;
    LIST_ENTRY Desktops;
} W32K_WINDOW_STATION;

typedef struct _W32K_DESKTOP {
    W32K_SESSION_OBJECT Common;
    W32K_WINDOW_STATION *Station;
    ULONG AttachedThreads, WindowReferences;
    UINT64 SavedForeground;
} W32K_DESKTOP;

typedef struct _W32K_SESSION_HANDLE {
    struct _W32K_SESSION_HANDLE *Next;
    HANDLE Handle;
    PVOID Object;
    BOOLEAN Borrowed, Inherit;
} W32K_SESSION_HANDLE;

typedef struct _W32K_PROCESS_SESSION {
    PKPROCESS Process;
    W32K_WINDOW_STATION *Station;
    W32K_DESKTOP *DefaultDesktop;
    HANDLE StationHandle, DesktopHandle;
    W32K_SESSION_HANDLE *Handles;
} W32K_PROCESS_SESSION;

static POBJECT_TYPE g_w32k_station_type, g_w32k_desktop_type;
static struct _OBJECT_DIRECTORY *g_w32k_station_directory;
static LIST_ENTRY g_w32k_stations = {&g_w32k_stations, &g_w32k_stations};
static KSPIN_LOCK g_w32k_session_lock;
static W32K_WINDOW_STATION *g_w32k_default_station;
static W32K_DESKTOP *g_w32k_default_desktop, *g_w32k_input_desktop;
static W32K_DESKTOP *g_w32k_window_desktops[128];
static W32K_SESSION_DELETE_NOTIFY g_w32k_session_delete_notify;

/* Reuse the generated Unicode simple-uppercase data for ordinal, Unicode
 * name comparison. This is data shared with kernel32, not a user DLL call. */
#define g_unicode_upper g_w32k_session_unicode_upper
#include "../../user/w/kernel32/UnicodeUpper.inc"
#undef g_unicode_upper

static UINT64 W32kSessionLock(void)
{ return KiAcquireRawSpinLock(&g_w32k_session_lock); }
static void W32kSessionUnlock(UINT64 flags)
{ KiReleaseRawSpinLock(&g_w32k_session_lock, flags); }

static void W32kSessionError(ULONG error)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (thread && thread->UserGsBase &&
        MmProbeForWrite(thread->UserGsBase + 0x68, sizeof(error)))
        *(ULONG *)(thread->UserGsBase + 0x68) = error;
}

static ULONG W32kSessionDosError(NTSTATUS status)
{
    if (status == STATUS_ACCESS_DENIED) return 5;
    if (status == STATUS_INVALID_HANDLE || status == STATUS_OBJECT_TYPE_MISMATCH) return 6;
    if (status == STATUS_NO_MEMORY || status == STATUS_INSUFFICIENT_RESOURCES) return 8;
    if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND) return 2;
    if (status == STATUS_OBJECT_NAME_COLLISION) return 183;
    if (status == STATUS_BUFFER_TOO_SMALL) return 122;
    if (status == STATUS_ACCESS_VIOLATION) return 998;
    if (status == STATUS_SHARING_VIOLATION) return 170;
    return 87;
}

static WCHAR W32kSessionUpper(WCHAR value)
{
    SIZE_T left = 0, right = sizeof(g_w32k_session_unicode_upper) /
                              sizeof(g_w32k_session_unicode_upper[0]);
    while (left < right) {
        SIZE_T middle = left + (right - left) / 2;
        if (g_w32k_session_unicode_upper[middle].From < value) left = middle + 1;
        else right = middle;
    }
    if (left < sizeof(g_w32k_session_unicode_upper) /
               sizeof(g_w32k_session_unicode_upper[0]) &&
        g_w32k_session_unicode_upper[left].From == value)
        return g_w32k_session_unicode_upper[left].To;
    return value;
}

static BOOLEAN W32kSessionNameEqual(const WCHAR *a, const WCHAR *b)
{
    while (*a && *b && W32kSessionUpper(*a) == W32kSessionUpper(*b)) { a++; b++; }
    return !*a && !*b;
}

static NTSTATUS W32kSessionCopyName(const WCHAR *source, ULONG chars,
                                   WCHAR **out, char **utf8)
{
    WCHAR *name = ExAllocatePoolWithTag(NonPagedPool,
                        ((SIZE_T)chars + 1) * sizeof(WCHAR), 'maNU');
    char *encoded = ExAllocatePoolWithTag(NonPagedPool,
                        (SIZE_T)chars * 3 + 1, 'maNU');
    if (!name || !encoded) {
        if (name) ExFreePool(name);
        if (encoded) ExFreePool(encoded);
        return STATUS_NO_MEMORY;
    }
    memcpy(name, source, (SIZE_T)chars * sizeof(WCHAR)); name[chars] = 0;
    SIZE_T written = 0;
    for (ULONG i = 0; i < chars; i++) {
        UINT32 cp = source[i];
        if (!cp || cp == '\\' || cp == '/') { ExFreePool(name); ExFreePool(encoded); return STATUS_OBJECT_NAME_INVALID; }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (++i >= chars || source[i] < 0xDC00 || source[i] > 0xDFFF) {
                ExFreePool(name); ExFreePool(encoded); return STATUS_OBJECT_NAME_INVALID;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + source[i] - 0xDC00;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            ExFreePool(name); ExFreePool(encoded); return STATUS_OBJECT_NAME_INVALID;
        }
        if (cp < 0x80) encoded[written++] = (char)cp;
        else if (cp < 0x800) { encoded[written++] = (char)(0xC0 | (cp >> 6)); encoded[written++] = (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) {
            encoded[written++] = (char)(0xE0 | (cp >> 12));
            encoded[written++] = (char)(0x80 | ((cp >> 6) & 63));
            encoded[written++] = (char)(0x80 | (cp & 63));
        } else {
            encoded[written++] = (char)(0xF0 | (cp >> 18));
            encoded[written++] = (char)(0x80 | ((cp >> 12) & 63));
            encoded[written++] = (char)(0x80 | ((cp >> 6) & 63));
            encoded[written++] = (char)(0x80 | (cp & 63));
        }
    }
    encoded[written] = 0; *out = name; *utf8 = encoded;
    return STATUS_SUCCESS;
}

static NTSTATUS W32kSessionCaptureName(UINT64 address, WCHAR **name,
                                      char **utf8, ULONG *chars)
{
    if (!address) return STATUS_OBJECT_NAME_INVALID;
    ULONG length = 0;
    while (length < 32767) {
        if (!MmProbeForRead(address + (UINT64)length * 2, 2)) return STATUS_ACCESS_VIOLATION;
        if (!*(const WCHAR *)(address + (UINT64)length * 2)) break;
        length++;
    }
    if (!length || length == 32767) return STATUS_OBJECT_NAME_INVALID;
    if (!MmProbeForRead(address, ((UINT64)length + 1) * 2)) return STATUS_ACCESS_VIOLATION;
    NTSTATUS status = W32kSessionCopyName((const WCHAR *)address, length, name, utf8);
    if (NT_SUCCESS(status)) *chars = length;
    return status;
}

static ACCESS_MASK W32kSessionMapAccess(ULONG kind, ACCESS_MASK access)
{
    POBJECT_TYPE type = kind == W32K_STATION_KIND ? g_w32k_station_type : g_w32k_desktop_type;
    return type ? SeMapGenericAccess(access, &type->GenericMapping) : access;
}

static BOOLEAN W32kSessionAccessAllowed(W32K_SESSION_OBJECT *object,
                                        PKPROCESS process, ACCESS_MASK desired)
{
    if (!process) return FALSE;
    SE_DESCRIPTOR *descriptor = SeReferenceObjectDescriptor(object);
    ACCESS_MASK granted = 0;
    NTSTATUS status = SeAccessCheck(descriptor, process->SecurityUserRid, desired,
                                   &ObHeaderFromObject(object)->Type->GenericMapping, &granted);
    SeDereferenceDescriptor(descriptor);
    return NT_SUCCESS(status);
}

static NTSTATUS W32kSessionCaptureSecurity(UINT64 attributes,
    W32K_SESSION_OBJECT *object, BOOLEAN *inherit)
{
    SE_DESCRIPTOR *descriptor = NULL;
    NTSTATUS status = SeCaptureAttributes(attributes, object->OwnerRid, &descriptor, inherit);
    if (NT_SUCCESS(status)) {
        SeAssignObjectDescriptor(object, descriptor);
        object->OwnerRid = SeDescriptorOwnerRid(descriptor);
    }
    SeDereferenceDescriptor(descriptor);
    return status;
}

static void W32kSessionFreeCommon(W32K_SESSION_OBJECT *object)
{
    if (g_w32k_session_delete_notify)
        g_w32k_session_delete_notify(object, object->Kind == W32K_STATION_KIND);
    if (object->NamespaceName) ExFreePool(object->NamespaceName);
    if (object->Name) ExFreePool(object->Name);
}

static void W32kDeleteWindowStation(POBJECT raw)
{
    W32K_WINDOW_STATION *station = raw;
    UINT64 flags = W32kSessionLock();
    if (station->Common.Entry.Flink != &station->Common.Entry)
        RemoveEntryList(&station->Common.Entry);
    W32kSessionUnlock(flags);
    W32kSessionFreeCommon(&station->Common);
}

static void W32kDeleteDesktop(POBJECT raw)
{
    W32K_DESKTOP *desktop = raw;
    W32K_WINDOW_STATION *station = desktop->Station;
    UINT64 flags = W32kSessionLock();
    if (desktop->Common.Entry.Flink != &desktop->Common.Entry)
        RemoveEntryList(&desktop->Common.Entry);
    W32kSessionUnlock(flags);
    W32kSessionFreeCommon(&desktop->Common);
    if (station) ObDereferenceObject(station);
}

void W32kRegisterSessionDeleteNotify(W32K_SESSION_DELETE_NOTIFY notify)
{ g_w32k_session_delete_notify = notify; }

static W32K_WINDOW_STATION *W32kFindStationLocked(const WCHAR *name)
{
    for (PLIST_ENTRY entry = g_w32k_stations.Flink; entry != &g_w32k_stations; entry = entry->Flink) {
        W32K_WINDOW_STATION *station = CONTAINING_RECORD(entry, W32K_WINDOW_STATION, Common.Entry);
        if ((station == g_w32k_default_station || ObHeaderFromObject(station)->ParentDirectory) &&
            W32kSessionNameEqual(station->Common.Name, name)) return station;
    }
    return NULL;
}

static W32K_DESKTOP *W32kFindDesktopLocked(W32K_WINDOW_STATION *station,
                                         const WCHAR *name)
{
    for (PLIST_ENTRY entry = station->Desktops.Flink; entry != &station->Desktops; entry = entry->Flink) {
        W32K_DESKTOP *desktop = CONTAINING_RECORD(entry, W32K_DESKTOP, Common.Entry);
        if (W32kSessionNameEqual(desktop->Common.Name, name)) return desktop;
    }
    return NULL;
}

static NTSTATUS W32kCreateStationObject(WCHAR *name, char *utf8, ULONG chars,
    ULONG rid, ULONG flags_word, UINT64 security, W32K_WINDOW_STATION **out,
    BOOLEAN *inherit)
{
    POBJECT raw;
    NTSTATUS status = ObCreateObject(g_w32k_station_type, sizeof(W32K_WINDOW_STATION), &raw);
    if (!NT_SUCCESS(status)) { ExFreePool(name); ExFreePool(utf8); return status; }
    W32K_WINDOW_STATION *station = raw;
    station->Common = (W32K_SESSION_OBJECT){.Kind = W32K_STATION_KIND,
        .OwnerRid = rid, .Flags = flags_word, .NameChars = chars, .Name = name,
        .NamespaceName = utf8};
    InitializeListHead(&station->Common.Entry); InitializeListHead(&station->Desktops);
    status = W32kSessionCaptureSecurity(security, &station->Common, inherit);
    if (NT_SUCCESS(status)) {
        UINT64 lock = W32kSessionLock();
        if (W32kFindStationLocked(name)) status = STATUS_OBJECT_NAME_COLLISION;
        else {
            status = ObInsertObjectByName(g_w32k_station_directory, utf8, station);
            if (NT_SUCCESS(status)) InsertTailList(&g_w32k_stations, &station->Common.Entry);
        }
        W32kSessionUnlock(lock);
    }
    if (!NT_SUCCESS(status)) { ObDereferenceObject(raw); return status; }
    *out = station; return STATUS_SUCCESS;
}

static NTSTATUS W32kCreateDesktopObject(W32K_WINDOW_STATION *station,
    WCHAR *name, char *utf8, ULONG chars, ULONG rid, ULONG flags_word,
    UINT64 security, W32K_DESKTOP **out, BOOLEAN *inherit)
{
    POBJECT raw;
    NTSTATUS status = ObCreateObject(g_w32k_desktop_type, sizeof(W32K_DESKTOP), &raw);
    if (!NT_SUCCESS(status)) { ExFreePool(name); ExFreePool(utf8); return status; }
    W32K_DESKTOP *desktop = raw;
    desktop->Common = (W32K_SESSION_OBJECT){.Kind = W32K_DESKTOP_KIND,
        .OwnerRid = rid, .Flags = flags_word, .NameChars = chars, .Name = name,
        .NamespaceName = utf8};
    InitializeListHead(&desktop->Common.Entry);
    desktop->Station = station; ObReferenceObject(station);
    status = W32kSessionCaptureSecurity(security, &desktop->Common, inherit);
    if (NT_SUCCESS(status)) {
        UINT64 lock = W32kSessionLock();
        if (W32kFindDesktopLocked(station, name)) status = STATUS_OBJECT_NAME_COLLISION;
        else InsertTailList(&station->Desktops, &desktop->Common.Entry);
        W32kSessionUnlock(lock);
    }
    if (!NT_SUCCESS(status)) { ObDereferenceObject(raw); return status; }
    *out = desktop; return STATUS_SUCCESS;
}

static NTSTATUS W32kSessionDirectory(struct _OBJECT_DIRECTORY *parent,
    const char *name, const char *path, struct _OBJECT_DIRECTORY **out)
{
    NTSTATUS status = ObCreateDirectory(parent, name, out);
    if (status == STATUS_OBJECT_NAME_COLLISION) {
        POBJECT object;
        status = ObLookupObjectByName(path, &object);
        if (NT_SUCCESS(status)) {
            if (strcmp(ObHeaderFromObject(object)->Type->Name, "Directory")) {
                ObDereferenceObject(object); return STATUS_OBJECT_TYPE_MISMATCH;
            }
            *out = object;
        }
    }
    return status;
}

static NTSTATUS W32kCanCloseSessionObject(POBJECT object, HANDLE handle)
{
    PKTHREAD caller = KeGetCurrentThread();
    W32K_PROCESS_SESSION *context = caller && caller->Process ? caller->Process->Win32Session : NULL;
    if (!context) return STATUS_SUCCESS;
    for (W32K_SESSION_HANDLE *record = context->Handles; record; record = record->Next)
        if (record->Handle == handle && record->Object == object && record->Borrowed)
            return STATUS_HANDLE_NOT_CLOSABLE;
    if (((W32K_SESSION_OBJECT *)object)->Kind == W32K_STATION_KIND)
        return context->Station == object ? STATUS_SHARING_VIOLATION : STATUS_SUCCESS;
    if (caller->Win32Desktop == object && caller->UserDesktop == (UINT64)handle)
        return STATUS_SHARING_VIOLATION;
    LIST_ENTRY *head = &context->Process->ThreadListHead;
    for (PLIST_ENTRY entry = head->Flink; entry && entry != head; entry = entry->Flink) {
        PKTHREAD thread = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
        if (thread->Win32Desktop == object && thread->UserDesktop == (UINT64)handle)
            return STATUS_SHARING_VIOLATION;
    }
    return STATUS_SUCCESS;
}

NTSTATUS W32kInitializeSessions(void)
{
    if (g_w32k_default_station) return STATUS_SUCCESS;
    if (!g_w32k_station_type) g_w32k_station_type = ObCreateObjectType("WindowStation", W32kDeleteWindowStation);
    if (!g_w32k_desktop_type) g_w32k_desktop_type = ObCreateObjectType("Desktop", W32kDeleteDesktop);
    if (!g_w32k_station_type || !g_w32k_desktop_type) return STATUS_NO_MEMORY;
    SeConfigureType(g_w32k_station_type, 0x20303u, 0x2003Cu, 0x20060u, W32K_STATION_ALL);
    SeConfigureType(g_w32k_desktop_type, 0x20041u, 0x200BEu, 0x20100u, W32K_DESKTOP_ALL);
    g_w32k_station_type->CloseProcedure = W32kCanCloseSessionObject;
    g_w32k_desktop_type->CloseProcedure = W32kCanCloseSessionObject;
    g_w32k_station_type->UnlinkNameOnLastHandle = TRUE;
    struct _OBJECT_DIRECTORY *sessions, *session, *windows;
    NTSTATUS status = W32kSessionDirectory(ObRootDirectory, "Sessions", "\\Sessions", &sessions);
    if (!NT_SUCCESS(status)) return status;
    status = W32kSessionDirectory(sessions, "1", "\\Sessions\\1", &session);
    ObDereferenceObject(sessions); if (!NT_SUCCESS(status)) return status;
    status = W32kSessionDirectory(session, "Windows", "\\Sessions\\1\\Windows", &windows);
    ObDereferenceObject(session); if (!NT_SUCCESS(status)) return status;
    status = W32kSessionDirectory(windows, "WindowStations", "\\Sessions\\1\\Windows\\WindowStations", &g_w32k_station_directory);
    ObDereferenceObject(windows); if (!NT_SUCCESS(status)) return status;
    static const WCHAR station_name[] = {'W','i','n','S','t','a','0',0};
    static const WCHAR desktop_name[] = {'D','e','f','a','u','l','t',0};
    WCHAR *name; char *encoded; BOOLEAN inherit;
    status = W32kSessionCopyName(station_name, 7, &name, &encoded);
    if (!NT_SUCCESS(status)) return status;
    W32K_WINDOW_STATION *station;
    status = W32kCreateStationObject(name, encoded, 7, 1000, 1, 0, &station, &inherit);
    if (!NT_SUCCESS(status)) return status;
    /* The interactive station is shared by the session's user processes.
     * User-created objects use token-owner/DACL access checks. */
    SE_DESCRIPTOR *shared_descriptor = NULL;
    status = SeCreateUnrestrictedDescriptor(station->Common.OwnerRid, &shared_descriptor);
    if (!NT_SUCCESS(status)) { ObRemoveObjectName(station); ObDereferenceObject(station); return status; }
    SeAssignObjectDescriptor(station, shared_descriptor);
    SeDereferenceDescriptor(shared_descriptor);
    status = W32kSessionCopyName(desktop_name, 7, &name, &encoded);
    W32K_DESKTOP *desktop = NULL;
    if (NT_SUCCESS(status)) status = W32kCreateDesktopObject(station, name, encoded,
                         7, 1000, 0, 0, &desktop, &inherit);
    if (!NT_SUCCESS(status)) { ObRemoveObjectName(station); ObDereferenceObject(station); return status; }
    shared_descriptor = NULL;
    status = SeCreateUnrestrictedDescriptor(desktop->Common.OwnerRid, &shared_descriptor);
    if (!NT_SUCCESS(status)) { ObDereferenceObject(desktop); ObRemoveObjectName(station); ObDereferenceObject(station); return status; }
    SeAssignObjectDescriptor(desktop, shared_descriptor);
    SeDereferenceDescriptor(shared_descriptor);
    g_w32k_default_station = station; g_w32k_default_desktop = desktop;
    g_w32k_input_desktop = desktop; ObReferenceObject(desktop);
    KeLog("[session] WinSta0\\Default created with executive station/desktop objects\n");
    return STATUS_SUCCESS;
}

static NTSTATUS W32kSessionHandle(W32K_PROCESS_SESSION *context, POBJECT object,
    ACCESS_MASK access, BOOLEAN borrowed, BOOLEAN inherit, HANDLE *out)
{
    W32K_SESSION_OBJECT *common = object;
    access = W32kSessionMapAccess(common->Kind, access);
    if (!W32kSessionAccessAllowed(common, context->Process, access)) return STATUS_ACCESS_DENIED;
    W32K_SESSION_HANDLE *record = ExAllocatePoolWithTag(NonPagedPool, sizeof(*record), 'HaSU');
    if (!record) return STATUS_NO_MEMORY;
    NTSTATUS status = ObCreateHandleInTable(context->Process->HandleTable, object, access, out);
    if (!NT_SUCCESS(status)) { ExFreePool(record); return status; }
    if (inherit) ObSetHandleAttributesInTable(context->Process->HandleTable, *out,
                                               OB_HANDLE_INHERIT, OB_HANDLE_INHERIT);
    *record = (W32K_SESSION_HANDLE){.Handle = *out, .Object = object,
                                    .Borrowed = borrowed, .Inherit = inherit};
    UINT64 flags = W32kSessionLock(); record->Next = context->Handles;
    context->Handles = record; W32kSessionUnlock(flags);
    return STATUS_SUCCESS;
}

static W32K_SESSION_HANDLE *W32kSessionFindHandle(W32K_PROCESS_SESSION *context,
                                                HANDLE handle, PVOID object)
{
    for (W32K_SESSION_HANDLE *record = context->Handles; record; record = record->Next)
        if (record->Handle == handle && record->Object == object) return record;
    return NULL;
}

static void W32kFreeProcessSessionContext(W32K_PROCESS_SESSION *context);

NTSTATUS W32kInitializeProcessSession(PKPROCESS process, PKPROCESS parent,
                                      PKTHREAD creator)
{
    if (!process || !process->HandleTable) return STATUS_INVALID_PARAMETER;
    if (process->Win32Session) return STATUS_SUCCESS;
    NTSTATUS status = W32kInitializeSessions();
    if (!NT_SUCCESS(status)) return status;
    W32K_PROCESS_SESSION *context = ExAllocatePoolWithTag(NonPagedPool, sizeof(*context), 'coSU');
    if (!context) return STATUS_NO_MEMORY;
    memset(context, 0, sizeof(*context)); context->Process = process;
    W32K_PROCESS_SESSION *source = parent ? parent->Win32Session : NULL;
    context->Station = source ? source->Station : g_w32k_default_station;
    context->DefaultDesktop = creator && creator->Process == parent && creator->Win32Desktop ?
                             creator->Win32Desktop : source ? source->DefaultDesktop : g_w32k_default_desktop;
    if (context->DefaultDesktop && context->DefaultDesktop->Station != context->Station)
        context->DefaultDesktop = NULL;
    ObReferenceObject(context->Station);
    if (context->DefaultDesktop) ObReferenceObject(context->DefaultDesktop);
    status = W32kSessionHandle(context, context->Station, W32K_STATION_ALL,
                                TRUE, FALSE, &context->StationHandle);
    if (NT_SUCCESS(status) && context->DefaultDesktop)
        status = W32kSessionHandle(context, context->DefaultDesktop, W32K_DESKTOP_ALL,
                                    TRUE, FALSE, &context->DesktopHandle);
    if (!NT_SUCCESS(status)) {
        W32kFreeProcessSessionContext(context); return status;
    }
    PVOID expected = NULL;
    if (!__atomic_compare_exchange_n(&process->Win32Session, &expected, context, FALSE,
                                     __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        W32kFreeProcessSessionContext(context); return STATUS_SUCCESS;
    }
    process->WindowsSessionId = 1;
    return STATUS_SUCCESS;
}

static W32K_PROCESS_SESSION *W32kCurrentProcessSession(void)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (!thread || !thread->Process || thread->Process->Exiting) return NULL;
    NTSTATUS status = W32kInitializeProcessSession(thread->Process, NULL, NULL);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return NULL; }
    return thread->Process->Win32Session;
}

NTSTATUS W32kInitializeThreadSession(PKTHREAD thread)
{
    if (!thread || !thread->Process) return STATUS_INVALID_PARAMETER;
    if (thread->Win32Desktop) return STATUS_SUCCESS;
    NTSTATUS status = W32kInitializeProcessSession(thread->Process, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;
    W32K_PROCESS_SESSION *context = thread->Process->Win32Session;
    UINT64 flags = W32kSessionLock();
    if (thread->Win32Desktop) { W32kSessionUnlock(flags); return STATUS_SUCCESS; }
    if (!context->DefaultDesktop) { W32kSessionUnlock(flags); return STATUS_OBJECT_NAME_NOT_FOUND; }
    thread->Win32Desktop = context->DefaultDesktop; thread->UserDesktop = (UINT64)context->DesktopHandle;
    ObReferenceObject(thread->Win32Desktop); context->DefaultDesktop->AttachedThreads++;
    W32kSessionUnlock(flags); return STATUS_SUCCESS;
}

NTSTATUS W32kInitializeThreadSessionForCreator(PKTHREAD thread, PKTHREAD creator)
{
    if (!thread || !creator || creator->Process != thread->Process || !creator->Win32Desktop)
        return W32kInitializeThreadSession(thread);
    W32K_DESKTOP *desktop = creator->Win32Desktop;
    UINT64 flags = W32kSessionLock();
    if (!thread->Win32Desktop) {
        thread->Win32Desktop = desktop; thread->UserDesktop = creator->UserDesktop;
        ObReferenceObject(desktop); desktop->AttachedThreads++;
    }
    W32kSessionUnlock(flags); return STATUS_SUCCESS;
}

NTSTATUS W32kInheritSessionHandles(PKPROCESS process, PKPROCESS parent)
{
    if (!process || !parent || !process->HandleTable || !parent->HandleTable)
        return STATUS_INVALID_PARAMETER;
    return ObInheritHandles(parent->HandleTable, process->HandleTable);
}

static void W32kSessionUndoHandle(W32K_PROCESS_SESSION *context, HANDLE handle,
                                 POBJECT object)
{
    ObCloseHandleForObject(context->Process->HandleTable, handle, object);
    UINT64 flags = W32kSessionLock(); W32K_SESSION_HANDLE **link = &context->Handles;
    while (*link && ((*link)->Handle != handle || (*link)->Object != object)) link = &(*link)->Next;
    W32K_SESSION_HANDLE *record = *link;
    if (record) *link = record->Next;
    W32kSessionUnlock(flags); if (record) ExFreePool(record);
}

NTSTATUS W32kAssignProcessStartupDesktop(PKPROCESS process, const UINT16 *path)
{
    if (!process || !process->Win32Session) return STATUS_INVALID_PARAMETER;
    W32K_PROCESS_SESSION *context = process->Win32Session;
    if (!path || !*path) return context->DefaultDesktop ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
    ULONG length = 0, separator = 32767;
    while (path[length] && length < 32767) {
        if (path[length] == '\\') {
            if (separator != 32767) return STATUS_OBJECT_NAME_INVALID;
            separator = length;
        }
        length++;
    }
    if (length == 32767 || !length || !separator || separator == length - 1) return STATUS_OBJECT_NAME_INVALID;
    WCHAR *station_name = NULL;
    if (separator != 32767) {
        station_name = ExAllocatePoolWithTag(NonPagedPool, ((SIZE_T)separator + 1) * 2, 'maNU');
        if (!station_name) return STATUS_NO_MEMORY;
        memcpy(station_name, path, separator * 2u); station_name[separator] = 0;
    }
    const WCHAR *desktop_name = separator == 32767 ? path : path + separator + 1;
    UINT64 flags = W32kSessionLock();
    W32K_WINDOW_STATION *station = station_name ? W32kFindStationLocked(station_name) : context->Station;
    W32K_DESKTOP *desktop = station ? W32kFindDesktopLocked(station, desktop_name) : NULL;
    if (station) ObReferenceObject(station);
    if (desktop) ObReferenceObject(desktop);
    W32kSessionUnlock(flags); if (station_name) ExFreePool(station_name);
    if (!station || !desktop) {
        if (station) ObDereferenceObject(station);
        if (desktop) ObDereferenceObject(desktop);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (station == context->Station && desktop == context->DefaultDesktop) {
        ObDereferenceObject(station); ObDereferenceObject(desktop); return STATUS_SUCCESS;
    }
    HANDLE station_handle = NULL, desktop_handle = NULL;
    NTSTATUS status = W32kSessionHandle(context, station, W32K_STATION_ALL, TRUE, FALSE, &station_handle);
    if (NT_SUCCESS(status)) status = W32kSessionHandle(context, desktop,
                                  W32K_DESKTOP_ALL, TRUE, FALSE, &desktop_handle);
    if (!NT_SUCCESS(status)) {
        if (station_handle) W32kSessionUndoHandle(context, station_handle, station);
        ObDereferenceObject(station); ObDereferenceObject(desktop); return status;
    }
    flags = W32kSessionLock();
    W32K_WINDOW_STATION *old_station = context->Station;
    W32K_DESKTOP *old_desktop = context->DefaultDesktop;
    context->Station = station; context->DefaultDesktop = desktop;
    context->StationHandle = station_handle; context->DesktopHandle = desktop_handle;
    W32kSessionUnlock(flags);
    if (old_station) ObDereferenceObject(old_station);
    if (old_desktop) ObDereferenceObject(old_desktop);
    return STATUS_SUCCESS;
}

void W32kCleanupThreadSession(PKTHREAD thread)
{
    if (!thread || !thread->Win32Desktop) return;
    UINT64 flags = W32kSessionLock();
    W32K_DESKTOP *desktop = thread->Win32Desktop;
    thread->Win32Desktop = NULL; thread->UserDesktop = 0;
    if (desktop->AttachedThreads) desktop->AttachedThreads--;
    W32kSessionUnlock(flags); ObDereferenceObject(desktop);
}

static void W32kFreeProcessSessionContext(W32K_PROCESS_SESSION *context)
{
    W32K_SESSION_HANDLE *record = context->Handles;
    while (record) {
        W32K_SESSION_HANDLE *next = record->Next;
        if (record->Borrowed && context->Process->HandleTable)
            ObCloseHandleForObject(context->Process->HandleTable, record->Handle, record->Object);
        ExFreePool(record); record = next;
    }
    if (context->DefaultDesktop) ObDereferenceObject(context->DefaultDesktop);
    if (context->Station) ObDereferenceObject(context->Station);
    ExFreePool(context);
}

void W32kCleanupProcessSession(PKPROCESS process)
{
    if (!process) return;
    W32K_PROCESS_SESSION *context = __atomic_exchange_n(&process->Win32Session, NULL, __ATOMIC_ACQ_REL);
    if (context) W32kFreeProcessSessionContext(context);
}

NTSTATUS W32kBindWindowDesktop(UINT16 slot, PKTHREAD thread, UINT16 parent)
{
    if (!slot || slot >= 128 || g_w32k_window_desktops[slot]) return STATUS_INVALID_PARAMETER;
    NTSTATUS status = W32kInitializeThreadSession(thread);
    if (!NT_SUCCESS(status)) return status;
    W32K_DESKTOP *desktop = thread->Win32Desktop;
    if (parent && (parent >= 128 || g_w32k_window_desktops[parent] != desktop)) return STATUS_ACCESS_DENIED;
    W32K_PROCESS_SESSION *context = thread->Process->Win32Session;
    POBJECT reference;
    status = ObReferenceObjectByHandleInTable(thread->Process->HandleTable,
                    (HANDLE)thread->UserDesktop, W32K_DESKTOP_CREATEWINDOW,
                    g_w32k_desktop_type, &reference, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (reference != desktop || desktop->Station != context->Station) {
        ObDereferenceObject(reference); return STATUS_ACCESS_DENIED;
    }
    UINT64 flags = W32kSessionLock(); g_w32k_window_desktops[slot] = desktop;
    desktop->WindowReferences++; W32kSessionUnlock(flags);
    /* The handle reference becomes the window's reference. */
    return STATUS_SUCCESS;
}

void W32kUnbindWindowDesktop(UINT16 slot)
{
    if (!slot || slot >= 128) return;
    UINT64 flags = W32kSessionLock(); W32K_DESKTOP *desktop = g_w32k_window_desktops[slot];
    g_w32k_window_desktops[slot] = NULL;
    if (desktop && desktop->WindowReferences) desktop->WindowReferences--;
    W32kSessionUnlock(flags); if (desktop) ObDereferenceObject(desktop);
}

PVOID W32kGetCurrentWindowStationIdentity(void)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession();
    return context ? context->Station : NULL;
}

PVOID W32kGetProcessWindowStationIdentity(PKPROCESS process)
{
    W32K_PROCESS_SESSION *context = process ? process->Win32Session : NULL;
    return context ? context->Station : NULL;
}

PVOID W32kGetCurrentDesktopIdentity(void)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (!thread || !NT_SUCCESS(W32kInitializeThreadSession(thread))) return NULL;
    return thread->Win32Desktop;
}

PVOID W32kGetInputDesktopIdentity(void) { return g_w32k_input_desktop; }

NTSTATUS W32kQueryProcessDesktopName(PKPROCESS process, UINT16 *output,
    ULONG capacity, ULONG *needed)
{
    W32K_PROCESS_SESSION *context = process ? process->Win32Session : NULL;
    if (!context || !context->DefaultDesktop || !needed) return STATUS_INVALID_PARAMETER;
    ULONG station_chars = context->Station->Common.NameChars;
    ULONG desktop_chars = context->DefaultDesktop->Common.NameChars;
    if (desktop_chars > 32765u || station_chars > 32765u - desktop_chars) return STATUS_BUFFER_TOO_SMALL;
    *needed = station_chars + desktop_chars + 2;
    if (!output || capacity < *needed) return STATUS_BUFFER_TOO_SMALL;
    memcpy(output, context->Station->Common.Name, station_chars * 2u);
    output[station_chars] = '\\';
    memcpy(output + station_chars + 1, context->DefaultDesktop->Common.Name, (desktop_chars + 1) * 2u);
    return STATUS_SUCCESS;
}
PVOID W32kGetWindowDesktopIdentity(UINT16 slot)
{ return slot < 128 ? g_w32k_window_desktops[slot] : NULL; }

BOOLEAN W32kWindowOnCurrentDesktop(UINT16 slot)
{
    if (!slot || slot >= 128) return FALSE;
    if (!g_w32k_default_station && !g_w32k_window_desktops[slot]) return TRUE;
    PVOID desktop = W32kGetCurrentDesktopIdentity();
    return desktop && g_w32k_window_desktops[slot] == desktop;
}

BOOLEAN W32kWindowOnInputDesktop(UINT16 slot)
{
    if (!slot || slot >= 128) return FALSE;
    if (!g_w32k_default_station && !g_w32k_window_desktops[slot]) return TRUE;
    return g_w32k_input_desktop && g_w32k_window_desktops[slot] == g_w32k_input_desktop;
}

BOOLEAN W32kWindowsShareDesktop(UINT16 first, UINT16 second)
{
    if (!first || !second || first >= 128 || second >= 128) return FALSE;
    if (!g_w32k_default_station && !g_w32k_window_desktops[first] &&
        !g_w32k_window_desktops[second]) return TRUE;
    return g_w32k_window_desktops[first] &&
           g_w32k_window_desktops[first] == g_w32k_window_desktops[second];
}

static NTSTATUS W32kReferenceSessionHandle(W32K_PROCESS_SESSION *context,
    HANDLE handle, ULONG kind, ACCESS_MASK access, POBJECT *out)
{
    return ObReferenceObjectByHandleInTable(context->Process->HandleTable, handle,
        W32kSessionMapAccess(kind, access), kind == W32K_STATION_KIND ?
        g_w32k_station_type : g_w32k_desktop_type, out, NULL);
}

static UINT64 NtUserGetProcessWindowStation(UINT64 *args)
{
    (void)args; W32K_PROCESS_SESSION *context = W32kCurrentProcessSession();
    return context ? (UINT64)context->StationHandle : 0;
}

static UINT64 NtUserGetThreadDesktop(UINT64 *args)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession();
    if (!context) return 0;
    PKTHREAD caller = KeGetCurrentThread();
    PKTHREAD target = args[0] ? KeLookupThreadById((ULONG)args[0]) : caller;
    if (!target || !target->Process || target->Process->SecurityUserRid != context->Process->SecurityUserRid) {
        W32kSessionError(target ? 5 : 1444); return 0;
    }
    NTSTATUS status = W32kInitializeThreadSession(target);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    W32K_DESKTOP *desktop = target->Win32Desktop;
    if (desktop->Station != context->Station) { W32kSessionError(5); return 0; }
    if (target->Process == caller->Process) return target->UserDesktop;
    for (W32K_SESSION_HANDLE *record = context->Handles; record; record = record->Next)
        if (record->Borrowed && record->Object == desktop) return (UINT64)record->Handle;
    HANDLE handle;
    status = W32kSessionHandle(context, desktop, W32K_DESKTOP_ALL, TRUE, FALSE, &handle);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    return (UINT64)handle;
}

static UINT64 NtUserSetThreadDesktop(UINT64 *args)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession();
    if (!context) return 0;
    POBJECT raw;
    NTSTATUS status = W32kReferenceSessionHandle(context, (HANDLE)args[0],
        W32K_DESKTOP_KIND, W32K_DESKTOP_READOBJECTS, &raw);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    W32K_DESKTOP *desktop = raw; PKTHREAD thread = KeGetCurrentThread();
    if (desktop->Station != context->Station) status = STATUS_ACCESS_DENIED;
    if (desktop != thread->Win32Desktop && NT_SUCCESS(status))
        for (UINT16 i = 1; i < 128; i++)
            if (g_window_owner[i] == thread->ThreadId && g_window_process[i] == thread->Process) { status = STATUS_SHARING_VIOLATION; break; }
    if (!NT_SUCCESS(status)) { ObDereferenceObject(raw); W32kSessionError(W32kSessionDosError(status)); return 0; }
    UINT64 flags = W32kSessionLock(); W32K_DESKTOP *previous = thread->Win32Desktop;
    thread->Win32Desktop = desktop; thread->UserDesktop = args[0]; desktop->AttachedThreads++;
    if (previous && previous->AttachedThreads) previous->AttachedThreads--;
    W32kSessionUnlock(flags); if (previous) ObDereferenceObject(previous);
    return 1; /* raw's reference is now held by the thread */
}

static UINT64 W32kOpenDesktopByName(UINT64 address, ULONG flags_word,
                                   BOOLEAN inherit, ACCESS_MASK access)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession();
    if (!context) return 0;
    if (flags_word & ~1u) { W32kSessionError(87); return 0; }
    WCHAR *name; char *utf8; ULONG chars;
    NTSTATUS status = W32kSessionCaptureName(address, &name, &utf8, &chars);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    UINT64 lock = W32kSessionLock(); W32K_DESKTOP *desktop = W32kFindDesktopLocked(context->Station, name);
    if (desktop) ObReferenceObject(desktop);
    W32kSessionUnlock(lock);
    ExFreePool(name); ExFreePool(utf8);
    if (!desktop) { W32kSessionError(2); return 0; }
    HANDLE handle;
    status = W32kSessionHandle(context, desktop, access, FALSE, inherit, &handle);
    ObDereferenceObject(desktop);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    return (UINT64)handle;
}

static UINT64 NtUserOpenDesktop(UINT64 *args)
{ return W32kOpenDesktopByName(args[0], (ULONG)args[1], args[2] != 0, (ULONG)args[3]); }

static UINT64 NtUserOpenInputDesktop(UINT64 *args)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession();
    if (!context || ((ULONG)args[0] & ~1u)) { W32kSessionError(87); return 0; }
    UINT64 flags = W32kSessionLock(); W32K_DESKTOP *desktop = g_w32k_input_desktop;
    if (desktop) ObReferenceObject(desktop);
    W32kSessionUnlock(flags);
    if (!desktop || desktop->Station != context->Station) {
        if (desktop) ObDereferenceObject(desktop);
        W32kSessionError(5); return 0;
    }
    HANDLE handle; NTSTATUS status = W32kSessionHandle(context, desktop, (ULONG)args[2], FALSE, args[1] != 0, &handle);
    ObDereferenceObject(desktop);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    return (UINT64)handle;
}

static UINT64 W32kCloseSessionHandle(UINT64 value, ULONG kind)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession();
    if (!context) return 0;
    POBJECT object; NTSTATUS status = W32kReferenceSessionHandle(context, (HANDLE)value, kind, 0, &object);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    UINT64 flags = W32kSessionLock();
    W32K_SESSION_HANDLE *record = W32kSessionFindHandle(context, (HANDLE)value, object);
    BOOLEAN busy = record && record->Borrowed;
    if (kind == W32K_STATION_KIND && context->Station == object) busy = TRUE;
    if (kind == W32K_DESKTOP_KIND) {
        for (PLIST_ENTRY entry = context->Process->ThreadListHead.Flink;
             entry != &context->Process->ThreadListHead; entry = entry->Flink) {
            PKTHREAD thread = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
            if (thread->Win32Desktop == object && thread->UserDesktop == value) { busy = TRUE; break; }
        }
        PKTHREAD caller = KeGetCurrentThread();
        if (caller && caller->Process == context->Process && caller->Win32Desktop == object &&
            caller->UserDesktop == value) busy = TRUE;
    }
    W32kSessionUnlock(flags);
    if (busy) { ObDereferenceObject(object); W32kSessionError(170); return 0; }
    status = ObCloseHandle((HANDLE)value);
    if (NT_SUCCESS(status) && record) {
        flags = W32kSessionLock(); W32K_SESSION_HANDLE **link = &context->Handles;
        while (*link && *link != record) link = &(*link)->Next;
        if (*link) *link = record->Next;
        W32kSessionUnlock(flags); ExFreePool(record);
    }
    ObDereferenceObject(object);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    return 1;
}

static UINT64 NtUserCloseDesktop(UINT64 *args)
{ return W32kCloseSessionHandle(args[0], W32K_DESKTOP_KIND); }
static UINT64 NtUserCloseWindowStation(UINT64 *args)
{ return W32kCloseSessionHandle(args[0], W32K_STATION_KIND); }

static UINT64 NtUserOpenWindowStation(UINT64 *args)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession(); if (!context) return 0;
    WCHAR *name; char *utf8; ULONG chars;
    NTSTATUS status = W32kSessionCaptureName(args[0], &name, &utf8, &chars);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    UINT64 flags = W32kSessionLock(); W32K_WINDOW_STATION *station = W32kFindStationLocked(name);
    if (station) ObReferenceObject(station);
    W32kSessionUnlock(flags);
    ExFreePool(name); ExFreePool(utf8);
    if (!station) { W32kSessionError(2); return 0; }
    HANDLE handle; status = W32kSessionHandle(context, station, (ULONG)args[2], FALSE, args[1] != 0, &handle);
    ObDereferenceObject(station);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    return (UINT64)handle;
}

static UINT64 NtUserSetProcessWindowStation(UINT64 *args)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession(); if (!context) return 0;
    POBJECT raw; NTSTATUS status = W32kReferenceSessionHandle(context, (HANDLE)args[0],
        W32K_STATION_KIND, W32K_WINSTA_READATTRIBUTES, &raw);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    W32K_WINDOW_STATION *station = raw;
    if (station == context->Station) { ObDereferenceObject(raw); context->StationHandle = (HANDLE)args[0]; return 1; }
    for (UINT16 i = 1; i < 128; i++) if (g_window_process[i] == context->Process && g_window_owner[i]) {
        ObDereferenceObject(raw); W32kSessionError(170); return 0;
    }
    UINT64 flags = W32kSessionLock();
    W32K_WINDOW_STATION *old_station = context->Station;
    W32K_DESKTOP *old_default = context->DefaultDesktop;
    context->Station = station; context->StationHandle = (HANDLE)args[0];
    context->DefaultDesktop = NULL; context->DesktopHandle = NULL;
    W32kSessionUnlock(flags);
    for (PLIST_ENTRY entry = context->Process->ThreadListHead.Flink;
         entry != &context->Process->ThreadListHead; entry = entry->Flink)
        W32kCleanupThreadSession(CONTAINING_RECORD(entry, KTHREAD, ProcessEntry));
    PKTHREAD caller = KeGetCurrentThread();
    if (caller && caller->Process == context->Process) W32kCleanupThreadSession(caller);
    if (old_default) ObDereferenceObject(old_default);
    ObDereferenceObject(old_station); return 1;
}

static UINT64 SvNtUserCreateWindowStation(UINT64 *args)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession(); if (!context) return 0;
    if ((ULONG)args[1]) { W32kSessionError(87); return 0; }
    WCHAR *name; char *utf8; ULONG chars;
    NTSTATUS status = W32kSessionCaptureName(args[0], &name, &utf8, &chars);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    UINT64 lock = W32kSessionLock(); W32K_WINDOW_STATION *station = W32kFindStationLocked(name);
    if (station) ObReferenceObject(station);
    W32kSessionUnlock(lock);
    BOOLEAN inherit = FALSE;
    if (station) { ExFreePool(name); ExFreePool(utf8); }
    else status = W32kCreateStationObject(name, utf8, chars, context->Process->SecurityUserRid,
                                            0, args[3], &station, &inherit);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    HANDLE handle; status = W32kSessionHandle(context, station, (ULONG)args[2], FALSE, inherit, &handle);
    if (!NT_SUCCESS(status) && !ObHeaderFromObject(station)->HandleCount)
        ObRemoveObjectName(station);
    ObDereferenceObject(station);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    return (UINT64)handle;
}

static UINT64 SvNtUserCreateDesktop(UINT64 *args)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession(); if (!context) return 0;
    /* Device and DEVMODE are reserved by CreateDesktopW. */
    if (args[1] || args[2] || ((ULONG)args[3] & ~1u)) { W32kSessionError(87); return 0; }
    POBJECT station_ref;
    NTSTATUS status = W32kReferenceSessionHandle(context, context->StationHandle,
        W32K_STATION_KIND, W32K_WINSTA_CREATEDESKTOP, &station_ref);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    WCHAR *name; char *utf8; ULONG chars;
    status = W32kSessionCaptureName(args[0], &name, &utf8, &chars);
    if (!NT_SUCCESS(status)) { ObDereferenceObject(station_ref); W32kSessionError(W32kSessionDosError(status)); return 0; }
    W32K_WINDOW_STATION *station = station_ref;
    UINT64 lock = W32kSessionLock(); W32K_DESKTOP *desktop = W32kFindDesktopLocked(station, name);
    if (desktop) ObReferenceObject(desktop);
    W32kSessionUnlock(lock);
    BOOLEAN inherit = FALSE;
    if (desktop) { ExFreePool(name); ExFreePool(utf8); }
    else status = W32kCreateDesktopObject(station, name, utf8, chars,
        context->Process->SecurityUserRid, (ULONG)args[3], args[5], &desktop, &inherit);
    ObDereferenceObject(station_ref);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    HANDLE handle; status = W32kSessionHandle(context, desktop, (ULONG)args[4], FALSE, inherit, &handle);
    if (NT_SUCCESS(status) && !context->DefaultDesktop) {
        HANDLE initial;
        status = W32kSessionHandle(context, desktop, (ULONG)args[4], TRUE, FALSE, &initial);
        if (NT_SUCCESS(status)) {
            UINT64 flags = W32kSessionLock(); context->DefaultDesktop = desktop;
            ObReferenceObject(desktop); context->DesktopHandle = initial; W32kSessionUnlock(flags);
        } else {
            W32kCloseSessionHandle((UINT64)handle, W32K_DESKTOP_KIND);
        }
    }
    ObDereferenceObject(desktop);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    return (UINT64)handle;
}

static UINT64 SvNtUserSwitchDesktop(UINT64 *args)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession(); if (!context) return 0;
    POBJECT raw; NTSTATUS status = W32kReferenceSessionHandle(context, (HANDLE)args[0],
        W32K_DESKTOP_KIND, W32K_DESKTOP_SWITCH, &raw);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    W32K_DESKTOP *desktop = raw;
    if (!(desktop->Station->Common.Flags & 1)) {
        ObDereferenceObject(raw); W32kSessionError(5); return 0;
    }
    UINT64 flags = W32kSessionLock(); W32K_DESKTOP *previous = g_w32k_input_desktop;
    if (previous) previous->SavedForeground = KiUserWindowHandle(W32kActiveSlot());
    g_w32k_input_desktop = desktop; W32kSessionUnlock(flags);
    W32kInputDesktopChanged(previous, desktop);
    if (previous) ObDereferenceObject(previous);
    return 1; /* raw's reference becomes the input-desktop reference */
}

static UINT64 SvNtUserEnumDesktopWindows(UINT64 *args)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession(); if (!context) return (UINT64)-1;
    POBJECT reference = NULL; W32K_DESKTOP *desktop;
    if (args[0]) {
        NTSTATUS status = W32kReferenceSessionHandle(context, (HANDLE)args[0],
            W32K_DESKTOP_KIND, W32K_DESKTOP_ENUMERATE, &reference);
        if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return (UINT64)-1; }
        desktop = reference;
    } else desktop = W32kGetCurrentDesktopIdentity();
    UINT64 snapshot[128]; ULONG count = 0;
    UINT64 flags = W32kSessionLock();
    for (UINT16 i = 1; i < 128; i++)
        if (g_window_owner[i] && !W32kWindowParent(i) && g_w32k_window_desktops[i] == desktop)
            snapshot[count++] = KiUserWindowHandle(i);
    W32kSessionUnlock(flags);
    if (reference) ObDereferenceObject(reference);
    if (args[2] < count) { W32kSessionError(122); return (UINT64)-1; }
    if (count && !MmProbeForWrite(args[1], (UINT64)count * 8)) { W32kSessionError(998); return (UINT64)-1; }
    if (count) memcpy((void *)args[1], snapshot, count * 8u);
    return count;
}

static UINT64 W32kEnumerateSessionNames(UINT64 *args, BOOLEAN stations)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession(); if (!context) return 0;
    POBJECT station_ref = NULL; LIST_ENTRY *list;
    NTSTATUS status = STATUS_SUCCESS;
    if (!stations) status = W32kReferenceSessionHandle(context, (HANDLE)args[0],
        W32K_STATION_KIND, W32K_WINSTA_ENUMDESKTOPS, &station_ref);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    list = stations ? &g_w32k_stations : &((W32K_WINDOW_STATION *)station_ref)->Desktops;
    UINT64 flags = W32kSessionLock(); ULONG required = 1, count = 0;
    for (PLIST_ENTRY entry = list->Flink; entry != list; entry = entry->Flink) {
        W32K_SESSION_OBJECT *object = CONTAINING_RECORD(entry, W32K_SESSION_OBJECT, Entry);
        if (stations && !W32kSessionAccessAllowed(object, context->Process, 0x0100u)) continue;
        if (required > 0x7FFFFFFFu - object->NameChars - 1) { status = STATUS_NO_MEMORY; break; }
        required += object->NameChars + 1; count++;
    }
    if (!count) required = 2;
    ULONG *needed = (ULONG *)args[3];
    if (!needed || !MmProbeForWrite((UINT64)needed, 4)) status = STATUS_ACCESS_VIOLATION;
    else *needed = required;
    if (NT_SUCCESS(status) && ((ULONG)args[2] < required || !args[1])) status = STATUS_BUFFER_TOO_SMALL;
    if (NT_SUCCESS(status) && !MmProbeForWrite(args[1], (UINT64)required * 2)) status = STATUS_ACCESS_VIOLATION;
    if (NT_SUCCESS(status)) {
        WCHAR *out = (WCHAR *)args[1]; ULONG written = 0;
        for (PLIST_ENTRY entry = list->Flink; entry != list; entry = entry->Flink) {
            W32K_SESSION_OBJECT *object = CONTAINING_RECORD(entry, W32K_SESSION_OBJECT, Entry);
            if (stations && !W32kSessionAccessAllowed(object, context->Process, 0x0100u)) continue;
            memcpy(out + written, object->Name, (object->NameChars + 1) * 2u);
            written += object->NameChars + 1;
        }
        out[written++] = 0; if (!count) out[written] = 0;
    }
    W32kSessionUnlock(flags); if (station_ref) ObDereferenceObject(station_ref);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    return 1;
}

static UINT64 SvNtUserEnumDesktops(UINT64 *args)
{ return W32kEnumerateSessionNames(args, FALSE); }
static UINT64 SvNtUserEnumWindowStations(UINT64 *args)
{ return W32kEnumerateSessionNames(args, TRUE); }

static UINT64 NtUserGetObjectInformation(UINT64 *args)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession(); if (!context) return 0;
    POBJECT raw; ACCESS_MASK granted;
    NTSTATUS status = ObReferenceObjectByHandleInTable(context->Process->HandleTable,
        (HANDLE)args[0], 0, NULL, &raw, &granted);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    POBJECT_TYPE type = ObHeaderFromObject(raw)->Type;
    if (type != g_w32k_station_type && type != g_w32k_desktop_type) {
        ObDereferenceObject(raw); W32kSessionError(6); return 0;
    }
    W32K_SESSION_OBJECT *object = raw;
    static const WCHAR station_type[] = {'W','i','n','d','o','w','S','t','a','t','i','o','n',0};
    static const WCHAR desktop_type[] = {'D','e','s','k','t','o','p',0};
    UINT8 sid[28] = {1,5,0,0,0,0,0,5}; ULONG parts[5] = {21,1,2,3,object->OwnerRid};
    memcpy(sid + 8, parts, 20);
    ULONG scalar[3] = {0}; const void *source = NULL; ULONG bytes = 0;
    ULONG index = (ULONG)args[1];
    if (index == 1) {
        ULONG attributes = 0;
        ObQueryHandleAttributesInTable(context->Process->HandleTable, (HANDLE)args[0], &attributes);
        scalar[0] = (attributes & OB_HANDLE_INHERIT) != 0; scalar[2] = object->Flags;
        source = scalar; bytes = 12;
    } else if (index == 2) { source = object->Name; bytes = (object->NameChars + 1) * 2u; }
    else if (index == 3) {
        source = type == g_w32k_station_type ? station_type : desktop_type;
        bytes = type == g_w32k_station_type ? sizeof(station_type) : sizeof(desktop_type);
    } else if (index == 4) { source = sid; bytes = sizeof(sid); }
    else if (index == 6 && type == g_w32k_desktop_type) {
        scalar[0] = raw == g_w32k_input_desktop; source = scalar; bytes = 4;
    } else status = STATUS_INVALID_PARAMETER;
    ULONG *needed = (ULONG *)args[4];
    if (needed) {
        if (!MmProbeForWrite((UINT64)needed, 4)) status = STATUS_ACCESS_VIOLATION;
        else *needed = bytes;
    }
    if (NT_SUCCESS(status) && ((ULONG)args[3] < bytes || !args[2])) status = STATUS_BUFFER_TOO_SMALL;
    if (NT_SUCCESS(status) && !MmProbeForWrite(args[2], bytes)) status = STATUS_ACCESS_VIOLATION;
    if (NT_SUCCESS(status)) memcpy((void *)args[2], source, bytes);
    ObDereferenceObject(raw);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    return 1;
}

static UINT64 SvNtUserSetObjectInformation(UINT64 *args)
{
    W32K_PROCESS_SESSION *context = W32kCurrentProcessSession(); if (!context) return 0;
    if (args[1] != 1 || args[3] != 12 || !MmProbeForRead(args[2], 12)) { W32kSessionError(87); return 0; }
    POBJECT raw; NTSTATUS status = ObReferenceObjectByHandleInTable(context->Process->HandleTable,
        (HANDLE)args[0], 0, NULL, &raw, NULL);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    POBJECT_TYPE type = ObHeaderFromObject(raw)->Type;
    if (type != g_w32k_station_type && type != g_w32k_desktop_type) { ObDereferenceObject(raw); W32kSessionError(6); return 0; }
    ULONG words[3]; memcpy(words, (void *)args[2], 12);
    W32K_SESSION_OBJECT *object = raw;
    if (words[1] || words[2] != object->Flags) { ObDereferenceObject(raw); W32kSessionError(87); return 0; }
    UINT64 lock = W32kSessionLock(); W32K_SESSION_HANDLE *record = W32kSessionFindHandle(context, (HANDLE)args[0], raw);
    if (record) record->Inherit = words[0] != 0;
    W32kSessionUnlock(lock); ObDereferenceObject(raw);
    status = ObSetHandleAttributesInTable(context->Process->HandleTable,
        (HANDLE)args[0], OB_HANDLE_INHERIT, words[0] ? OB_HANDLE_INHERIT : 0);
    if (!NT_SUCCESS(status)) { W32kSessionError(W32kSessionDosError(status)); return 0; }
    return 1;
}
