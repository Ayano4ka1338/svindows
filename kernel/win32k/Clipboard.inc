/* Window-station clipboard broker. Data outlives the producer, opening is
 * thread-exclusive, delayed formats are rendered by their actual owner, and
 * every stored byte is owned by the executive pool. */
#include <abi/Clipboard.h>

typedef struct _W32K_CLIPBOARD_FORMAT {
    struct _W32K_CLIPBOARD_FORMAT *Next;
    UINT32 Format;
    BOOLEAN Rendering;
    SIZE_T Bytes;
    UINT64 Version;
    void *Data;
} W32K_CLIPBOARD_FORMAT;
typedef struct _W32K_CLIPBOARD_LISTENER {
    struct _W32K_CLIPBOARD_LISTENER *Next;
    UINT64 Window;
} W32K_CLIPBOARD_LISTENER;
typedef struct _W32K_CLIPBOARD {
    struct _W32K_CLIPBOARD *Next;
    PVOID Station;
    PKTHREAD OpenThread;
    PKPROCESS OwnerProcess;
    UINT64 OpenWindow, OwnerWindow, Revision;
    UINT32 Sequence;
    BOOLEAN Changed;
    PKEVENT ChangeEvent;
    W32K_CLIPBOARD_FORMAT *Formats;
    W32K_CLIPBOARD_LISTENER *Listeners;
} W32K_CLIPBOARD;

static W32K_CLIPBOARD *g_clipboards;

static W32K_CLIPBOARD_FORMAT *W32kClipboardFormat(W32K_CLIPBOARD *clipboard, UINT32 format)
{
    for (W32K_CLIPBOARD_FORMAT *entry = clipboard ? clipboard->Formats : NULL; entry; entry = entry->Next)
        if (entry->Format == format) return entry;
    return NULL;
}

static BOOLEAN W32kClipboardAvailable(W32K_CLIPBOARD *clipboard, UINT32 format)
{
    if (W32kClipboardFormat(clipboard, format)) return TRUE;
    if (format == 1 || format == 7 || format == 13)
        return W32kClipboardFormat(clipboard, 1) || W32kClipboardFormat(clipboard, 7) ||
               W32kClipboardFormat(clipboard, 13);
    return FALSE;
}

static void W32kClipboardClear(W32K_CLIPBOARD *clipboard)
{
    while (clipboard->Formats) {
        W32K_CLIPBOARD_FORMAT *entry = clipboard->Formats;
        clipboard->Formats = entry->Next;
        if (entry->Data) ExFreePool(entry->Data);
        ExFreePool(entry);
    }
}

static void W32kClipboardNotify(W32K_CLIPBOARD *clipboard)
{
    W32K_CLIPBOARD_LISTENER **link = &clipboard->Listeners;
    while (*link) {
        W32K_CLIPBOARD_LISTENER *listener = *link;
        if (!KiUserWindowValid(listener->Window, FALSE)) {
            *link = listener->Next; ExFreePool(listener); continue;
        }
        KiUserEnqueueMessage(listener->Window, 0x031d, 0, 0, 0, 0); /* WM_CLIPBOARDUPDATE */
        link = &listener->Next;
    }
    clipboard->Changed = FALSE;
}

static W32K_CLIPBOARD *W32kCurrentClipboard(BOOLEAN create)
{
    PVOID station = W32kGetCurrentWindowStationIdentity();
    for (W32K_CLIPBOARD *clipboard = g_clipboards; clipboard; clipboard = clipboard->Next)
        if (clipboard->Station == station) return clipboard;
    if (!create) return NULL;
    W32K_CLIPBOARD *clipboard = ExAllocatePoolWithTag(NonPagedPool, sizeof(*clipboard), 'pilC');
    if (!clipboard) return NULL;
    memset(clipboard, 0, sizeof(*clipboard));
    HANDLE event_handle;
    NTSTATUS status = PsCreateNotificationEvent(&clipboard->ChangeEvent, &event_handle);
    if (!NT_SUCCESS(status)) { ExFreePool(clipboard); return NULL; }
    ObCloseHandle(event_handle); /* the station retains the object reference */
    clipboard->Station = station;
    clipboard->Revision = 1;
    clipboard->Next = g_clipboards;
    g_clipboards = clipboard;
    return clipboard;
}

static void W32kClipboardDeleteSession(PVOID identity, BOOLEAN station)
{
    if (!station) return;
    W32K_CLIPBOARD **link = &g_clipboards;
    while (*link && (*link)->Station != identity) link = &(*link)->Next;
    if (!*link) return;
    W32K_CLIPBOARD *clipboard = *link;
    *link = clipboard->Next;
    KeSetEvent(clipboard->ChangeEvent);
    W32kClipboardClear(clipboard);
    while (clipboard->Listeners) {
        W32K_CLIPBOARD_LISTENER *listener = clipboard->Listeners;
        clipboard->Listeners = listener->Next; ExFreePool(listener);
    }
    ObDereferenceObject(clipboard->ChangeEvent);
    ExFreePool(clipboard);
}

