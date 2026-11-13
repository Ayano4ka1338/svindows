/* Executive console objects and native conhost protocol. Console File handles
 * are real device endpoints; applications and the renderer share one console
 * state rather than redirecting the application's stream through pipes. */
#include <nt/Ntstatus.h>
#include <svindows/AddressSpace.h>
#include <svindows/Console.h>
#include <svindows/Ex.h>
#include <svindows/Gfx.h>
#include <svindows/Io.h>
#include <svindows/Ke.h>
#include <svindows/Mm.h>
#include <svindows/Ob.h>
#include <svindows/Ps.h>
#include <svindows/Rtl.h>
#include <svindows/Se.h>
#include <svindows/Session.h>
#include <svindows/Win32k.h>
typedef UINT16 WCHAR;
extern PVOID W32kGetProcessWindowStationIdentity(PKPROCESS process);
extern NTSTATUS PsCreateConsoleHost(PKPROCESS owner, const char *command, HANDLE *host);
extern NTSTATUS PsCreateUserThreadForProcess(PKPROCESS process, UINT64 entry, UINT64 argument,
                                             HANDLE *thread);
extern BOOLEAN W32kWindowBelongsToProcess(UINT64 window, PKPROCESS process);

#include "ConsoleCodepage.inc"

#define CONSOLE_INPUT_FLAGS 0x03ffu
#define CONSOLE_OUTPUT_FLAGS 0x001fu
#define CONSOLE_LINE_DEFAULT (CON_INPUT_PROCESSED | CON_INPUT_LINE | CON_INPUT_ECHO | 0xe0u)
#define CREATE_NEW_CONSOLE_ 0x10u
#define CREATE_NEW_PROCESS_GROUP_ 0x200u
#define DETACHED_PROCESS_ 8u
#define CREATE_NO_WINDOW_ 0x08000000u
#define STARTF_USESTDHANDLES_ 0x100u
#define CTRL_C_EXIT_ ((NTSTATUS)0xc000013au)
#ifndef STATUS_CANCELLED
#define STATUS_CANCELLED ((NTSTATUS)0xc0000120u)
#endif

typedef struct _CON_SCREEN CON_SCREEN;
typedef struct _CON_OBJECT CON_OBJECT;
typedef struct _CON_ATTACHMENT CON_ATTACHMENT;
struct _CON_SCREEN {
    CON_OBJECT *Console;
    SV_CONSOLE_CELL *Cells;
    UINT32 Width, Height, CursorX, CursorY, CursorSize;
    UINT16 Attribute, PopupAttribute;
    UINT32 Mode, Colors[16];
    SV_CONSOLE_RECT Window;
    BOOLEAN CursorVisible, WrapPending;
    UINT8 UtfRemaining, UtfTotal;
    UINT32 UtfValue;
    UINT8 AnsiState, AnsiCount;
    UINT32 AnsiArguments[16], SavedX, SavedY;
};
struct _CON_ATTACHMENT {
    CON_ATTACHMENT *Next;
    CON_OBJECT *Console;
    PKPROCESS Process;
    UINT32 Group;
    UINT64 ControlRoutine;
    BOOLEAN IgnoreCtrlC;
};
struct _CON_OBJECT {
    CON_ATTACHMENT *Attachments;
    CON_SCREEN *Active;
    SV_CONSOLE_INPUT_RECORD *Input;
    UINT32 InputCapacity, InputHead, InputCount, InputMode, InputCP, OutputCP;
    KEVENT InputReady;
    KEVENT HostReady;
    NTSTATUS HostStatus;
    KMUTANT Reader;
    WCHAR *Cooked, *Title;
    SIZE_T CookedCapacity, CookedCount, CookedRead, TitleLength;
    BOOLEAN CookedReady, LegacyInput, Closing, HostRequested, DisplayAllowed;
    UINT8 PendingBytes[16], PendingCount, PendingIndex;
    UINT32 HostProcessId, ForegroundProcessId, FontWidth, FontHeight;
    UINT32 ReadWakeupMask, LastReadControlState;
    UINT64 HostWindow, Revision, Identity;
    PVOID Station;
};
typedef struct {
    CON_OBJECT *Console;
    CON_SCREEN *Screen;
    BOOLEAN Input;
} CON_ENDPOINT;
typedef struct _CON_HOST_START {
    struct _CON_HOST_START *Next;
    POBJECT Process;
} CON_HOST_START;

static POBJECT_TYPE g_console_type, g_screen_type;
static UINT64 g_console_identity;
static CON_HOST_START *g_host_start_head, **g_host_start_tail = &g_host_start_head;
static KEVENT g_host_start_event;
static PKTHREAD g_console_broker;
static CON_OBJECT *g_broker_console;
static const IO_CHAR_DEVICE_OPS con_endpoint_ops;
static void con_close_handle(PKPROCESS process, HANDLE handle) {
    if (!handle)
        return;
    POBJECT object;
    if (NT_SUCCESS(ObReferenceObjectByHandleInTable(process->HandleTable, handle, 0, NULL, &object,
                                                    NULL))) {
        ObCloseHandleForObject(process->HandleTable, handle, object);
        ObDereferenceObject(object);
    }
}

static CON_ATTACHMENT *con_attachment(PKPROCESS process) {
    return process ? (CON_ATTACHMENT *)process->NativeConsole : NULL;
}
static CON_ATTACHMENT *con_current_attachment(void) {
    PKTHREAD thread = KeGetCurrentThread();
    return con_attachment(thread ? thread->Process : NULL);
}
static const UINT16 *con_codepage(UINT32 codepage) {
    return codepage == 866 ? con_cp866 : codepage == 1252 ? con_cp1252 : con_cp437;
}
static BOOLEAN con_valid_codepage(UINT32 codepage) {
    return codepage == 437 || codepage == 866 || codepage == 1252 || codepage == 65001;
}
static UINT16 con_decode_byte(UINT32 codepage, UINT8 byte) { return con_codepage(codepage)[byte]; }
static UINT8 con_encode_byte(UINT32 codepage, WCHAR character) {
    const UINT16 *map = con_codepage(codepage);
    for (UINT32 i = 0; i < 256; i++)
        if (map[i] == character)
            return (UINT8)i;
    return '?';
}
static void con_changed(CON_OBJECT *console) { console->Revision++; }
static void con_sync_input(CON_OBJECT *console) {
    if (console->InputCount || console->CookedReady ||
        console->PendingIndex < console->PendingCount)
        KeSetEvent(&console->InputReady);
    else
        KeResetEvent(&console->InputReady);
}

static void con_delete_screen(POBJECT raw) {
    CON_SCREEN *screen = raw;
    if (screen->Cells)
        ExFreePool(screen->Cells);
}
static void con_delete_console(POBJECT raw) {
    CON_OBJECT *console = raw;
    if (console->Active)
        ObDereferenceObject(console->Active);
    if (console->Input)
        ExFreePool(console->Input);
    if (console->Cooked)
        ExFreePool(console->Cooked);
    if (console->Title)
        ExFreePool(console->Title);
}
static NTSTATUS con_create_screen(CON_OBJECT *console, UINT32 width, UINT32 height,
                                  CON_SCREEN **out) {
    if (!width || !height || width > 32767 || height > 32767)
        return STATUS_INVALID_PARAMETER;
    POBJECT object;
    NTSTATUS status = ObCreateObject(g_screen_type, sizeof(CON_SCREEN), &object);
    if (!NT_SUCCESS(status))
        return status;
    CON_SCREEN *screen = object;
    SeAssignObjectDescriptor(screen, ObHeaderFromObject(console)->SecurityDescriptor);
    screen->Cells = ExAllocatePoolWithTag(NonPagedPool,
                                          (SIZE_T)width * height * sizeof(*screen->Cells), 'ScnC');
    if (!screen->Cells) {
        ObDereferenceObject(object);
        return STATUS_NO_MEMORY;
    }
    screen->Console = console;
    screen->Width = width;
    screen->Height = height;
    screen->Attribute = 7;
    screen->PopupAttribute = 0xf5;
    screen->CursorSize = 25;
    screen->CursorVisible = TRUE;
    screen->Mode = CON_OUTPUT_PROCESSED | CON_OUTPUT_WRAP;
    screen->Window =
        (SV_CONSOLE_RECT){0, 0, (INT16)(width - 1), (INT16)(height < 25 ? height - 1 : 24)};
    static const UINT32 colors[16] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080,
                                      0x008080, 0xc0c0c0, 0x808080, 0xff0000, 0x00ff00, 0xffff00,
                                      0x0000ff, 0xff00ff, 0x00ffff, 0xffffff};
    memcpy(screen->Colors, colors, sizeof(colors));
    for (SIZE_T i = 0; i < (SIZE_T)width * height; i++)
        screen->Cells[i] = (SV_CONSOLE_CELL){' ', 7};
    *out = screen;
    return STATUS_SUCCESS;
}
static NTSTATUS con_set_title(CON_OBJECT *console, const WCHAR *text, SIZE_T count) {
    if (count > 32767)
        return STATUS_INVALID_PARAMETER;
    WCHAR *copy = ExAllocatePoolWithTag(NonPagedPool, (count + 1) * sizeof(WCHAR), 'TinC');
    if (!copy)
        return STATUS_NO_MEMORY;
    if (count)
        memcpy(copy, text, count * sizeof(WCHAR));
    copy[count] = 0;
    WCHAR *old = console->Title;
    console->Title = copy;
    console->TitleLength = count;
    if (old)
        ExFreePool(old);
    con_changed(console);
    return STATUS_SUCCESS;
}
static NTSTATUS con_create_console(PKPROCESS process, CON_OBJECT **out) {
    POBJECT object;
    NTSTATUS status = ObCreateObject(g_console_type, sizeof(CON_OBJECT), &object);
    if (!NT_SUCCESS(status))
        return status;
    CON_OBJECT *console = object;
    SE_DESCRIPTOR *descriptor = NULL;
    status = SeCreateDefaultDescriptor(process->SecurityUserRid, &descriptor);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(object);
        return status;
    }
    SeAssignObjectDescriptor(object, descriptor);
    SeDereferenceDescriptor(descriptor);
    console->InputMode = CONSOLE_LINE_DEFAULT;
    console->InputCP = console->OutputCP = 437;
    console->FontWidth = 10;
    console->FontHeight = 20;
    console->Station = W32kGetProcessWindowStationIdentity(process);
    console->Identity = ++g_console_identity;
    console->Revision = 1;
    console->DisplayAllowed = (process->CreationFlags & CREATE_NO_WINDOW_) == 0;
    console->HostStatus = STATUS_PENDING;
    KeInitializeEvent(&console->InputReady, TRUE, FALSE);
    KeInitializeEvent(&console->HostReady, TRUE, FALSE);
    /* Reader is embedded, so ownership pins the containing Console object;
     * treating &Reader itself as an Object Manager body corrupts its prefix. */
    KeInitializeMutantWithReference(&console->Reader, FALSE, console);
    UINT32 width = process->StartupXChars ? process->StartupXChars : 80;
    UINT32 height = process->StartupYChars ? process->StartupYChars : 300;
    status = con_create_screen(console, width, height, &console->Active);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(object);
        return status;
    }
    if (process->StartupFlags & 8u) {
        console->Active->Attribute = (UINT16)process->StartupFillAttribute;
        for (SIZE_T i = 0; i < (SIZE_T)width * height; i++)
            console->Active->Cells[i].Attribute = console->Active->Attribute;
    }
    WCHAR title[260];
    SIZE_T count = 0;
    const char *name = process->ImagePath[0] ? process->ImagePath : process->Name;
    for (; name && name[count] && count < 259; count++)
        title[count] = (UINT8)name[count];
    status = con_set_title(console, title, count);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(object);
        return status;
    }
    *out = console;
    return STATUS_SUCCESS;
}