static void W32kClipboardCleanupThread(PKTHREAD thread)
{
    for (W32K_CLIPBOARD *clipboard = g_clipboards; clipboard; clipboard = clipboard->Next)
        if (clipboard->OpenThread == thread) {
            clipboard->OpenThread = NULL;
            clipboard->OpenWindow = 0;
            if (clipboard->Changed) W32kClipboardNotify(clipboard);
            KeSetEvent(clipboard->ChangeEvent);
        }
}

static void W32kClipboardWindowDestroyed(UINT64 window)
{
    for (W32K_CLIPBOARD *clipboard = g_clipboards; clipboard; clipboard = clipboard->Next) {
        if (clipboard->OwnerWindow == window) {
            clipboard->OwnerWindow = 0;
            clipboard->OwnerProcess = NULL;
            W32K_CLIPBOARD_FORMAT **link = &clipboard->Formats;
            while (*link) {
                W32K_CLIPBOARD_FORMAT *entry = *link;
                if (!entry->Data) { *link = entry->Next; ExFreePool(entry); clipboard->Sequence++; }
                else link = &entry->Next;
            }
            KeSetEvent(clipboard->ChangeEvent);
        }
        W32K_CLIPBOARD_LISTENER **link = &clipboard->Listeners;
        while (*link) {
            W32K_CLIPBOARD_LISTENER *listener = *link;
            if (listener->Window == window) { *link = listener->Next; ExFreePool(listener); }
            else link = &listener->Next;
        }
    }
}

void W32kInitializeWindowEnvironment(void)
{
    W32kRegisterSessionDeleteNotify(W32kShellDeleteSession);
    W32kRegisterSessionDeleteNotify(W32kClipboardDeleteSession);
}

static UINT64 SvNtUserClipboardControl(UINT64 *a)
{
    UINT32 operation = (UINT32)a[0];
    UINT64 flags = KiIrqSave();
    W32K_CLIPBOARD *clipboard = W32kCurrentClipboard(TRUE);
    PKTHREAD caller = KeGetCurrentThread();
    if (!clipboard || !caller || !caller->Process) {
        KiIrqRestore(flags); W32kEnvironmentError(8); return 0;
    }
    if (operation == SV_CLIP_OPEN) {
        if (a[1] && (!KiUserWindowValid(a[1], FALSE) ||
            !W32kWindowOnCurrentDesktop((UINT16)a[1]))) {
            KiIrqRestore(flags); W32kEnvironmentError(1400); return 0;
        }
        if (clipboard->OpenThread &&
            (clipboard->OpenThread != caller || clipboard->OpenWindow != a[1])) {
            KiIrqRestore(flags); W32kEnvironmentError(5); return 0;
        }
        clipboard->OpenThread = caller; clipboard->OpenWindow = a[1];
        KiIrqRestore(flags); return 1;
    }
    if (operation == SV_CLIP_CLOSE || operation == SV_CLIP_EMPTY) {
        if (clipboard->OpenThread != caller) {
            KiIrqRestore(flags); W32kEnvironmentError(1418); return 0;
        }
        if (operation == SV_CLIP_CLOSE) {
            clipboard->OpenThread = NULL; clipboard->OpenWindow = 0;
            if (clipboard->Changed) W32kClipboardNotify(clipboard);
        } else {
            if (clipboard->OwnerWindow && KiUserWindowValid(clipboard->OwnerWindow, FALSE))
                KiUserEnqueueMessagePrivate(clipboard->OwnerWindow, 0x0307, 0, 0, 0, 0, 0, TRUE);
            W32kClipboardClear(clipboard);
            clipboard->OwnerWindow = clipboard->OpenWindow;
            clipboard->OwnerProcess = caller->Process;
            clipboard->Revision++;
            clipboard->Sequence++;
            clipboard->Changed = TRUE;
            KeSetEvent(clipboard->ChangeEvent);
        }
        KiIrqRestore(flags); return 1;
    }
    if (operation == SV_CLIP_OWNER) { UINT64 result = clipboard->OwnerWindow; KiIrqRestore(flags); return result; }
    if (operation == SV_CLIP_OPEN_WINDOW) { UINT64 result = clipboard->OpenWindow; KiIrqRestore(flags); return result; }
    if (operation == SV_CLIP_SEQUENCE) { UINT64 result = clipboard->Sequence; KiIrqRestore(flags); return result; }
    if (operation == SV_CLIP_CONTENT_REVISION) { UINT64 result = clipboard->Revision; KiIrqRestore(flags); return result; }
    if (operation == SV_CLIP_AVAILABLE || operation == SV_CLIP_EXACT_FORMAT) {
        BOOLEAN result = operation == SV_CLIP_AVAILABLE ? W32kClipboardAvailable(clipboard, (UINT32)a[1]) :
                         W32kClipboardFormat(clipboard, (UINT32)a[1]) != NULL;
        KiIrqRestore(flags); return result;
    }
    if (operation == SV_CLIP_COUNT || operation == SV_CLIP_ENUMERATE) {
        if (operation == SV_CLIP_ENUMERATE && clipboard->OpenThread != caller) {
            KiIrqRestore(flags); W32kEnvironmentError(1418); return 0;
        }
        UINT32 count = 0, next = 0, previous = (UINT32)a[1];
        BOOLEAN found = !previous;
        for (W32K_CLIPBOARD_FORMAT *entry = clipboard->Formats; entry; entry = entry->Next) {
            count++;
            if (found && !next) next = entry->Format;
            if (entry->Format == previous) found = TRUE;
        }
        const UINT32 text_formats[] = {1, 7, 13};
        for (UINT32 i = 0; i < 3; ++i) if (!W32kClipboardFormat(clipboard, text_formats[i]) &&
                                                    W32kClipboardAvailable(clipboard, text_formats[i])) {
            count++;
            if (found && !next) next = text_formats[i];
            if (text_formats[i] == previous) found = TRUE;
        }
        KiIrqRestore(flags);
        W32kEnvironmentError(found ? 0 : 87);
        return operation == SV_CLIP_COUNT ? count : next;
    }
    if (operation == SV_CLIP_REGISTER_FORMAT) {
        WCHAR name[USER_ATOM_CHARS];
        if (!a[1] || !W32kCaptureEnvironmentString(a[1], name, USER_ATOM_CHARS) || !name[0]) {
            KiIrqRestore(flags); W32kEnvironmentError(87); return 0;
        }
        UINT64 atom = KiUserAddAtom(name);
        KiIrqRestore(flags); if (!atom) W32kEnvironmentError(8); return atom;
    }
    if (operation == SV_CLIP_FORMAT_NAME) {
        const WCHAR *name = KiUserAtomName((UINT16)a[1]);
        if (!name || !a[2] || !a[3] || a[3] > 0x7fffffff ||
            !MmProbeForWrite(a[2], a[3] * sizeof(WCHAR))) {
            KiIrqRestore(flags); W32kEnvironmentError(87); return 0;
        }
        WCHAR *out = (WCHAR *)a[2]; UINT32 copied = 0;
        while (copied + 1 < a[3] && name[copied]) { out[copied] = name[copied]; copied++; }
        out[copied] = 0; KiIrqRestore(flags); return copied;
    }
    if (operation == SV_CLIP_CHANGE_EVENT) {
        HANDLE handle;
        NTSTATUS status = ObCreateHandle(clipboard->ChangeEvent, 0x00100000u, &handle);
        KiIrqRestore(flags);
        if (!NT_SUCCESS(status)) { W32kEnvironmentError(8); return 0; }
        return (UINT64)handle;
    }
    if (operation == SV_CLIP_ADD_LISTENER || operation == SV_CLIP_REMOVE_LISTENER) {
        if (!KiUserWindowValid(a[1], TRUE) || !W32kWindowOnCurrentDesktop((UINT16)a[1])) {
            KiIrqRestore(flags); W32kEnvironmentError(1400); return 0;
        }
        W32K_CLIPBOARD_LISTENER **link = &clipboard->Listeners;
        while (*link && (*link)->Window != a[1]) link = &(*link)->Next;
        if (operation == SV_CLIP_REMOVE_LISTENER) {
            if (!*link) { KiIrqRestore(flags); W32kEnvironmentError(87); return 0; }
            W32K_CLIPBOARD_LISTENER *listener = *link; *link = listener->Next; ExFreePool(listener);
        } else if (!*link) {
            W32K_CLIPBOARD_LISTENER *listener = ExAllocatePoolWithTag(NonPagedPool, sizeof(*listener), 'tslC');
            if (!listener) { KiIrqRestore(flags); W32kEnvironmentError(8); return 0; }
            *listener = (W32K_CLIPBOARD_LISTENER){.Window = a[1]}; *link = listener;
        }
        KiIrqRestore(flags); return 1;
    }
    KiIrqRestore(flags); W32kEnvironmentError(87); return 0;
}