static void con_screen_follow_cursor(CON_SCREEN *screen) {
    INT32 rows = screen->Window.Bottom - screen->Window.Top + 1;
    if (screen->CursorY > (UINT32)screen->Window.Bottom) {
        screen->Window.Bottom = (INT16)screen->CursorY;
        screen->Window.Top = (INT16)(screen->CursorY + 1 - (UINT32)rows);
    }
}
static void con_screen_scroll(CON_SCREEN *screen) {
    SIZE_T row = screen->Width, total = (SIZE_T)screen->Width * screen->Height;
    memmove(screen->Cells, screen->Cells + row, (total - row) * sizeof(*screen->Cells));
    for (SIZE_T i = total - row; i < total; i++)
        screen->Cells[i] = (SV_CONSOLE_CELL){' ', screen->Attribute};
    screen->CursorY = screen->Height - 1;
}
static void con_put_wide(CON_SCREEN *screen, WCHAR character) {
    if (screen->Mode & CON_OUTPUT_PROCESSED) {
        if (character == '\r') {
            screen->CursorX = 0;
            screen->WrapPending = FALSE;
            return;
        }
        if (character == '\n') {
            screen->CursorY++;
            if (screen->CursorY >= screen->Height)
                con_screen_scroll(screen);
            con_screen_follow_cursor(screen);
            screen->WrapPending = FALSE;
            return;
        }
        if (character == '\b') {
            if (screen->CursorX)
                screen->CursorX--;
            screen->WrapPending = FALSE;
            return;
        }
        if (character == '\t') {
            UINT32 spaces = 8 - (screen->CursorX & 7);
            while (spaces--)
                con_put_wide(screen, ' ');
            return;
        }
        if (character == '\a')
            return;
    }
    if (screen->WrapPending) {
        screen->CursorX = 0;
        screen->CursorY++;
        screen->WrapPending = FALSE;
        if (screen->CursorY >= screen->Height)
            con_screen_scroll(screen);
    }
    screen->Cells[(SIZE_T)screen->CursorY * screen->Width + screen->CursorX] =
        (SV_CONSOLE_CELL){character, screen->Attribute};
    if (++screen->CursorX >= screen->Width) {
        screen->CursorX = screen->Width - 1;
        if (screen->Mode & CON_OUTPUT_WRAP) {
            if (screen->Mode & 8u)
                screen->WrapPending = TRUE;
            else {
                screen->CursorX = 0;
                screen->CursorY++;
                if (screen->CursorY >= screen->Height)
                    con_screen_scroll(screen);
            }
        }
    }
    con_screen_follow_cursor(screen);
}
static void con_fill_cells(CON_SCREEN *screen, SIZE_T start, SIZE_T count, WCHAR character,
                           UINT16 attribute, BOOLEAN fill_character, BOOLEAN fill_attribute) {
    SIZE_T total = (SIZE_T)screen->Width * screen->Height;
    if (start >= total)
        return;
    if (count > total - start)
        count = total - start;
    for (SIZE_T i = 0; i < count; i++) {
        if (fill_character)
            screen->Cells[start + i].Character = character;
        if (fill_attribute)
            screen->Cells[start + i].Attribute = attribute;
    }
}
static void con_ansi_execute(CON_SCREEN *screen, UINT8 final) {
    UINT32 n = screen->AnsiArguments[0] ? screen->AnsiArguments[0] : 1;
    UINT32 *p = screen->AnsiArguments;
    switch (final) {
    case 'A':
        screen->CursorY = n > screen->CursorY ? 0 : screen->CursorY - n;
        break;
    case 'B':
        screen->CursorY =
            n >= screen->Height - screen->CursorY ? screen->Height - 1 : screen->CursorY + n;
        break;
    case 'C':
        screen->CursorX =
            n >= screen->Width - screen->CursorX ? screen->Width - 1 : screen->CursorX + n;
        break;
    case 'D':
        screen->CursorX = n > screen->CursorX ? 0 : screen->CursorX - n;
        break;
    case 'G':
        screen->CursorX = n > screen->Width ? screen->Width - 1 : n - 1;
        break;
    case 'H':
    case 'f':
        screen->CursorY = p[0] ? p[0] - 1 : 0;
        screen->CursorX = p[1] ? p[1] - 1 : 0;
        if (screen->CursorY >= screen->Height)
            screen->CursorY = screen->Height - 1;
        if (screen->CursorX >= screen->Width)
            screen->CursorX = screen->Width - 1;
        break;
    case 'J': {
        SIZE_T pos = (SIZE_T)screen->CursorY * screen->Width + screen->CursorX,
               total = (SIZE_T)screen->Width * screen->Height;
        if (p[0] == 2 || p[0] == 3)
            con_fill_cells(screen, 0, total, ' ', screen->Attribute, TRUE, TRUE);
        else if (!p[0])
            con_fill_cells(screen, pos, total - pos, ' ', screen->Attribute, TRUE, TRUE);
        else if (p[0] == 1)
            con_fill_cells(screen, 0, pos + 1, ' ', screen->Attribute, TRUE, TRUE);
        break;
    }
    case 'K': {
        SIZE_T base = (SIZE_T)screen->CursorY * screen->Width;
        if (p[0] == 2)
            con_fill_cells(screen, base, screen->Width, ' ', screen->Attribute, TRUE, TRUE);
        else if (p[0] == 1)
            con_fill_cells(screen, base, screen->CursorX + 1, ' ', screen->Attribute, TRUE, TRUE);
        else
            con_fill_cells(screen, base + screen->CursorX, screen->Width - screen->CursorX, ' ',
                           screen->Attribute, TRUE, TRUE);
        break;
    }
    case 'm': {
        static const UINT8 order[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        for (UINT32 i = 0; i <= screen->AnsiCount; i++) {
            UINT32 value = p[i];
            if (!value)
                screen->Attribute = 7;
            else if (value == 1)
                screen->Attribute |= 8;
            else if (value == 22)
                screen->Attribute &= ~8u;
            else if (value >= 30 && value <= 37)
                screen->Attribute = (screen->Attribute & 0xfff0) | order[value - 30];
            else if (value >= 40 && value <= 47)
                screen->Attribute = (screen->Attribute & 0xff0f) | (order[value - 40] << 4);
            else if (value >= 90 && value <= 97)
                screen->Attribute = (screen->Attribute & 0xfff0) | order[value - 90] | 8;
            else if (value >= 100 && value <= 107)
                screen->Attribute = (screen->Attribute & 0xff0f) | ((order[value - 100] | 8) << 4);
            else if (value == 39)
                screen->Attribute = (screen->Attribute & 0xfff0) | 7;
            else if (value == 49)
                screen->Attribute &= 0xff0f;
        }
        break;
    }
    case 's':
        screen->SavedX = screen->CursorX;
        screen->SavedY = screen->CursorY;
        break;
    case 'u':
        screen->CursorX = screen->SavedX < screen->Width ? screen->SavedX : screen->Width - 1;
        screen->CursorY = screen->SavedY < screen->Height ? screen->SavedY : screen->Height - 1;
        break;
    }
    screen->WrapPending = FALSE;
    con_screen_follow_cursor(screen);
}
static void con_write_character(CON_SCREEN *screen, WCHAR character) {
    if (!(screen->Mode & 4u)) {
        con_put_wide(screen, character);
        return;
    }
    if (screen->AnsiState == 1) {
        screen->AnsiState = character == '[' ? 2 : 0;
        if (character == '7') {
            screen->SavedX = screen->CursorX;
            screen->SavedY = screen->CursorY;
        }
        if (character == '8') {
            screen->CursorX = screen->SavedX;
            screen->CursorY = screen->SavedY;
        }
        return;
    }
    if (screen->AnsiState == 2) {
        if (character >= '0' && character <= '9') {
            UINT32 *p = &screen->AnsiArguments[screen->AnsiCount];
            *p = *p > 32767 ? 32767 : *p * 10 + character - '0';
        } else if (character == ';') {
            if (screen->AnsiCount < 15)
                screen->AnsiCount++;
        } else if (character >= 0x40 && character <= 0x7e) {
            con_ansi_execute(screen, (UINT8)character);
            screen->AnsiState = 0;
        } else if (character == 0x1b)
            screen->AnsiState = 1;
        return;
    }
    if (character == 0x1b) {
        screen->AnsiState = 1;
        screen->AnsiCount = 0;
        memset(screen->AnsiArguments, 0, sizeof(screen->AnsiArguments));
        return;
    }
    con_put_wide(screen, character);
}
static void con_write_byte(CON_SCREEN *screen, UINT8 byte) {
    UINT32 codepage = screen->Console->OutputCP;
    if (codepage != 65001) {
        con_write_character(screen, con_decode_byte(codepage, byte));
        return;
    }
    if (screen->UtfRemaining) {
        if ((byte & 0xc0) != 0x80) {
            screen->UtfRemaining = 0;
            con_write_character(screen, 0xfffd);
            con_write_byte(screen, byte);
            return;
        }
        screen->UtfValue = (screen->UtfValue << 6) | (byte & 63);
        if (--screen->UtfRemaining)
            return;
        UINT32 value = screen->UtfValue;
        if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            (screen->UtfTotal == 2 && value < 0x80) || (screen->UtfTotal == 3 && value < 0x800) ||
            (screen->UtfTotal == 4 && value < 0x10000)) {
            con_write_character(screen, 0xfffd);
            return;
        }
        if (value > 0xffff) {
            value -= 0x10000;
            con_write_character(screen, (WCHAR)(0xd800 + (value >> 10)));
            con_write_character(screen, (WCHAR)(0xdc00 + (value & 1023)));
        } else
            con_write_character(screen, (WCHAR)value);
        return;
    }
    if (byte < 0x80) {
        con_write_character(screen, byte);
        return;
    }
    if (byte >= 0xc2 && byte <= 0xdf) {
        screen->UtfRemaining = 1;
        screen->UtfTotal = 2;
        screen->UtfValue = byte & 31;
    } else if (byte >= 0xe0 && byte <= 0xef) {
        screen->UtfRemaining = 2;
        screen->UtfTotal = 3;
        screen->UtfValue = byte & 15;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
        screen->UtfRemaining = 3;
        screen->UtfTotal = 4;
        screen->UtfValue = byte & 7;
    } else
        con_write_character(screen, 0xfffd);
}

static NTSTATUS con_grow_input(CON_OBJECT *console) {
    UINT32 capacity = console->InputCapacity ? console->InputCapacity * 2 : 256;
    if (capacity < console->InputCapacity)
        return STATUS_NO_MEMORY;
    SV_CONSOLE_INPUT_RECORD *records =
        ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)capacity * sizeof(*records), 'InnC');
    if (!records)
        return STATUS_NO_MEMORY;
    for (UINT32 i = 0; i < console->InputCount; i++)
        records[i] = console->Input[(console->InputHead + i) % console->InputCapacity];
    if (console->Input)
        ExFreePool(console->Input);
    console->Input = records;
    console->InputHead = 0;
    console->InputCapacity = capacity;
    return STATUS_SUCCESS;
}
static NTSTATUS con_enqueue_input(CON_OBJECT *console, const SV_CONSOLE_INPUT_RECORD *record) {
    if (console->Closing)
        return STATUS_CANCELLED;
    if (console->InputCount == console->InputCapacity) {
        NTSTATUS status = con_grow_input(console);
        if (!NT_SUCCESS(status))
            return status;
    }
    console->Input[(console->InputHead + console->InputCount) % console->InputCapacity] = *record;
    console->InputCount++;
    con_sync_input(console);
    return STATUS_SUCCESS;
}
static BOOLEAN con_pop_input(CON_OBJECT *console, SV_CONSOLE_INPUT_RECORD *record) {
    if (!console->InputCount)
        return FALSE;
    *record = console->Input[console->InputHead];
    console->InputHead = (console->InputHead + 1) % console->InputCapacity;
    console->InputCount--;
    con_sync_input(console);
    return TRUE;
}
static NTSTATUS con_grow_cooked(CON_OBJECT *console, SIZE_T count) {
    if (count <= console->CookedCapacity)
        return STATUS_SUCCESS;
    SIZE_T capacity = console->CookedCapacity ? console->CookedCapacity * 2 : 128;
    if (capacity < count)
        capacity = count;
    if (capacity > ((SIZE_T)-1) / sizeof(WCHAR))
        return STATUS_NO_MEMORY;
    WCHAR *buffer = ExAllocatePoolWithTag(NonPagedPool, capacity * sizeof(WCHAR), 'LnnC');
    if (!buffer)
        return STATUS_NO_MEMORY;
    if (console->CookedCount)
        memcpy(buffer, console->Cooked, console->CookedCount * sizeof(WCHAR));
    if (console->Cooked)
        ExFreePool(console->Cooked);
    console->Cooked = buffer;
    console->CookedCapacity = capacity;
    return STATUS_SUCCESS;
}
static const char *con_navigation(UINT16 key) {
    switch (key) {
    case 0x25:
        return "\033[D";
    case 0x26:
        return "\033[A";
    case 0x27:
        return "\033[C";
    case 0x28:
        return "\033[B";
    case 0x24:
        return "\033[H";
    case 0x23:
        return "\033[F";
    case 0x2e:
        return "\033[3~";
    case 0x2d:
        return "\033[2~";
    case 0x21:
        return "\033[5~";
    case 0x22:
        return "\033[6~";
    default:
        return NULL;
    }
}
static BOOLEAN con_next_character(CON_OBJECT *console, WCHAR *character) {
    if (console->PendingIndex < console->PendingCount) {
        *character = console->PendingBytes[console->PendingIndex++];
        if (console->PendingIndex == console->PendingCount)
            console->PendingIndex = console->PendingCount = 0;
        return TRUE;
    }
    SV_CONSOLE_INPUT_RECORD record;
    while (con_pop_input(console, &record)) {
        if (record.EventType != 1 || !record.Event.Key.KeyDown)
            continue;
        console->LastReadControlState = record.Event.Key.ControlState;
        if (record.Event.Key.Repeat > 1) {
            record.Event.Key.Repeat--;
            console->InputHead =
                (console->InputHead + console->InputCapacity - 1) % console->InputCapacity;
            console->Input[console->InputHead] = record;
            console->InputCount++;
            con_sync_input(console);
        }
        if (record.Event.Key.Character) {
            *character = record.Event.Key.Character;
            return TRUE;
        }
        if (!(console->InputMode & CON_INPUT_LINE) &&
            (console->LegacyInput || (console->InputMode & 0x200))) {
            const char *sequence = con_navigation(record.Event.Key.VirtualKey);
            if (sequence) {
                SIZE_T length = strlen(sequence);
                memcpy(console->PendingBytes, sequence, length);
                console->PendingCount = (UINT8)length;
                console->PendingIndex = 1;
                *character = (UINT8)sequence[0];
                return TRUE;
            }
        }
    }
    return FALSE;
}
static NTSTATUS con_read_wide(CON_OBJECT *console, WCHAR *out, SIZE_T capacity, SIZE_T *read,
                              BOOLEAN wait) {
    *read = 0;
    if (!capacity)
        return STATUS_SUCCESS;
    NTSTATUS lock = wait ? KeWaitForSingleObject(&console->Reader.Header)
                         : KeWaitForSingleObjectTimeout(&console->Reader.Header, 0);
    if (!NT_SUCCESS(lock) || lock == STATUS_TIMEOUT)
        return wait ? lock : STATUS_SUCCESS;
    NTSTATUS status = STATUS_SUCCESS;
    for (;;) {
        UINT64 flags = KiIrqSave();
        CON_ATTACHMENT *reader = con_current_attachment();
        if (!reader || reader->Console != console) {
            KiIrqRestore(flags);
            status = STATUS_INVALID_HANDLE;
            break;
        }
        if (console->Closing) {
            KiIrqRestore(flags);
            status = STATUS_CANCELLED;
            break;
        }
        if (console->InputMode & CON_INPUT_LINE) {
            while (!console->CookedReady) {
                WCHAR character;
                if (!con_next_character(console, &character))
                    break;
                if (character < 32 && (console->ReadWakeupMask & (1u << character))) {
                    status = con_grow_cooked(console, console->CookedCount + 1);
                    if (!NT_SUCCESS(status))
                        break;
                    console->Cooked[console->CookedCount++] = character;
                    console->CookedReady = TRUE;
                    break;
                }
                if (character == '\b') {
                    if (console->CookedCount) {
                        console->CookedCount--;
                        if (console->CookedCount &&
                            console->Cooked[console->CookedCount] >= 0xdc00 &&
                            console->Cooked[console->CookedCount] <= 0xdfff &&
                            console->Cooked[console->CookedCount - 1] >= 0xd800 &&
                            console->Cooked[console->CookedCount - 1] <= 0xdbff)
                            console->CookedCount--;
                        if (console->InputMode & CON_INPUT_ECHO) {
                            con_put_wide(console->Active, '\b');
                            con_put_wide(console->Active, ' ');
                            con_put_wide(console->Active, '\b');
                            con_changed(console);
                        }
                    }
                    continue;
                }
                status = con_grow_cooked(console, console->CookedCount + 2);
                if (!NT_SUCCESS(status))
                    break;
                if (character == '\r' || character == '\n') {
                    console->Cooked[console->CookedCount++] = '\r';
                    console->Cooked[console->CookedCount++] = '\n';
                    console->CookedReady = TRUE;
                    if (console->InputMode & CON_INPUT_ECHO) {
                        con_put_wide(console->Active, '\r');
                        con_put_wide(console->Active, '\n');
                        con_changed(console);
                    }
                } else {
                    console->Cooked[console->CookedCount++] = character;
                    if (console->InputMode & CON_INPUT_ECHO) {
                        con_put_wide(console->Active, character);
                        con_changed(console);
                    }
                }
            }
            if (!NT_SUCCESS(status)) {
                KiIrqRestore(flags);
                break;
            }
            if (console->CookedReady) {
                SIZE_T count = console->CookedCount - console->CookedRead;
                if (count > capacity)
                    count = capacity;
                if (MmIsUserAddress((UINT64)out) &&
                    !MmProbeForWrite((UINT64)out, count * sizeof(WCHAR))) {
                    KiIrqRestore(flags);
                    status = STATUS_ACCESS_VIOLATION;
                    break;
                }
                memcpy(out, console->Cooked + console->CookedRead, count * sizeof(WCHAR));
                console->CookedRead += count;
                *read = count;
                if (console->CookedRead == console->CookedCount) {
                    console->CookedRead = console->CookedCount = 0;
                    console->CookedReady = FALSE;
                }
                con_sync_input(console);
                KiIrqRestore(flags);
                break;
            }
        } else {
            WCHAR character;
            while (*read < capacity && con_next_character(console, &character)) {
                if (MmIsUserAddress((UINT64)(out + *read)) &&
                    !MmProbeForWrite((UINT64)(out + *read), sizeof(WCHAR))) {
                    status = STATUS_ACCESS_VIOLATION;
                    break;
                }
                out[(*read)++] = character;
                if (console->InputMode & CON_INPUT_ECHO) {
                    con_put_wide(console->Active, character);
                    con_changed(console);
                }
            }
            con_sync_input(console);
            if (!NT_SUCCESS(status)) {
                KiIrqRestore(flags);
                break;
            }
            if (*read) {
                KiIrqRestore(flags);
                break;
            }
        }
        con_sync_input(console);
        KiIrqRestore(flags);
        if (!wait)
            break;
        status = KeWaitForSingleObject(&console->InputReady.Header);
        if (!NT_SUCCESS(status))
            break;
    }
    KeReleaseMutant(&console->Reader, NULL);
    return status;
}
static NTSTATUS con_read_wide_control(CON_OBJECT *console, WCHAR *out, SIZE_T capacity,
                                      SIZE_T *read, UINT64 address) {
    if (!address)
        return con_read_wide(console, out, capacity, read, TRUE);
    SV_CONSOLE_READ_CONTROL control;
    if (!MmProbeForRead(address, sizeof(control)) || !MmProbeForWrite(address, sizeof(control)))
        return STATUS_ACCESS_VIOLATION;
    memcpy(&control, (void *)address, sizeof(control));
    if (control.Size != sizeof(control) || control.InitialChars > capacity ||
        !MmProbeForRead((UINT64)out, (SIZE_T)control.InitialChars * sizeof(WCHAR)))
        return STATUS_INVALID_PARAMETER;
    NTSTATUS status = KeWaitForSingleObject(&console->Reader.Header);
    if (!NT_SUCCESS(status))
        return status;
    UINT64 flags = KiIrqSave();
    if (!MmProbeForRead((UINT64)out, (SIZE_T)control.InitialChars * sizeof(WCHAR)))
        status = STATUS_ACCESS_VIOLATION;
    else if (control.InitialChars && console->CookedCount)
        status = STATUS_INVALID_PARAMETER;
    else
        status = con_grow_cooked(console, control.InitialChars);
    if (NT_SUCCESS(status)) {
        if (control.InitialChars) {
            memcpy(console->Cooked, out, (SIZE_T)control.InitialChars * sizeof(WCHAR));
            console->CookedCount = control.InitialChars;
        }
        console->ReadWakeupMask = control.WakeupMask;
    }
    KiIrqRestore(flags);
    if (NT_SUCCESS(status))
        status = con_read_wide(console, out, capacity, read, TRUE);
    flags = KiIrqSave();
    console->ReadWakeupMask = 0;
    control.ControlKeyState = console->LastReadControlState;
    if (MmProbeForWrite(address, sizeof(control)))
        memcpy((void *)address, &control, sizeof(control));
    else
        status = STATUS_ACCESS_VIOLATION;
    KiIrqRestore(flags);
    KeReleaseMutant(&console->Reader, NULL);
    return status;
}