static UINT64 SvNtUserWriteClipboard(UINT64 *a)
{
    UINT32 format = (UINT32)a[0];
    UINT64 flags = KiIrqSave();
    W32K_CLIPBOARD *clipboard = W32kCurrentClipboard(FALSE);
    PKTHREAD caller = KeGetCurrentThread();
    W32K_CLIPBOARD_FORMAT *entry = W32kClipboardFormat(clipboard, format);
    BOOLEAN rendering = clipboard && caller && entry && entry->Rendering &&
                        clipboard->OwnerProcess == caller->Process;
    if (!clipboard || !caller || (clipboard->OpenThread != caller && !rendering)) {
        KiIrqRestore(flags); return STATUS_ACCESS_DENIED;
    }
    if (!format || format > 0xffff || (!clipboard->OwnerWindow && !rendering) ||
        (a[1] == 0 && a[2] != 0) || a[2] > (UINT64)(SIZE_T)-1) {
        KiIrqRestore(flags); return STATUS_INVALID_PARAMETER;
    }
    if (a[2] && !MmProbeForRead(a[1], a[2])) { KiIrqRestore(flags); return STATUS_ACCESS_VIOLATION; }
    void *data = NULL;
    if (a[1]) {
        data = ExAllocatePoolWithTag(NonPagedPool, a[2] ? (SIZE_T)a[2] : 1, 'atdC');
        if (!data) { KiIrqRestore(flags); return STATUS_NO_MEMORY; }
        if (a[2]) memcpy(data, (void *)a[1], (SIZE_T)a[2]);
    }
    if (!entry) {
        entry = ExAllocatePoolWithTag(NonPagedPool, sizeof(*entry), 'mrfC');
        if (!entry) { if (data) ExFreePool(data); KiIrqRestore(flags); return STATUS_NO_MEMORY; }
        memset(entry, 0, sizeof(*entry)); entry->Format = format;
        W32K_CLIPBOARD_FORMAT **link = &clipboard->Formats;
        while (*link) link = &(*link)->Next;
        *link = entry;
    }
    if (entry->Data) ExFreePool(entry->Data);
    entry->Data = data; entry->Bytes = (SIZE_T)a[2]; entry->Rendering = FALSE;
    clipboard->Sequence++; entry->Version = clipboard->Sequence; clipboard->Changed = TRUE;
    KeSetEvent(clipboard->ChangeEvent);
    if (!clipboard->OpenThread) W32kClipboardNotify(clipboard);
    KiIrqRestore(flags); return STATUS_SUCCESS;
}

static UINT64 SvNtUserReadClipboard(UINT64 *a)
{
    UINT64 flags = KiIrqSave();
    W32K_CLIPBOARD *clipboard = W32kCurrentClipboard(FALSE);
    PKTHREAD caller = KeGetCurrentThread();
    if (!clipboard || !caller || clipboard->OpenThread != caller) {
        KiIrqRestore(flags); return STATUS_ACCESS_DENIED;
    }
    W32K_CLIPBOARD_FORMAT *entry = W32kClipboardFormat(clipboard, (UINT32)a[0]);
    if (!entry) { KiIrqRestore(flags); return STATUS_OBJECT_NAME_NOT_FOUND; }
    if (!MmProbeForWrite(a[3], sizeof(SV_CLIPBOARD_DATA))) {
        KiIrqRestore(flags); return STATUS_ACCESS_VIOLATION;
    }
    if (!entry->Data) {
        if (!clipboard->OwnerWindow || !KiUserWindowValid(clipboard->OwnerWindow, FALSE) ||
            !clipboard->OwnerProcess || clipboard->OwnerProcess->Exiting) {
            KiIrqRestore(flags); return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        KeResetEvent(clipboard->ChangeEvent);
        if (!entry->Rendering) {
            if (!KiUserEnqueueMessagePrivate(clipboard->OwnerWindow, 0x0305, entry->Format, 0, 0, 0, 0, TRUE)) {
                KiIrqRestore(flags); return STATUS_NO_MEMORY;
            }
            entry->Rendering = TRUE;
        }
        KiIrqRestore(flags); return STATUS_PENDING;
    }
    SV_CLIPBOARD_DATA metadata = {.Bytes = entry->Bytes, .Revision = clipboard->Revision,
        .Version = entry->Version, .Format = entry->Format};
    memcpy((void *)a[3], &metadata, sizeof(metadata));
    if (!a[1]) { KiIrqRestore(flags); return STATUS_SUCCESS; }
    if (a[2] < entry->Bytes) { KiIrqRestore(flags); return STATUS_BUFFER_TOO_SMALL; }
    if (!MmProbeForWrite(a[1], entry->Bytes)) { KiIrqRestore(flags); return STATUS_ACCESS_VIOLATION; }
    memcpy((void *)a[1], entry->Data, entry->Bytes);
    KiIrqRestore(flags); return STATUS_SUCCESS;
}