static NTSTATUS con_endpoint_access(CON_ENDPOINT *endpoint, BOOLEAN write) {
    CON_ATTACHMENT *attachment = con_current_attachment();
    if (!attachment || attachment->Console != endpoint->Console || endpoint->Console->Closing)
        return STATUS_INVALID_HANDLE;
    return write == endpoint->Input ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
}
static SIZE_T con_endpoint_read_common(void *context, void *buffer, SIZE_T length, BOOLEAN wait,
                                       NTSTATUS *result) {
    CON_ENDPOINT *endpoint = context;
    CON_OBJECT *console = endpoint->Console;
    *result = con_endpoint_access(endpoint, FALSE);
    if (!NT_SUCCESS(*result))
        return 0;
    UINT8 *out = buffer;
    SIZE_T done = 0;
    while (done < length) {
        UINT64 flags = KiIrqSave();
        while (done < length && console->PendingIndex < console->PendingCount) {
            if (MmIsUserAddress((UINT64)(out + done)) &&
                !MmProbeForWrite((UINT64)(out + done), 1)) {
                *result = STATUS_ACCESS_VIOLATION;
                KiIrqRestore(flags);
                return done;
            }
            out[done++] = console->PendingBytes[console->PendingIndex++];
        }
        if (console->PendingIndex == console->PendingCount)
            console->PendingIndex = console->PendingCount = 0;
        KiIrqRestore(flags);
        if (done == length)
            break;
        WCHAR character;
        SIZE_T read = 0;
        NTSTATUS status = con_read_wide(console, &character, 1, &read, wait && !done);
        if (!NT_SUCCESS(status) || !read) {
            *result = status;
            break;
        }
        flags = KiIrqSave();
        if (MmIsUserAddress((UINT64)(out + done)) &&
            !MmProbeForWrite((UINT64)(out + done), length - done)) {
            *result = STATUS_ACCESS_VIOLATION;
            KiIrqRestore(flags);
            return done;
        }
        if (console->InputCP == 65001) {
            UINT32 value = character;
            if (character >= 0xd800 && character <= 0xdbff) {
                KiIrqRestore(flags);
                WCHAR low;
                SIZE_T low_read = 0;
                status = con_read_wide(console, &low, 1, &low_read, wait);
                flags = KiIrqSave();
                value = NT_SUCCESS(status) && low_read && low >= 0xdc00 && low <= 0xdfff
                            ? 0x10000 + ((character - 0xd800) << 10) + (low - 0xdc00)
                            : 0xfffd;
            } else if (character >= 0xdc00 && character <= 0xdfff)
                value = 0xfffd;
            UINT8 bytes[4];
            UINT32 count;
            if (value < 0x80) {
                bytes[0] = (UINT8)value;
                count = 1;
            } else if (value < 0x800) {
                bytes[0] = 0xc0 | (value >> 6);
                bytes[1] = 0x80 | (value & 63);
                count = 2;
            } else if (value < 0x10000) {
                bytes[0] = 0xe0 | (value >> 12);
                bytes[1] = 0x80 | ((value >> 6) & 63);
                bytes[2] = 0x80 | (value & 63);
                count = 3;
            } else {
                bytes[0] = 0xf0 | (value >> 18);
                bytes[1] = 0x80 | ((value >> 12) & 63);
                bytes[2] = 0x80 | ((value >> 6) & 63);
                bytes[3] = 0x80 | (value & 63);
                count = 4;
            }
            for (UINT32 i = 0; i < count; i++)
                if (done < length)
                    out[done++] = bytes[i];
                else
                    console->PendingBytes[console->PendingCount++] = bytes[i];
        } else
            out[done++] = con_encode_byte(console->InputCP, character);
        KiIrqRestore(flags);
        if ((console->InputMode & CON_INPUT_LINE) && character == '\n')
            break;
        if (!wait)
            break;
    }
    return done;
}
static SIZE_T con_endpoint_read(void *context, void *buffer, SIZE_T length) {
    NTSTATUS status;
    return con_endpoint_read_common(context, buffer, length, TRUE, &status);
}
static SIZE_T con_endpoint_try_read(void *context, void *buffer, SIZE_T length) {
    NTSTATUS status;
    return con_endpoint_read_common(context, buffer, length, FALSE, &status);
}
static NTSTATUS con_endpoint_read_result(void *context, void *buffer, SIZE_T length, BOOLEAN wait,
                                         SIZE_T *read) {
    CON_ENDPOINT *endpoint = context;
    *read = 0;
    NTSTATUS status = con_endpoint_access(endpoint, FALSE);
    if (!NT_SUCCESS(status))
        return status;
    status = wait ? KeWaitForSingleObject(&endpoint->Console->Reader.Header)
                  : KeWaitForSingleObjectTimeout(&endpoint->Console->Reader.Header, 0);
    if (status == STATUS_TIMEOUT)
        return STATUS_SUCCESS;
    if (!NT_SUCCESS(status))
        return status;
    *read = con_endpoint_read_common(context, buffer, length, wait, &status);
    KeReleaseMutant(&endpoint->Console->Reader, NULL);
    return status;
}
static BOOLEAN con_endpoint_input_available(void *context) {
    CON_ENDPOINT *endpoint = context;
    CON_OBJECT *console = endpoint->Console;
    return endpoint->Input && (console->InputCount || console->CookedReady ||
                               console->PendingIndex < console->PendingCount);
}
static SIZE_T con_endpoint_write(void *context, const void *buffer, SIZE_T length) {
    CON_ENDPOINT *endpoint = context;
    if (!NT_SUCCESS(con_endpoint_access(endpoint, TRUE)))
        return 0;
    UINT64 flags = KiIrqSave();
    const UINT8 *bytes = buffer;
    for (SIZE_T i = 0; i < length; i++)
        con_write_byte(endpoint->Screen, bytes[i]);
    con_changed(endpoint->Console);
    KiIrqRestore(flags);
    return length;
}
static NTSTATUS con_endpoint_write_result(void *context, const void *buffer, SIZE_T length,
                                          SIZE_T *written) {
    *written = 0;
    CON_ENDPOINT *endpoint = context;
    NTSTATUS status = con_endpoint_access(endpoint, TRUE);
    if (!NT_SUCCESS(status))
        return status;
    *written = con_endpoint_write(context, buffer, length);
    return STATUS_SUCCESS;
}
static void con_endpoint_close(void *context) {
    CON_ENDPOINT *endpoint = context;
    CON_OBJECT *console = endpoint->Console;
    if (endpoint->Screen)
        ObDereferenceObject(endpoint->Screen);
    ExFreePool(endpoint);
    ObDereferenceObject(console);
}
static void con_endpoint_prepare_windows(void *context) { (void)context; }
static NTSTATUS con_endpoint_control(void *context, UINT32 operation, UINT64 a, UINT64 b, UINT64 c,
                                     UINT64 *result);
static const IO_CHAR_DEVICE_OPS con_endpoint_ops = {.Read = con_endpoint_read,
                                                    .TryRead = con_endpoint_try_read,
                                                    .InputAvailable = con_endpoint_input_available,
                                                    .Write = con_endpoint_write,
                                                    .PrepareWindows = con_endpoint_prepare_windows,
                                                    .ConsoleControl = con_endpoint_control,
                                                    .Close = con_endpoint_close,
                                                    .ReadResult = con_endpoint_read_result,
                                                    .WriteResult = con_endpoint_write_result};

static NTSTATUS con_open_endpoint_access(PKPROCESS process, CON_OBJECT *console, CON_SCREEN *screen,
                                         BOOLEAN input, ACCESS_MASK access, BOOLEAN check,
                                         HANDLE *out) {
    POBJECT source = screen ? (POBJECT)screen : (POBJECT)console;
    if (check) {
        ACCESS_MASK granted = 0;
        NTSTATUS status =
            SeAccessCheck(ObHeaderFromObject(source)->SecurityDescriptor, process->SecurityUserRid,
                          access, &ObHeaderFromObject(source)->Type->GenericMapping, &granted);
        if (!NT_SUCCESS(status))
            return status;
    }
    CON_ENDPOINT *endpoint = ExAllocatePoolWithTag(NonPagedPool, sizeof(*endpoint), 'EdnC');
    if (!endpoint)
        return STATUS_NO_MEMORY;
    *endpoint = (CON_ENDPOINT){.Console = console, .Screen = screen, .Input = input};
    ObReferenceObject(console);
    if (screen)
        ObReferenceObject(screen);
    NTSTATUS status = IoCreateCharDeviceFileInTable(
        process->HandleTable, IO_CHAR_DEVICE_TAG_CONSOLE, endpoint, &con_endpoint_ops, access, out);
    if (!NT_SUCCESS(status))
        con_endpoint_close(endpoint);
    return status;
}
static NTSTATUS con_open_endpoint(PKPROCESS process, CON_OBJECT *console, CON_SCREEN *screen,
                                  BOOLEAN input, HANDLE *out) {
    return con_open_endpoint_access(process, console, screen, input,
                                    GENERIC_READ | GENERIC_WRITE | 0x00100000u, TRUE, out);
}
static NTSTATUS con_attach_process(PKPROCESS process, CON_OBJECT *console, UINT32 group,
                                   BOOLEAN replace_handles) {
    if (process->NativeConsole)
        return STATUS_ACCESS_DENIED;
    CON_ATTACHMENT *attachment = ExAllocatePoolWithTag(NonPagedPool, sizeof(*attachment), 'AtnC');
    if (!attachment)
        return STATUS_NO_MEMORY;
    *attachment = (CON_ATTACHMENT){.Console = console, .Process = process, .Group = group};
    HANDLE input = NULL, output = NULL, error = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    if (replace_handles) {
        status = con_open_endpoint(process, console, NULL, TRUE, &input);
        if (NT_SUCCESS(status))
            status = con_open_endpoint(process, console, console->Active, FALSE, &output);
        if (NT_SUCCESS(status))
            status = con_open_endpoint(process, console, console->Active, FALSE, &error);
        if (!NT_SUCCESS(status)) {
            con_close_handle(process, input);
            con_close_handle(process, output);
            ExFreePool(attachment);
            return status;
        }
    }
    ObReferenceObject(console);
    UINT64 flags = KiIrqSave();
    attachment->Next = console->Attachments;
    console->Attachments = attachment;
    process->NativeConsole = attachment;
    if (replace_handles) {
        process->StandardInput = input;
        process->StandardOutput = output;
        process->StandardError = error;
    }
    con_changed(console);
    KiIrqRestore(flags);
    return STATUS_SUCCESS;
}

NTSTATUS ConOpenProcessConsole(BOOLEAN input, HANDLE *out) {
    CON_ATTACHMENT *attachment = con_current_attachment();
    if (!attachment)
        return STATUS_INVALID_HANDLE;
    return con_open_endpoint(attachment->Process, attachment->Console,
                             input ? NULL : attachment->Console->Active, input, out);
}
void ConDetachProcessConsole(PKPROCESS process) {
    CON_ATTACHMENT *attachment = con_attachment(process);
    if (!attachment)
        return;
    CON_OBJECT *console = attachment->Console;
    UINT64 flags = KiIrqSave();
    HANDLE handles[3] = {process->StandardInput, process->StandardOutput, process->StandardError};
    HANDLE *slots[3] = {&process->StandardInput, &process->StandardOutput, &process->StandardError};
    for (UINT32 i = 0; i < 3; i++) {
        BOOLEAN already_closed = FALSE;
        for (UINT32 j = 0; j < i; j++)
            if (handles[i] && handles[i] == handles[j] && !*slots[j])
                already_closed = TRUE;
        if (already_closed) {
            *slots[i] = NULL;
            continue;
        }
        UINT64 tag = 0;
        void *context = NULL;
        if (handles[i] &&
            NT_SUCCESS(
                IoQueryCharDeviceFileInTable(process->HandleTable, handles[i], &tag, &context)) &&
            tag == IO_CHAR_DEVICE_TAG_CONSOLE && ((CON_ENDPOINT *)context)->Console == console) {
            con_close_handle(process, handles[i]);
            *slots[i] = NULL;
        }
    }
    CON_ATTACHMENT **link = &console->Attachments;
    while (*link && *link != attachment)
        link = &(*link)->Next;
    if (*link)
        *link = attachment->Next;
    process->NativeConsole = NULL;
    if (console->HostProcessId == process->ProcessId) {
        if (!console->HostWindow) {
            console->HostStatus =
                NT_SUCCESS(process->ExitStatus) ? STATUS_UNSUCCESSFUL : process->ExitStatus;
            KeSetEvent(&console->HostReady);
        }
        console->HostProcessId = 0;
        console->HostWindow = 0;
        console->HostRequested = FALSE;
    }
    if (console->ForegroundProcessId == process->ProcessId)
        console->ForegroundProcessId = 0;
    con_changed(console);
    KeSetEvent(&console->InputReady); /* detached readers must recheck their attachment */
    if (!console->Attachments) {
        console->Closing = TRUE;
        console->HostStatus = STATUS_CANCELLED;
        KeSetEvent(&console->HostReady);
        KeSetEvent(&console->InputReady);
    }
    KiIrqRestore(flags);
    ExFreePool(attachment);
    ObDereferenceObject(console);
}
NTSTATUS ConInitializeProcessConsole(PKPROCESS process, PKPROCESS parent, ULONG creation_flags,
                                     ULONG image_subsystem, HANDLE input, HANDLE output,
                                     HANDLE error) {
    (void)input;
    (void)output;
    (void)error;
    if (process->NativeConsole)
        return STATUS_SUCCESS;
    if (creation_flags & DETACHED_PROCESS_)
        return STATUS_SUCCESS;
    CON_ATTACHMENT *source = con_attachment(parent);
    CON_OBJECT *console = source ? source->Console : NULL;
    if (KeGetCurrentThread() == g_console_broker && g_broker_console)
        console = g_broker_console;
    /* The boot command processor owns the original framebuffer/serial console.
     * Its CUI descendants inherit that console instead of manufacturing a new
     * hosted console for every external command such as CLS.EXE. */
    if (!console && image_subsystem == 3 && parent &&
        (parent->LegacyConsole || !parent->Object) &&
        !(creation_flags & (CREATE_NEW_CONSOLE_ | CREATE_NO_WINDOW_))) {
        process->LegacyConsole = TRUE;
        return STATUS_SUCCESS;
    }
    BOOLEAN independent = (creation_flags & CREATE_NEW_CONSOLE_) ||
                          (!console && image_subsystem == 3 && parent && parent->Object);
    if (creation_flags & CREATE_NO_WINDOW_)
        independent = image_subsystem == 3;
    if (!console && !independent)
        return STATUS_SUCCESS; /* boot/serial console retains its device */
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN created = FALSE;
    if (independent) {
        status = con_create_console(process, &console);
        if (!NT_SUCCESS(status))
            return status;
        created = TRUE;
    }
    UINT32 group = creation_flags & CREATE_NEW_PROCESS_GROUP_ ? process->ProcessId
                   : source                                   ? source->Group
                                                              : process->ProcessId;
    status = con_attach_process(process, console, group,
                                independent && !(process->StartupFlags & STARTF_USESTDHANDLES_));
    if (NT_SUCCESS(status))
        con_attachment(process)->IgnoreCtrlC =
            (creation_flags & CREATE_NEW_PROCESS_GROUP_) != 0 || (source && source->IgnoreCtrlC);
    if (NT_SUCCESS(status) && KeGetCurrentThread() == g_console_broker &&
        g_broker_console == console)
        console->HostProcessId = process->ProcessId;
    if (created)
        ObDereferenceObject(console);
    return status;
}

static void con_host_broker(PVOID context) {
    (void)context;
    for (;;) {
        KeWaitForSingleObject(&g_host_start_event.Header);
        UINT64 flags = KiIrqSave();
        CON_HOST_START *request = g_host_start_head;
        if (request) {
            g_host_start_head = request->Next;
            if (!g_host_start_head)
                g_host_start_tail = &g_host_start_head;
        }
        if (!g_host_start_head)
            KeResetEvent(&g_host_start_event);
        KiIrqRestore(flags);
        if (!request)
            continue;
        PKPROCESS process = PsGetProcessFromObject(request->Process);
        CON_ATTACHMENT *attachment = con_attachment(process);
        if (process && !process->Exiting && attachment) {
            char command[128], number[16];
            UINT32 id = process->ProcessId, n = 0;
            do {
                number[n++] = (char)('0' + id % 10);
                id /= 10;
            } while (id);
            const char prefix[] = "C:\\WINDOWS\\SYSTEM32\\CONHOST.EXE --attach ";
            SIZE_T at = sizeof(prefix) - 1;
            memcpy(command, prefix, at);
            while (n)
                command[at++] = number[--n];
            command[at] = 0;
            g_broker_console = attachment->Console;
            HANDLE host = NULL;
            NTSTATUS status = PsCreateConsoleHost(process, command, &host);
            g_broker_console = NULL;
            if (host)
                ObCloseHandle(host);
            if (!NT_SUCCESS(status)) {
                attachment->Console->HostRequested = FALSE;
                attachment->Console->HostStatus = status;
                KeSetEvent(&attachment->Console->HostReady);
                KeLog("[console] conhost creation failed for PID %u: 0x%08x\n", process->ProcessId,
                      status);
            }
        }
        ObDereferenceObject(request->Process);
        ExFreePool(request);
    }
}
void ConProcessConsolePublished(PKPROCESS process) {
    CON_ATTACHMENT *attachment = con_attachment(process);
    if (!attachment || !attachment->Console->DisplayAllowed || process->Exiting)
        return;
    CON_OBJECT *console = attachment->Console;
    UINT64 flags = KiIrqSave();
    if (console->HostRequested || console->HostProcessId) {
        KiIrqRestore(flags);
        return;
    }
    CON_HOST_START *request = ExAllocatePoolWithTag(NonPagedPool, sizeof(*request), 'QhnC');
    if (!request) {
        console->HostStatus = STATUS_NO_MEMORY;
        KeSetEvent(&console->HostReady);
        KiIrqRestore(flags);
        return;
    }
    request->Next = NULL;
    request->Process = process->Object;
    ObReferenceObject(request->Process);
    if (!g_console_broker)
        g_console_broker = KeCreateThread("Console host broker", con_host_broker, NULL, 10);
    if (!g_console_broker) {
        ObDereferenceObject(request->Process);
        ExFreePool(request);
        console->HostStatus = STATUS_NO_MEMORY;
        KeSetEvent(&console->HostReady);
        KiIrqRestore(flags);
        return;
    }
    *g_host_start_tail = request;
    g_host_start_tail = &request->Next;
    console->HostRequested = TRUE;
    console->HostStatus = STATUS_PENDING;
    KeResetEvent(&console->HostReady);
    KeSetEvent(&g_host_start_event);
    KiIrqRestore(flags);
}
NTSTATUS ConWaitForProcessConsoleHost(PKPROCESS process) {
    CON_ATTACHMENT *attachment = con_attachment(process);
    if (!attachment || !attachment->Console->DisplayAllowed ||
        attachment->Console->HostProcessId == process->ProcessId)
        return STATUS_SUCCESS;
    CON_OBJECT *console = attachment->Console;
    ObReferenceObject(console);
    PVOID pin = IoPinThreadConsoleObject(console);
    if (!pin) {
        ObDereferenceObject(console);
        return STATUS_NO_MEMORY;
    }
    NTSTATUS status = KeWaitForSingleObject(&console->HostReady.Header);
    if (NT_SUCCESS(status)) {
        attachment = con_attachment(process);
        status =
            attachment && attachment->Console == console ? console->HostStatus : STATUS_CANCELLED;
    }
    IoUnpinThreadConsoleObject(pin);
    ObDereferenceObject(console);
    return status;
}
void ConInitializeHostedConsoles(void) {
    if (g_console_type)
        return;
    g_console_type = ObCreateObjectType("Console", con_delete_console);
    g_screen_type = ObCreateObjectType("ConsoleScreenBuffer", con_delete_screen);
    if (!g_console_type || !g_screen_type)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "console object types");
    SeConfigureType(g_console_type, 0x120089u, 0x120116u, 0x1200a0u, 0x1f01ffu);
    SeConfigureType(g_screen_type, 0x120089u, 0x120116u, 0x1200a0u, 0x1f01ffu);
    KeInitializeEvent(&g_host_start_event, TRUE, FALSE);
    PsRegisterConsoleProcessInitializer(ConInitializeProcessConsole);
    PsRegisterConsoleProcessPublished(ConProcessConsolePublished);
}

static NTSTATUS con_resize_screen(CON_SCREEN *screen, UINT32 width, UINT32 height) {
    if (!width || !height || width > 32767 || height > 32767 ||
        width < (UINT32)(screen->Window.Right - screen->Window.Left + 1) ||
        height < (UINT32)(screen->Window.Bottom - screen->Window.Top + 1))
        return STATUS_INVALID_PARAMETER;
    SV_CONSOLE_CELL *cells =
        ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)width * height * sizeof(*cells), 'RsnC');
    if (!cells)
        return STATUS_NO_MEMORY;
    for (SIZE_T i = 0; i < (SIZE_T)width * height; i++)
        cells[i] = (SV_CONSOLE_CELL){' ', screen->Attribute};
    UINT32 rows = height < screen->Height ? height : screen->Height,
           cols = width < screen->Width ? width : screen->Width;
    for (UINT32 row = 0; row < rows; row++)
        memcpy(cells + (SIZE_T)row * width, screen->Cells + (SIZE_T)row * screen->Width,
               (SIZE_T)cols * sizeof(*cells));
    ExFreePool(screen->Cells);
    screen->Cells = cells;
    screen->Width = width;
    screen->Height = height;
    if (screen->CursorX >= width)
        screen->CursorX = width - 1;
    if (screen->CursorY >= height)
        screen->CursorY = height - 1;
    INT32 view_width = screen->Window.Right - screen->Window.Left + 1,
          view_height = screen->Window.Bottom - screen->Window.Top + 1;
    if ((UINT32)screen->Window.Right >= width) {
        screen->Window.Right = (INT16)(width - 1);
        screen->Window.Left = (INT16)(width - view_width);
    }
    if ((UINT32)screen->Window.Bottom >= height) {
        screen->Window.Bottom = (INT16)(height - 1);
        screen->Window.Top = (INT16)(height - view_height);
    }
    screen->WrapPending = FALSE;
    con_changed(screen->Console);
    return STATUS_SUCCESS;
}
static NTSTATUS con_set_window(CON_SCREEN *screen, const SV_CONSOLE_RECT *requested,
                               BOOLEAN absolute) {
    INT32 left = requested->Left, top = requested->Top, right = requested->Right,
          bottom = requested->Bottom;
    if (!absolute) {
        left += screen->Window.Left;
        right += screen->Window.Right;
        top += screen->Window.Top;
        bottom += screen->Window.Bottom;
    }
    if (left < 0 || top < 0 || right < left || bottom < top || right >= (INT32)screen->Width ||
        bottom >= (INT32)screen->Height)
        return STATUS_INVALID_PARAMETER;
    UINT32 maximum_width =
        GfxAvailable() ? GfxFramebuffer.Width / screen->Console->FontWidth : 32767;
    UINT32 maximum_height =
        GfxAvailable() ? GfxFramebuffer.Height / screen->Console->FontHeight : 32767;
    if ((UINT32)(right - left + 1) > maximum_width || (UINT32)(bottom - top + 1) > maximum_height)
        return STATUS_INVALID_PARAMETER;
    screen->Window = (SV_CONSOLE_RECT){(INT16)left, (INT16)top, (INT16)right, (INT16)bottom};
    con_changed(screen->Console);
    return STATUS_SUCCESS;
}
static NTSTATUS con_read_input_records(CON_OBJECT *console, SV_CONSOLE_INPUT_RECORD *out,
                                       SIZE_T capacity, SIZE_T *read, BOOLEAN peek, BOOLEAN wait) {
    *read = 0;
    if (!capacity)
        return STATUS_SUCCESS;
    for (;;) {
        UINT64 flags = KiIrqSave();
        if (console->Closing) {
            KiIrqRestore(flags);
            return STATUS_CANCELLED;
        }
        SIZE_T count = console->InputCount;
        if (count > capacity)
            count = capacity;
        if (count) {
            if (MmIsUserAddress((UINT64)out) &&
                !MmProbeForWrite((UINT64)out, count * sizeof(*out))) {
                KiIrqRestore(flags);
                return STATUS_ACCESS_VIOLATION;
            }
            for (SIZE_T i = 0; i < count; i++)
                out[i] = console->Input[(console->InputHead + (UINT32)i) % console->InputCapacity];
            if (!peek) {
                console->InputHead = (console->InputHead + (UINT32)count) % console->InputCapacity;
                console->InputCount -= (UINT32)count;
                con_sync_input(console);
            }
            *read = count;
            KiIrqRestore(flags);
            return STATUS_SUCCESS;
        }
        KiIrqRestore(flags);
        if (!wait || peek)
            return STATUS_SUCCESS;
        NTSTATUS status = KeWaitForSingleObject(&console->InputReady.Header);
        if (!NT_SUCCESS(status))
            return status;
    }
}
static NTSTATUS con_transfer_output(CON_SCREEN *screen, UINT64 address, BOOLEAN write) {
    if (!MmProbeForRead(address, sizeof(SV_CONSOLE_BUFFER_TRANSFER)) ||
        !MmProbeForWrite(address, sizeof(SV_CONSOLE_BUFFER_TRANSFER)))
        return STATUS_ACCESS_VIOLATION;
    SV_CONSOLE_BUFFER_TRANSFER transfer;
    memcpy(&transfer, (void *)address, sizeof(transfer));
    if (transfer.Width <= 0 || transfer.Height <= 0 || transfer.X < 0 || transfer.Y < 0 ||
        transfer.X >= transfer.Width || transfer.Y >= transfer.Height)
        return STATUS_INVALID_PARAMETER;
    SIZE_T bytes = (SIZE_T)transfer.Width * transfer.Height * sizeof(SV_CONSOLE_CELL);
    if (write ? !MmProbeForRead(transfer.Cells, bytes) : !MmProbeForWrite(transfer.Cells, bytes))
        return STATUS_ACCESS_VIOLATION;
    INT32 left = transfer.Region.Left, top = transfer.Region.Top, right = transfer.Region.Right,
          bottom = transfer.Region.Bottom;
    INT32 buffer_x = transfer.X, buffer_y = transfer.Y;
    if (left < 0) {
        buffer_x -= left;
        left = 0;
    }
    if (top < 0) {
        buffer_y -= top;
        top = 0;
    }
    if (right >= (INT32)screen->Width)
        right = (INT32)screen->Width - 1;
    if (bottom >= (INT32)screen->Height)
        bottom = (INT32)screen->Height - 1;
    if (right - left + 1 > transfer.Width - buffer_x)
        right = left + transfer.Width - buffer_x - 1;
    if (bottom - top + 1 > transfer.Height - buffer_y)
        bottom = top + transfer.Height - buffer_y - 1;
    if (right < left || bottom < top) {
        transfer.Region = (SV_CONSOLE_RECT){0, 0, -1, -1};
    } else {
        SV_CONSOLE_CELL *cells = (SV_CONSOLE_CELL *)transfer.Cells;
        for (INT32 row = top; row <= bottom; row++) {
            SV_CONSOLE_CELL *client =
                cells + (SIZE_T)(buffer_y + row - top) * transfer.Width + buffer_x;
            SV_CONSOLE_CELL *server = screen->Cells + (SIZE_T)row * screen->Width + left;
            memcpy(write ? server : client, write ? client : server,
                   (SIZE_T)(right - left + 1) * sizeof(*cells));
        }
        transfer.Region = (SV_CONSOLE_RECT){(INT16)left, (INT16)top, (INT16)right, (INT16)bottom};
        if (write)
            con_changed(screen->Console);
    }
    memcpy((void *)address, &transfer, sizeof(transfer));
    return STATUS_SUCCESS;
}
static NTSTATUS con_endpoint_control(void *context, UINT32 operation, UINT64 a, UINT64 b, UINT64 c,
                                     UINT64 *result) {
    CON_ENDPOINT *endpoint = context;
    CON_OBJECT *console = endpoint->Console;
    CON_SCREEN *screen = endpoint->Screen;
    CON_ATTACHMENT *attachment = con_current_attachment();
    if (!attachment || attachment->Console != console || console->Closing)
        return STATUS_INVALID_HANDLE;
    if (result)
        *result = 0;
    if (operation == SV_CONSOLE_CONTROL_READ_WIDE || operation == SV_CONSOLE_CONTROL_READ_INPUT ||
        operation == SV_CONSOLE_CONTROL_PEEK_INPUT) {
        if (!endpoint->Input)
            return STATUS_INVALID_HANDLE;
        SIZE_T item_size = operation == SV_CONSOLE_CONTROL_READ_WIDE
                               ? sizeof(WCHAR)
                               : sizeof(SV_CONSOLE_INPUT_RECORD);
        if (b > ((SIZE_T)-1) / item_size || !MmProbeForWrite(a, b * item_size))
            return STATUS_ACCESS_VIOLATION;
        SIZE_T count = 0;
        NTSTATUS status =
            operation == SV_CONSOLE_CONTROL_READ_WIDE
                ? con_read_wide_control(console, (WCHAR *)a, (SIZE_T)b, &count, c)
                : con_read_input_records(console, (SV_CONSOLE_INPUT_RECORD *)a, (SIZE_T)b, &count,
                                         operation == SV_CONSOLE_CONTROL_PEEK_INPUT, TRUE);
        if (result && NT_SUCCESS(status))
            *result = count;
        return status;
    }
    UINT64 flags = KiIrqSave();
    NTSTATUS status = STATUS_SUCCESS;
    switch (operation) {
    case ConsoleControlGetMode:
        if (result)
            *result = endpoint->Input ? console->InputMode : screen->Mode;
        break;
    case ConsoleControlSetMode:
        if (endpoint->Input) {
            if (a & ~CONSOLE_INPUT_FLAGS || ((a & CON_INPUT_ECHO) && !(a & CON_INPUT_LINE)))
                status = STATUS_INVALID_PARAMETER;
            else {
                console->InputMode = (UINT32)a;
                console->LegacyInput = FALSE;
            }
        } else {
            if (a & ~CONSOLE_OUTPUT_FLAGS)
                status = STATUS_INVALID_PARAMETER;
            else
                screen->Mode = (UINT32)a;
        }
        break;
    case ConsoleControlSetInputMode:
        if (!endpoint->Input)
            status = STATUS_INVALID_HANDLE;
        else {
            console->InputMode = (UINT32)a;
            console->LegacyInput = TRUE;
        }
        break;
    case ConsoleControlGetScreenInfo: {
        if (!screen) {
            status = STATUS_INVALID_HANDLE;
            break;
        }
        if (!MmProbeForWrite(a, sizeof(CONSOLE_SCREEN_INFO))) {
            status = STATUS_ACCESS_VIOLATION;
            break;
        }
        CONSOLE_SCREEN_INFO *info = (void *)a;
        *info = (CONSOLE_SCREEN_INFO){
            (INT16)screen->Width,
            (INT16)screen->Height,
            (INT16)screen->CursorX,
            (INT16)screen->CursorY,
            screen->Attribute,
            screen->Window.Left,
            screen->Window.Top,
            screen->Window.Right,
            screen->Window.Bottom,
            (INT16)(GfxAvailable() ? GfxFramebuffer.Width / console->FontWidth : screen->Width),
            (INT16)(GfxAvailable() ? GfxFramebuffer.Height / console->FontHeight : screen->Height)};
        break;
    }
    case ConsoleControlSetAttribute:
        if (!screen)
            status = STATUS_INVALID_HANDLE;
        else {
            screen->Attribute = (UINT16)a;
            con_changed(console);
        }
        break;
    case ConsoleControlSetCursorPosition: {
        UINT32 x = (UINT16)a, y = (UINT16)(a >> 16);
        if (!screen)
            status = STATUS_INVALID_HANDLE;
        else if (x >= screen->Width || y >= screen->Height)
            status = STATUS_INVALID_PARAMETER;
        else {
            screen->CursorX = x;
            screen->CursorY = y;
            screen->WrapPending = FALSE;
            con_screen_follow_cursor(screen);
            con_changed(console);
        }
        break;
    }
    case ConsoleControlGetCursorInfo:
        if (!screen)
            status = STATUS_INVALID_HANDLE;
        else if (!MmProbeForWrite(a, sizeof(CONSOLE_CURSOR_INFO)))
            status = STATUS_ACCESS_VIOLATION;
        else
            *(CONSOLE_CURSOR_INFO *)a =
                (CONSOLE_CURSOR_INFO){screen->CursorSize, screen->CursorVisible};
        break;
    case ConsoleControlSetCursorInfo:
        if (!screen)
            status = STATUS_INVALID_HANDLE;
        else if (!MmProbeForRead(a, sizeof(CONSOLE_CURSOR_INFO)))
            status = STATUS_ACCESS_VIOLATION;
        else {
            CONSOLE_CURSOR_INFO info = *(CONSOLE_CURSOR_INFO *)a;
            if (!info.Size || info.Size > 100)
                status = STATUS_INVALID_PARAMETER;
            else {
                screen->CursorSize = info.Size;
                screen->CursorVisible = info.Visible != 0;
                con_changed(console);
            }
        }
        break;
    case ConsoleControlFillCharacter:
    case ConsoleControlFillAttribute:
    case SV_CONSOLE_CONTROL_FILL_WIDE: {
        UINT32 x = (UINT16)c, y = (UINT16)(c >> 16);
        if (!screen)
            status = STATUS_INVALID_HANDLE;
        else if (x >= screen->Width || y >= screen->Height)
            status = STATUS_INVALID_PARAMETER;
        else {
            SIZE_T first = (SIZE_T)y * screen->Width + x,
                   count = (SIZE_T)screen->Width * screen->Height - first;
            if (count > b)
                count = (SIZE_T)b;
            WCHAR character = operation == SV_CONSOLE_CONTROL_FILL_WIDE
                                  ? (WCHAR)a
                                  : con_decode_byte(console->OutputCP, (UINT8)a);
            con_fill_cells(screen, first, count, character, (UINT16)a,
                           operation != ConsoleControlFillAttribute,
                           operation == ConsoleControlFillAttribute);
            if (result)
                *result = count;
            con_changed(console);
        }
        break;
    }
    case ConsoleControlSetForegroundProcess: {
        if (!a)
            console->ForegroundProcessId = 0;
        else {
            POBJECT object;
            status = ObReferenceObjectByHandle((HANDLE)a, 0, NULL, &object);
            if (NT_SUCCESS(status)) {
                PKPROCESS process = PsGetProcessFromObject(object);
                CON_ATTACHMENT *target = con_attachment(process);
                if (!target || target->Console != console)
                    status = STATUS_INVALID_HANDLE;
                else
                    console->ForegroundProcessId = process->ProcessId;
                ObDereferenceObject(object);
            }
        }
        break;
    }
    case ConsoleControlRedraw:
        con_changed(console);
        break;
    case ConsoleControlStreamControl:
        if (!screen)
            status = STATUS_INVALID_HANDLE;
        else if (a >= 1 && a <= 4) {
            /* Line-editor presentation barriers. The boot renderer mirrors
             * them as COM1 save/restore/erase sequences; conhost already
             * consumes the authoritative screen buffer. Publish it without
             * changing the actual cells, cursor position or colors. */
            con_changed(console);
        } else
            status = STATUS_INVALID_PARAMETER;
        break;
    case SV_CONSOLE_CONTROL_WRITE_WIDE:
        if (!screen)
            status = STATUS_INVALID_HANDLE;
        else if (b > ((SIZE_T)-1) / sizeof(WCHAR) || !MmProbeForRead(a, b * sizeof(WCHAR)))
            status = STATUS_ACCESS_VIOLATION;
        else {
            const WCHAR *text = (void *)a;
            for (SIZE_T i = 0; i < b; i++)
                con_write_character(screen, text[i]);
            con_changed(console);
            if (result)
                *result = b;
        }
        break;
    case SV_CONSOLE_CONTROL_RESIZE_BUFFER:
        status = screen ? con_resize_screen(screen, (UINT16)a, (UINT16)(a >> 16))
                        : STATUS_INVALID_HANDLE;
        break;
    case SV_CONSOLE_CONTROL_SET_WINDOW:
        if (!screen)
            status = STATUS_INVALID_HANDLE;
        else if (!MmProbeForRead(a, sizeof(SV_CONSOLE_RECT)))
            status = STATUS_ACCESS_VIOLATION;
        else
            status = con_set_window(screen, (const SV_CONSOLE_RECT *)a, b != 0);
        break;
    case SV_CONSOLE_CONTROL_FLUSH_INPUT:
        if (!endpoint->Input)
            status = STATUS_INVALID_HANDLE;
        else {
            console->InputHead = console->InputCount = 0;
            console->CookedRead = console->CookedCount = 0;
            console->CookedReady = FALSE;
            console->PendingCount = console->PendingIndex = 0;
            con_sync_input(console);
        }
        break;
    case SV_CONSOLE_CONTROL_COUNT_INPUT:
        if (!endpoint->Input)
            status = STATUS_INVALID_HANDLE;
        else if (result)
            *result = console->InputCount;
        break;
    case SV_CONSOLE_CONTROL_WRITE_INPUT:
        if (!endpoint->Input)
            status = STATUS_INVALID_HANDLE;
        else if (b > ((SIZE_T)-1) / sizeof(SV_CONSOLE_INPUT_RECORD) ||
                 !MmProbeForRead(a, b * sizeof(SV_CONSOLE_INPUT_RECORD)))
            status = STATUS_ACCESS_VIOLATION;
        else {
            for (SIZE_T i = 0; i < b; i++) {
                status = con_enqueue_input(console, (const SV_CONSOLE_INPUT_RECORD *)a + i);
                if (!NT_SUCCESS(status))
                    break;
                if (result)
                    (*result)++;
            }
        }
        break;
    case SV_CONSOLE_CONTROL_READ_OUTPUT:
    case SV_CONSOLE_CONTROL_WRITE_OUTPUT:
        status = screen
                     ? con_transfer_output(screen, a, operation == SV_CONSOLE_CONTROL_WRITE_OUTPUT)
                     : STATUS_INVALID_HANDLE;
        break;
    case SV_CONSOLE_CONTROL_GET_INPUT_CP:
        if (result)
            *result = console->InputCP;
        break;
    case SV_CONSOLE_CONTROL_GET_OUTPUT_CP:
        if (result)
            *result = console->OutputCP;
        break;
    case SV_CONSOLE_CONTROL_SET_INPUT_CP:
    case SV_CONSOLE_CONTROL_SET_OUTPUT_CP:
        if (!con_valid_codepage((UINT32)a))
            status = STATUS_INVALID_PARAMETER;
        else if (operation == SV_CONSOLE_CONTROL_SET_INPUT_CP)
            console->InputCP = (UINT32)a;
        else {
            console->OutputCP = (UINT32)a;
            for (CON_ATTACHMENT *p = console->Attachments; p; p = p->Next)
                (void)p;
            con_changed(console);
        }
        break;
    case SV_CONSOLE_CONTROL_GET_TITLE:
        if (b && !MmProbeForWrite(a, b * sizeof(WCHAR)))
            status = STATUS_ACCESS_VIOLATION;
        else {
            SIZE_T count = console->TitleLength;
            if (!b)
                count = 0;
            else if (count >= b)
                count = (SIZE_T)b - 1;
            if (b) {
                memcpy((void *)a, console->Title, count * sizeof(WCHAR));
                ((WCHAR *)a)[count] = 0;
            }
            if (result)
                *result = count;
        }
        break;
    case SV_CONSOLE_CONTROL_SET_TITLE:
        if (b > 32767 || !MmProbeForRead(a, b * sizeof(WCHAR)))
            status = STATUS_ACCESS_VIOLATION;
        else
            status = con_set_title(console, (const WCHAR *)a, (SIZE_T)b);
        break;
    case SV_CONSOLE_CONTROL_GET_INFO_EX:
    case SV_CONSOLE_CONTROL_SET_INFO_EX: {
        if (!screen) {
            status = STATUS_INVALID_HANDLE;
            break;
        }
        if (!MmProbeForRead(a, sizeof(SV_CONSOLE_SCREEN_INFO_EX)) ||
            (operation == SV_CONSOLE_CONTROL_GET_INFO_EX &&
             !MmProbeForWrite(a, sizeof(SV_CONSOLE_SCREEN_INFO_EX)))) {
            status = STATUS_ACCESS_VIOLATION;
            break;
        }
        SV_CONSOLE_SCREEN_INFO_EX info = *(SV_CONSOLE_SCREEN_INFO_EX *)a;
        if (info.Size != sizeof(info)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (operation == SV_CONSOLE_CONTROL_GET_INFO_EX) {
            info.Width = (INT16)screen->Width;
            info.Height = (INT16)screen->Height;
            info.CursorX = (INT16)screen->CursorX;
            info.CursorY = (INT16)screen->CursorY;
            info.Attribute = screen->Attribute;
            info.PopupAttribute = screen->PopupAttribute;
            info.Window = screen->Window;
            info.MaximumWidth =
                (INT16)(GfxAvailable() ? GfxFramebuffer.Width / console->FontWidth : screen->Width);
            info.MaximumHeight =
                (INT16)(GfxAvailable() ? GfxFramebuffer.Height / console->FontHeight
                                       : screen->Height);
            info.FullscreenSupported = FALSE;
            memcpy(info.ColorTable, screen->Colors, sizeof(info.ColorTable));
            *(SV_CONSOLE_SCREEN_INFO_EX *)a = info;
        } else {
            if (info.Width <= 0 || info.Height <= 0 || info.CursorX < 0 || info.CursorY < 0 ||
                info.CursorX >= info.Width || info.CursorY >= info.Height || info.Window.Left < 0 ||
                info.Window.Top < 0 || info.Window.Right < info.Window.Left ||
                info.Window.Bottom < info.Window.Top || info.Window.Right >= info.Width ||
                info.Window.Bottom >= info.Height ||
                (GfxAvailable() && ((UINT32)(info.Window.Right - info.Window.Left + 1) >
                                        GfxFramebuffer.Width / console->FontWidth ||
                                    (UINT32)(info.Window.Bottom - info.Window.Top + 1) >
                                        GfxFramebuffer.Height / console->FontHeight))) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            SV_CONSOLE_RECT previous_window = screen->Window;
            screen->Window = (SV_CONSOLE_RECT){0, 0, 0, 0};
            status = con_resize_screen(screen, (UINT16)info.Width, (UINT16)info.Height);
            if (!NT_SUCCESS(status))
                screen->Window = previous_window;
            if (NT_SUCCESS(status))
                status = con_set_window(screen, &info.Window, TRUE);
            if (NT_SUCCESS(status)) {
                screen->Attribute = info.Attribute;
                screen->PopupAttribute = info.PopupAttribute;
                screen->CursorX = (UINT16)info.CursorX;
                screen->CursorY = (UINT16)info.CursorY;
                memcpy(screen->Colors, info.ColorTable, sizeof(screen->Colors));
                con_changed(console);
            }
        }
        break;
    }
    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    KiIrqRestore(flags);
    return status;
}

static POBJECT con_reference_process(UINT32 pid) {
    ULONG found = 0;
    POBJECT object = PsReferenceNextProcess(pid ? pid - 1 : 0, &found);
    if (object && found != pid) {
        ObDereferenceObject(object);
        object = NULL;
    }
    return object;
}
static NTSTATUS con_generate_control(CON_OBJECT *console, UINT32 event, UINT32 group,
                                     BOOLEAN keyboard) {
    BOOLEAN force = (event & 0x80000000u) != 0;
    event &= 0x7fffffffu;
    if (event > 2)
        return STATUS_INVALID_PARAMETER;
    UINT64 flags = KiIrqSave();
    SIZE_T count = 0;
    for (CON_ATTACHMENT *p = console->Attachments; p; p = p->Next)
        if (p->Process->ProcessId != console->HostProcessId && (!group || p->Group == group) &&
            !(event == 0 && p->IgnoreCtrlC))
            count++;
    UINT32 *ids =
        count ? ExAllocatePoolWithTag(NonPagedPool, count * sizeof(UINT32), 'CtnC') : NULL;
    if (count && !ids) {
        KiIrqRestore(flags);
        return STATUS_NO_MEMORY;
    }
    SIZE_T used = 0;
    for (CON_ATTACHMENT *p = console->Attachments; p; p = p->Next)
        if (p->Process->ProcessId != console->HostProcessId && (!group || p->Group == group) &&
            !(event == 0 && p->IgnoreCtrlC) &&
            (!keyboard || !console->ForegroundProcessId ||
             p->Process->ProcessId == console->ForegroundProcessId))
            ids[used++] = p->Process->ProcessId;
    KiIrqRestore(flags);
    NTSTATUS result = STATUS_SUCCESS;
    for (SIZE_T i = 0; i < used; i++) {
        POBJECT object = con_reference_process(ids[i]);
        if (!object)
            continue;
        PKPROCESS process = PsGetProcessFromObject(object);
        CON_ATTACHMENT *attachment = con_attachment(process);
        if (!force && attachment && attachment->ControlRoutine) {
            HANDLE thread = NULL;
            NTSTATUS status =
                PsCreateUserThreadForProcess(process, attachment->ControlRoutine, event, &thread);
            if (thread)
                ObCloseHandle(thread);
            if (!NT_SUCCESS(status))
                result = status;
        } else {
            HANDLE handle = NULL;
            NTSTATUS status = ObCreateHandle(object, 1, &handle);
            if (NT_SUCCESS(status)) {
                UINT64 args[2] = {(UINT64)handle, (UINT64)(UINT32)CTRL_C_EXIT_};
                NtTerminateProcess(args);
                ObCloseHandle(handle);
            } else
                result = status;
        }
        ObDereferenceObject(object);
    }
    if (ids)
        ExFreePool(ids);
    return result;
}
static void con_update_process_parameters(PKPROCESS process) {
    PKTHREAD thread = KeGetCurrentThread();
    if (!thread || thread->Process != process)
        return;
    UINT64 params = PROCESS_PARAMS_VA;
    if (MmProbeForWrite(params + 0x10, 0x28)) {
        CON_ATTACHMENT *attachment = con_attachment(process);
        *(UINT64 *)(params + 0x10) = attachment ? attachment->Console->Identity : 0;
        *(HANDLE *)(params + 0x20) = process->StandardInput;
        *(HANDLE *)(params + 0x28) = process->StandardOutput;
        *(HANDLE *)(params + 0x30) = process->StandardError;
    }
}

UINT64 SvNtConsoleManage(UINT64 *a) {
    PKTHREAD caller = KeGetCurrentThread();
    PKPROCESS process = caller ? caller->Process : NULL;
    if (!process)
        return STATUS_INVALID_PARAMETER;
    UINT32 operation = (UINT32)a[0];
    UINT64 *result = (UINT64 *)a[4];
    if (result && !MmProbeForWrite((UINT64)result, sizeof(*result)))
        return STATUS_ACCESS_VIOLATION;
    if (result)
        *result = 0;
    if (operation == SV_CONSOLE_SET_STANDARD_HANDLE) {
        if ((UINT32)a[1] == (UINT32)-10)
            process->StandardInput = (HANDLE)a[2];
        else if ((UINT32)a[1] == (UINT32)-11)
            process->StandardOutput = (HANDLE)a[2];
        else if ((UINT32)a[1] == (UINT32)-12)
            process->StandardError = (HANDLE)a[2];
        else
            return STATUS_INVALID_PARAMETER;
        con_update_process_parameters(process);
        return STATUS_SUCCESS;
    }
    CON_ATTACHMENT *attachment = con_attachment(process);
    CON_OBJECT *console = attachment ? attachment->Console : NULL;
    if (operation == SV_CONSOLE_ALLOCATE) {
        HANDLE original[3] = {process->StandardInput, process->StandardOutput,
                              process->StandardError};
        if (console)
            return STATUS_ACCESS_DENIED;
        CON_OBJECT *created;
        NTSTATUS status = con_create_console(process, &created);
        if (!NT_SUCCESS(status))
            return status;
        created->DisplayAllowed = TRUE;
        status = con_attach_process(process, created, process->ProcessId, TRUE);
        ObDereferenceObject(created);
        if (NT_SUCCESS(status)) {
            con_update_process_parameters(process);
            ConProcessConsolePublished(process);
            status = ConWaitForProcessConsoleHost(process);
            if (!NT_SUCCESS(status)) {
                ConDetachProcessConsole(process);
                process->StandardInput = original[0];
                process->StandardOutput = original[1];
                process->StandardError = original[2];
                con_update_process_parameters(process);
            }
        }
        return status;
    }
    if (operation == SV_CONSOLE_ATTACH) {
        HANDLE original[3] = {process->StandardInput, process->StandardOutput,
                              process->StandardError};
        if (console)
            return STATUS_ACCESS_DENIED;
        UINT32 target_id = (UINT32)a[1] == (UINT32)-1 ? process->ParentProcessId : (UINT32)a[1];
        POBJECT target = con_reference_process(target_id);
        if (!target)
            return STATUS_INVALID_PARAMETER;
        PKPROCESS parent = PsGetProcessFromObject(target);
        CON_ATTACHMENT *source = con_attachment(parent);
        NTSTATUS status;
        if (!source)
            status = STATUS_INVALID_HANDLE;
        else if (source->Console->Station != W32kGetProcessWindowStationIdentity(process))
            status = STATUS_ACCESS_DENIED;
        else {
            ACCESS_MASK granted = 0;
            status = SeAccessCheck(ObHeaderFromObject(source->Console)->SecurityDescriptor,
                                   process->SecurityUserRid, GENERIC_READ | GENERIC_WRITE,
                                   &g_console_type->GenericMapping, &granted);
            if (NT_SUCCESS(status))
                status = con_attach_process(process, source->Console, source->Group, TRUE);
        }
        ObDereferenceObject(target);
        if (NT_SUCCESS(status)) {
            con_update_process_parameters(process);
            status = ConWaitForProcessConsoleHost(process);
            if (!NT_SUCCESS(status)) {
                ConDetachProcessConsole(process);
                process->StandardInput = original[0];
                process->StandardOutput = original[1];
                process->StandardError = original[2];
                con_update_process_parameters(process);
            }
        }
        return status;
    }
    if (!console)
        return STATUS_INVALID_HANDLE;
    if (operation == SV_CONSOLE_FREE) {
        ConDetachProcessConsole(process);
        con_update_process_parameters(process);
        return STATUS_SUCCESS;
    }
    if (operation == SV_CONSOLE_OPEN_INPUT || operation == SV_CONSOLE_OPEN_OUTPUT) {
        HANDLE handle = NULL;
        if (a[2] & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_ALL | 0x00100000u))
            return STATUS_INVALID_PARAMETER;
        NTSTATUS status = con_open_endpoint_access(
            process, console, operation == SV_CONSOLE_OPEN_INPUT ? NULL : console->Active,
            operation == SV_CONSOLE_OPEN_INPUT, (ACCESS_MASK)a[2], TRUE, &handle);
        if (NT_SUCCESS(status) && result)
            *result = (UINT64)handle;
        return status;
    }
    if (operation == SV_CONSOLE_GET_WINDOW) {
        if (result)
            *result = console->HostWindow;
        return STATUS_SUCCESS;
    }
    if (operation == SV_CONSOLE_GET_CONTROL_STATE) {
        if (result)
            *result = attachment->IgnoreCtrlC;
        return STATUS_SUCCESS;
    }
    if (operation == SV_CONSOLE_REGISTER_CONTROL) {
        if (a[1] && (!MmIsUserAddress(a[1]) || !MmProbeForRead(a[1], 1)))
            return STATUS_ACCESS_VIOLATION;
        attachment->ControlRoutine = a[1];
        attachment->IgnoreCtrlC = a[2] != 0;
        return STATUS_SUCCESS;
    }
    if (operation == SV_CONSOLE_GENERATE_CONTROL)
        return a[1] > 1 ? STATUS_INVALID_PARAMETER
                        : con_generate_control(console, (UINT32)a[1], (UINT32)a[2], FALSE);
    if (operation == SV_CONSOLE_GET_PROCESS_LIST) {
        if (a[2] > 0x7fffffff || !MmProbeForWrite(a[1], a[2] * sizeof(UINT32)))
            return STATUS_ACCESS_VIOLATION;
        UINT64 flags = KiIrqSave();
        UINT32 count = 0;
        for (CON_ATTACHMENT *p = console->Attachments; p; p = p->Next) {
            if (p->Process->ProcessId == console->HostProcessId)
                continue;
            if (count < a[2])
                ((UINT32 *)a[1])[count] = p->Process->ProcessId;
            count++;
        }
        if (result)
            *result = count;
        KiIrqRestore(flags);
        return STATUS_SUCCESS;
    }
    if (operation == SV_CONSOLE_CREATE_BUFFER) {
        if (a[1] & ~(GENERIC_READ | GENERIC_WRITE) || a[2] & ~3u)
            return STATUS_INVALID_PARAMETER;
        SE_DESCRIPTOR *descriptor = NULL;
        BOOLEAN inherit = FALSE;
        NTSTATUS status =
            SeCaptureAttributes(a[3], process->SecurityUserRid, &descriptor, &inherit);
        if (!NT_SUCCESS(status))
            return status;
        CON_SCREEN *screen;
        status =
            con_create_screen(console, console->Active->Width, console->Active->Height, &screen);
        if (!NT_SUCCESS(status)) {
            SeDereferenceDescriptor(descriptor);
            return status;
        }
        SeAssignObjectDescriptor(screen, descriptor);
        SeDereferenceDescriptor(descriptor);
        HANDLE handle = NULL;
        status = con_open_endpoint_access(process, console, screen, FALSE, (ACCESS_MASK)a[1], FALSE,
                                          &handle);
        ObDereferenceObject(screen);
        if (NT_SUCCESS(status) && inherit) {
            status = ObSetHandleAttributesInTable(process->HandleTable, handle, 1, 1);
            if (!NT_SUCCESS(status))
                con_close_handle(process, handle);
        }
        if (NT_SUCCESS(status) && result)
            *result = (UINT64)handle;
        return status;
    }
    if (operation == SV_CONSOLE_ACTIVATE_BUFFER) {
        UINT64 flags = KiIrqSave(), tag;
        void *context;
        NTSTATUS status = IoQueryCharDeviceFile((HANDLE)a[1], &tag, &context);
        CON_ENDPOINT *endpoint = context;
        if (!NT_SUCCESS(status) || tag != IO_CHAR_DEVICE_TAG_CONSOLE || endpoint->Input ||
            endpoint->Console != console)
            status = STATUS_INVALID_HANDLE;
        else {
            ObReferenceObject(endpoint->Screen);
            CON_SCREEN *previous = console->Active;
            console->Active = endpoint->Screen;
            ObDereferenceObject(previous);
            con_changed(console);
        }
        KiIrqRestore(flags);
        return status;
    }
    if (operation == SV_CONSOLE_CONNECT_HOST) {
        if (console->HostProcessId != process->ProcessId ||
            !W32kWindowBelongsToProcess(a[1], process))
            return STATUS_ACCESS_DENIED;
        console->HostWindow = a[1];
        if ((UINT16)a[2] && (UINT16)(a[2] >> 16)) {
            console->FontWidth = (UINT16)a[2];
            console->FontHeight = (UINT16)(a[2] >> 16);
        }
        console->HostStatus = STATUS_SUCCESS;
        KeSetEvent(&console->HostReady);
        con_changed(console);
        return STATUS_SUCCESS;
    }
    if (operation == SV_CONSOLE_HOST_CLOSE) {
        if (console->HostProcessId != process->ProcessId)
            return STATUS_ACCESS_DENIED;
        return con_generate_control(console, a[1] ? 0x80000002u : 2u, 0, FALSE);
    }
    return STATUS_INVALID_PARAMETER;
}

UINT64 SvNtConsoleHost(UINT64 *a) {
    CON_ATTACHMENT *attachment = con_current_attachment();
    if (!attachment || attachment->Console->HostProcessId != attachment->Process->ProcessId)
        return STATUS_ACCESS_DENIED;
    CON_OBJECT *console = attachment->Console;
    CON_SCREEN *screen = console->Active;
    if (a[0] == SV_CONSOLE_HOST_STATE) {
        if (!MmProbeForRead(a[1], sizeof(SV_CONSOLE_HOST_SNAPSHOT)))
            return STATUS_ACCESS_VIOLATION;
        SV_CONSOLE_HOST_SNAPSHOT request = *(SV_CONSOLE_HOST_SNAPSHOT *)a[1];
        UINT64 flags = KiIrqSave();
        SIZE_T cols = screen->Window.Right - screen->Window.Left + 1,
               rows = screen->Window.Bottom - screen->Window.Top + 1;
        if (!MmProbeForWrite(request.View, sizeof(SV_CONSOLE_HOST_VIEW))) {
            KiIrqRestore(flags);
            return STATUS_ACCESS_VIOLATION;
        }
        UINT32 count = 0;
        for (CON_ATTACHMENT *p = console->Attachments; p; p = p->Next)
            if (p->Process->ProcessId != console->HostProcessId && !p->Process->Exiting)
                count++;
        SV_CONSOLE_HOST_VIEW view = {.Size = sizeof(view),
                                     .Width = screen->Width,
                                     .Height = screen->Height,
                                     .CursorX = screen->CursorX,
                                     .CursorY = screen->CursorY,
                                     .CursorSize = screen->CursorSize,
                                     .CursorVisible = screen->CursorVisible,
                                     .InputMode = console->InputMode,
                                     .OutputMode = screen->Mode,
                                     .InputCodepage = console->InputCP,
                                     .OutputCodepage = console->OutputCP,
                                     .ProcessCount = count,
                                     .Window = screen->Window,
                                     .Attribute = screen->Attribute,
                                     .Revision = console->Revision,
                                     .TitleLength = (UINT32)console->TitleLength,
                                     .HostAttached = console->HostWindow != 0};
        memcpy(view.ColorTable, screen->Colors, sizeof(view.ColorTable));
        *(SV_CONSOLE_HOST_VIEW *)request.View = view;
        if (request.Cells) {
            if (request.CellCapacity < cols * rows) {
                KiIrqRestore(flags);
                return STATUS_BUFFER_TOO_SMALL;
            }
            if (!MmProbeForWrite(request.Cells, cols * rows * sizeof(SV_CONSOLE_CELL))) {
                KiIrqRestore(flags);
                return STATUS_ACCESS_VIOLATION;
            }
            for (SIZE_T row = 0; row < rows; row++)
                memcpy((SV_CONSOLE_CELL *)request.Cells + row * cols,
                       screen->Cells + (row + screen->Window.Top) * screen->Width +
                           screen->Window.Left,
                       cols * sizeof(SV_CONSOLE_CELL));
        }
        if (request.Title) {
            if (request.TitleCapacity <= console->TitleLength) {
                KiIrqRestore(flags);
                return STATUS_BUFFER_TOO_SMALL;
            }
            if (!MmProbeForWrite(request.Title, (console->TitleLength + 1) * sizeof(WCHAR))) {
                KiIrqRestore(flags);
                return STATUS_ACCESS_VIOLATION;
            }
            memcpy((void *)request.Title, console->Title,
                   (console->TitleLength + 1) * sizeof(WCHAR));
        }
        KiIrqRestore(flags);
        return STATUS_SUCCESS;
    }
    if (a[0] == SV_CONSOLE_HOST_INPUT) {
        if (a[2] > 0x7fffffff || !MmProbeForRead(a[1], a[2] * sizeof(SV_CONSOLE_INPUT_RECORD)))
            return STATUS_ACCESS_VIOLATION;
        for (SIZE_T i = 0; i < a[2]; i++) {
            SV_CONSOLE_INPUT_RECORD record = ((SV_CONSOLE_INPUT_RECORD *)a[1])[i];
            if (record.EventType == 1 && record.Event.Key.KeyDown &&
                record.Event.Key.Character == 3 && (console->InputMode & CON_INPUT_PROCESSED) &&
                !(console->LegacyInput && !console->ForegroundProcessId)) {
                NTSTATUS status = con_generate_control(console, 0, 0, TRUE);
                if (!NT_SUCCESS(status))
                    return status;
                continue;
            }
            if (record.EventType == 2 && !(console->InputMode & 0x10u))
                continue;
            if (record.EventType == 4 && !(console->InputMode & 8u))
                continue;
            UINT64 flags = KiIrqSave();
            NTSTATUS status = con_enqueue_input(console, &record);
            KiIrqRestore(flags);
            if (!NT_SUCCESS(status))
                return status;
        }
        return STATUS_SUCCESS;
    }
    if (a[0] == SV_CONSOLE_HOST_RESIZE) {
        UINT32 cols = (UINT16)a[1], rows = (UINT16)(a[1] >> 16), fw = (UINT16)a[2],
               fh = (UINT16)(a[2] >> 16);
        if (!cols || !rows || !fw || !fh || cols > 32767 || rows > 32767)
            return STATUS_INVALID_PARAMETER;
        UINT64 flags = KiIrqSave();
        if ((console->InputMode & 8u) && console->InputCount == console->InputCapacity) {
            NTSTATUS reserve = con_grow_input(console);
            if (!NT_SUCCESS(reserve)) {
                KiIrqRestore(flags);
                return reserve;
            }
        }
        UINT32 previous_font_width = console->FontWidth, previous_font_height = console->FontHeight;
        console->FontWidth = fw;
        console->FontHeight = fh;
        /* The viewport and backing width change together, preserving all rows. */
        SV_CONSOLE_RECT previous = screen->Window;
        screen->Window = (SV_CONSOLE_RECT){0, 0, 0, 0};
        NTSTATUS status =
            con_resize_screen(screen, cols, screen->Height < rows ? rows : screen->Height);
        if (NT_SUCCESS(status)) {
            UINT32 top = screen->CursorY >= rows ? screen->CursorY - rows + 1 : 0;
            if (top + rows > screen->Height)
                top = screen->Height - rows;
            screen->Window =
                (SV_CONSOLE_RECT){0, (INT16)top, (INT16)(cols - 1), (INT16)(top + rows - 1)};
            SV_CONSOLE_INPUT_RECORD record = {.EventType = 4};
            record.Event.Size.Width = (INT16)cols;
            record.Event.Size.Height = (INT16)screen->Height;
            if (console->InputMode & 8u)
                status = con_enqueue_input(console, &record);
        } else {
            screen->Window = previous;
            console->FontWidth = previous_font_width;
            console->FontHeight = previous_font_height;
        }
        con_changed(console);
        KiIrqRestore(flags);
        return status;
    }
    if (a[0] == SV_CONSOLE_HOST_SCROLL) {
        UINT64 flags = KiIrqSave();
        INT32 rows = screen->Window.Bottom - screen->Window.Top + 1, top = (INT32)a[1];
        if (top < 0)
            top = 0;
        if ((UINT32)(top + rows) > screen->Height)
            top = (INT32)screen->Height - rows;
        screen->Window.Top = (INT16)top;
        screen->Window.Bottom = (INT16)(top + rows - 1);
        con_changed(console);
        KiIrqRestore(flags);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}
