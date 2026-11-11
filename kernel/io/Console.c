/*
 * io/Console.c - process-visible character console.
 *
 * This is deliberately above HAL: it owns the screen buffer, cursor,
 * attributes, modes and input events.  HAL is only a serial/framebuffer
 * renderer, so KeLog never modifies this state.
 */
#include <svindows/Console.h>
#include <svindows/Ex.h>
#include <svindows/Gfx.h>
#include <svindows/Hal.h>
#include <svindows/Ke.h>
#include <svindows/Ob.h>
#include <svindows/Ps.h>
#include <svindows/Rtl.h>
#include <nt/Ntstatus.h>

#define CONSOLE_INPUT_CAPACITY 256
#define CONSOLE_CURSOR_BLINK_TICKS 50 /* 500 ms at the 100-Hz system clock */

typedef struct _CONSOLE_INPUT_EVENT {
    UINT8 Character;
    UINT8 KeyDown;
    UINT16 VirtualKey;
    UINT32 ControlState;
} CONSOLE_INPUT_EVENT;

typedef struct _CONSOLE_STATE {
    CONSOLE_CELL *Cells;
    UINT32 Width;
    UINT32 Height;
    UINT32 CursorX;
    UINT32 CursorY;
    UINT8 Attribute;
    UINT32 InputMode;
    UINT32 OutputMode;
    UINT32 CursorSize;
    BOOLEAN CursorVisible;
    BOOLEAN CursorBlinkShown;
    UINT64 CursorBlinkTick;
    CONSOLE_INPUT_EVENT Input[CONSOLE_INPUT_CAPACITY];
    volatile UINT32 InputHead;
    volatile UINT32 InputTail;
    POBJECT ForegroundProcess; /* referenced while a console child is active */
} CONSOLE_STATE;

static CONSOLE_STATE g_console;
static KEVENT g_console_input_ready;
static void con_fill(UINT32 first, UINT32 count, UINT8 character,
                     UINT8 attribute, BOOLEAN fill_character,
                     BOOLEAN fill_attribute);
static void con_restart_cursor_blink(void);

/* A single process-visible terminal is shared by native and Linux console
 * handles. Keep the small ANSI parser state across write() boundaries. */
static UINT8 g_ansi_state;
static UINT16 g_ansi_parameters[8];
static UINT8 g_ansi_parameter_count;
static BOOLEAN g_ansi_have_parameter;
static UINT32 g_ansi_saved_x, g_ansi_saved_y;

static void con_ansi_csi(UINT8 final)
{
    UINT32 p[8] = {0};
    UINT32 count = g_ansi_parameter_count;
    if (g_ansi_have_parameter && count < 8) count++;
    for (UINT32 i = 0; i < count; i++) p[i] = g_ansi_parameters[i];
    UINT32 n = p[0] ? p[0] : 1;
    switch (final) {
    case 'A': g_console.CursorY = n > g_console.CursorY ? 0 : g_console.CursorY - n; break;
    case 'B': g_console.CursorY = n + g_console.CursorY >= g_console.Height ? g_console.Height - 1 : g_console.CursorY + n; break;
    case 'C': g_console.CursorX = n + g_console.CursorX >= g_console.Width ? g_console.Width - 1 : g_console.CursorX + n; break;
    case 'D': g_console.CursorX = n > g_console.CursorX ? 0 : g_console.CursorX - n; break;
    case 'H': case 'f':
        g_console.CursorY = p[0] ? p[0] - 1 : 0;
        g_console.CursorX = p[1] ? p[1] - 1 : 0;
        if (g_console.CursorY >= g_console.Height) g_console.CursorY = g_console.Height - 1;
        if (g_console.CursorX >= g_console.Width) g_console.CursorX = g_console.Width - 1;
        break;
    case 'J':
        if (p[0] == 2 || p[0] == 3) con_fill(0, g_console.Width * g_console.Height, ' ', g_console.Attribute, TRUE, TRUE);
        else if (p[0] == 0) con_fill(g_console.CursorY * g_console.Width + g_console.CursorX,
                                      g_console.Width * g_console.Height - (g_console.CursorY * g_console.Width + g_console.CursorX), ' ', g_console.Attribute, TRUE, TRUE);
        else if (p[0] == 1) con_fill(0, g_console.CursorY * g_console.Width + g_console.CursorX + 1, ' ', g_console.Attribute, TRUE, TRUE);
        if (p[0] == 2 || p[0] == 3) g_console.CursorX = g_console.CursorY = 0;
        break;
    case 'K': {
        UINT32 first = g_console.CursorY * g_console.Width;
        if (p[0] == 1) con_fill(first, g_console.CursorX + 1, ' ', g_console.Attribute, TRUE, TRUE);
        else if (p[0] == 2) con_fill(first, g_console.Width, ' ', g_console.Attribute, TRUE, TRUE);
        else con_fill(first + g_console.CursorX, g_console.Width - g_console.CursorX, ' ', g_console.Attribute, TRUE, TRUE);
        break;
    }
    case 'm':
        if (!count || (count == 1 && p[0] == 0)) g_console.Attribute = 0x07;
        for (UINT32 i = 0; i < count; i++) {
            UINT32 v = p[i];
            if (v == 0) g_console.Attribute = 0x07;
            else if (v == 1) g_console.Attribute |= 0x08;
            else if (v == 22) g_console.Attribute &= 0x07;
            else if (v >= 30 && v <= 37) g_console.Attribute = (g_console.Attribute & 0xf0) | (UINT8)(v - 30);
            else if (v >= 90 && v <= 97) g_console.Attribute = (g_console.Attribute & 0xf0) | (UINT8)(v - 90 + 8);
            else if (v >= 40 && v <= 47) g_console.Attribute = (g_console.Attribute & 0x0f) | (UINT8)((v - 40) << 4);
            else if (v >= 100 && v <= 107) g_console.Attribute = (g_console.Attribute & 0x0f) | (UINT8)((v - 100 + 8) << 4);
            else if (v == 39) g_console.Attribute = (g_console.Attribute & 0xf0) | 7;
            else if (v == 49) g_console.Attribute &= 0x0f;
        }
        break;
    case 's': g_ansi_saved_x = g_console.CursorX; g_ansi_saved_y = g_console.CursorY; break;
    case 'u': g_console.CursorX = g_ansi_saved_x; g_console.CursorY = g_ansi_saved_y; break;
    }
    con_restart_cursor_blink();
}

static void con_restore_cursor_cell(void)
{
    if (!g_console.Cells || g_console.CursorX >= g_console.Width ||
        g_console.CursorY >= g_console.Height) return;
    CONSOLE_CELL *cell = &g_console.Cells[
        g_console.CursorY * g_console.Width + g_console.CursorX];
    HalConsoleRenderCell(g_console.CursorX, g_console.CursorY,
                         (char)cell->Character, cell->Attribute);
}

static void con_restart_cursor_blink(void)
{
    g_console.CursorBlinkTick = KeGetTickCount();
    g_console.CursorBlinkShown = g_console.CursorVisible;
    HalConsoleSetUserCursor(g_console.CursorX, g_console.CursorY,
                            g_console.CursorBlinkShown, g_console.CursorSize,
                            g_console.Attribute);
}

static void con_render_all(void)
{
    for (UINT32 y = 0; y < g_console.Height; y++) {
        for (UINT32 x = 0; x < g_console.Width; x++) {
            CONSOLE_CELL *cell = &g_console.Cells[y * g_console.Width + x];
            HalConsoleRenderCell(x, y, (char)cell->Character, cell->Attribute);
        }
    }
}

void ConRedraw(void)
{
    if (!g_console.Cells) return;
    con_render_all();
    con_restart_cursor_blink();
}

static void con_fill(UINT32 first, UINT32 count, UINT8 character,
                     UINT8 attribute, BOOLEAN fill_character,
                     BOOLEAN fill_attribute)
{
    UINT32 total = g_console.Width * g_console.Height;
    if (first >= total) return;
    if (count > total - first) count = total - first;
    for (UINT32 i = 0; i < count; i++) {
        CONSOLE_CELL *cell = &g_console.Cells[first + i];
        if (fill_character) cell->Character = character;
        if (fill_attribute) cell->Attribute = attribute;
        HalConsoleRenderCell((first + i) % g_console.Width,
                             (first + i) / g_console.Width,
                             (char)cell->Character, cell->Attribute);
    }
}

static void con_scroll(void)
{
    UINT32 row = g_console.Width;
    UINT32 total = g_console.Width * g_console.Height;
    for (UINT32 i = row; i < total; i++)
        g_console.Cells[i - row] = g_console.Cells[i];
    for (UINT32 i = total - row; i < total; i++) {
        g_console.Cells[i].Character = ' ';
        g_console.Cells[i].Attribute = g_console.Attribute;
    }
    g_console.CursorY = g_console.Height - 1;
    con_render_all();
}

static void con_put_character(char character, BOOLEAN write_stream)
{
    if (write_stream)
        HalConsolePutUserChar(character, g_console.Attribute);
    if ((g_console.OutputMode & CON_OUTPUT_PROCESSED) != 0) {
        if (character == '\r') {
            g_console.CursorX = 0;
            return;
        }
        if (character == '\n') {
            g_console.CursorX = 0;
            g_console.CursorY++;
            if (g_console.CursorY >= g_console.Height) con_scroll();
            return;
        }
        if (character == '\b') {
            if (g_console.CursorX) g_console.CursorX--;
            UINT32 index = g_console.CursorY * g_console.Width + g_console.CursorX;
            g_console.Cells[index].Character = ' ';
            g_console.Cells[index].Attribute = g_console.Attribute;
            if (!write_stream)
                HalConsoleRenderCell(g_console.CursorX, g_console.CursorY,
                                     ' ', g_console.Attribute);
            return;
        }
        if (character == '\t') {
            UINT32 spaces = 8 - (g_console.CursorX & 7);
            while (spaces--) con_put_character(' ', write_stream);
            return;
        }
        if (character == '\f') {
            con_fill(0, g_console.Width * g_console.Height, ' ',
                     g_console.Attribute, TRUE, TRUE);
            g_console.CursorX = g_console.CursorY = 0;
            return;
        }
        if ((UINT8)character < 32) return;
    }

    UINT32 index = g_console.CursorY * g_console.Width + g_console.CursorX;
    g_console.Cells[index].Character = (UINT8)character;
    g_console.Cells[index].Attribute = g_console.Attribute;
    if (!write_stream)
        HalConsoleRenderCell(g_console.CursorX, g_console.CursorY,
                             character, g_console.Attribute);
    g_console.CursorX++;
    if (g_console.CursorX >= g_console.Width) {
        if (g_console.OutputMode & CON_OUTPUT_WRAP) {
            g_console.CursorX = 0;
            g_console.CursorY++;
            if (g_console.CursorY >= g_console.Height) con_scroll();
        } else {
            g_console.CursorX = g_console.Width - 1;
        }
    }
}

void ConInitialize(void)
{
    KeInitializeEvent(&g_console_input_ready, TRUE, FALSE);
    g_console.Width = GfxAvailable() ? GfxFramebuffer.Width / GFX_FONT_W : 80;
    g_console.Height = GfxAvailable() ? GfxFramebuffer.Height / GFX_FONT_H : 25;
    if (!g_console.Width) g_console.Width = 80;
    if (!g_console.Height) g_console.Height = 25;
    SIZE_T bytes = g_console.Width * g_console.Height * sizeof(CONSOLE_CELL);
    g_console.Cells = ExAllocatePoolWithTag(NonPagedPool, bytes, 'noCC');
    if (!g_console.Cells)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "console screen buffer");
    g_console.Attribute = 0x07;
    g_console.InputMode = CON_INPUT_PROCESSED | CON_INPUT_LINE;
    g_console.OutputMode = CON_OUTPUT_PROCESSED | CON_OUTPUT_WRAP;
    g_console.CursorSize = 20;
    g_console.CursorVisible = TRUE;
    for (UINT32 i = 0; i < g_console.Width * g_console.Height; i++) {
        g_console.Cells[i].Character = ' ';
        g_console.Cells[i].Attribute = g_console.Attribute;
    }
    con_render_all();
}

void ConQueueInputCharacter(char character)
{
    if ((UINT8)character == 3) {
        POBJECT foreground = NULL;
        UINT64 flags = KiIrqSave();
        foreground = g_console.ForegroundProcess;
        if (foreground) g_console.ForegroundProcess = NULL;
        KiIrqRestore(flags);

        if (foreground) {
            PKPROCESS process = PsGetProcessFromObject(foreground);
            PKTHREAD main_thread = NULL;
            BOOLEAN terminated = FALSE;
            if (process) {
                flags = KiIrqSave();
                for (PLIST_ENTRY entry = process->ThreadListHead.Flink;
                     entry != &process->ThreadListHead; entry = entry->Flink) {
                    PKTHREAD thread = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
                    if (thread->ProcessMainThread) {
                        main_thread = thread;
                        break;
                    }
                }
                if (main_thread)
                    terminated = NT_SUCCESS(KeTerminateThreadByPointer(
                        main_thread, STATUS_CONTROL_C_EXIT));
                KiIrqRestore(flags);
            }
            ObDereferenceObject(foreground);
            if (terminated) return;
        }
    }

    UINT64 irq = KiIrqSave();
    UINT32 next = (g_console.InputHead + 1) % CONSOLE_INPUT_CAPACITY;
    if (next != g_console.InputTail) {
        CONSOLE_INPUT_EVENT *event = &g_console.Input[g_console.InputHead];
        event->Character = (UINT8)character;
        event->KeyDown = TRUE;
        event->VirtualKey = (UINT8)character;
        event->ControlState = 0;
        g_console.InputHead = next;
        KeSetEvent(&g_console_input_ready);
    }
    KiIrqRestore(irq);
}

static char con_pop_input(void)
{
    UINT64 irq = KiIrqSave();
    char character = 0;
    if (g_console.InputHead != g_console.InputTail) {
        character = (char)g_console.Input[g_console.InputTail].Character;
        g_console.InputTail = (g_console.InputTail + 1) % CONSOLE_INPUT_CAPACITY;
    }
    if (g_console.InputHead == g_console.InputTail)
        KeResetEvent(&g_console_input_ready);
    KiIrqRestore(irq);
    return character;
}

void ConFlushInput(void)
{
    /* The keyboard worker may enqueue input asynchronously. Disable local
     * interrupts while moving the consumer to the producer's current head so
     * no partially published event can survive the flush. */
    UINT64 irq = KiIrqSave();
    g_console.InputTail = g_console.InputHead;
    KeResetEvent(&g_console_input_ready);
    KiIrqRestore(irq);
}

SIZE_T ConRead(void *buffer, SIZE_T length)
{
    char *output = (char *)buffer;
    SIZE_T count = 0;
    while (count < length) {
        char character;
        while (!(character = con_pop_input()))
            if (!NT_SUCCESS(KeWaitForSingleObject(&g_console_input_ready.Header)))
                return count;
        if ((g_console.InputMode & CON_INPUT_PROCESSED) && character == '\r')
            character = '\n';
        output[count++] = character;
        if (g_console.InputMode & CON_INPUT_ECHO) ConWrite(&character, 1);
        if ((g_console.InputMode & CON_INPUT_LINE) && character == '\n') break;
        if (!(g_console.InputMode & CON_INPUT_LINE) &&
            g_console.InputHead == g_console.InputTail) break;
    }
    return count;
}

SIZE_T ConTryRead(void *buffer, SIZE_T length)
{
    if (!length) return 0;
    char character = con_pop_input();
    if (!character) return 0;
    if ((g_console.InputMode & CON_INPUT_PROCESSED) && character == '\r')
        character = '\n';
    *(char *)buffer = character;
    if (g_console.InputMode & CON_INPUT_ECHO) ConWrite(&character, 1);
    return 1;
}

BOOLEAN ConInputAvailable(void)
{
    return g_console.InputHead != g_console.InputTail;
}

SIZE_T ConWrite(const void *buffer, SIZE_T length)
{
    const char *characters = (const char *)buffer;
    /* Remove the cursor by restoring the actual buffered cell.  Painting a
     * blank here corrupts column zero for the common "\r\n" sequence: after
     * '\r' the cursor sits on top of the first character of the old line. */
    if (length && g_console.CursorX < g_console.Width &&
        g_console.CursorY < g_console.Height) {
        CONSOLE_CELL *cell = &g_console.Cells[
            g_console.CursorY * g_console.Width + g_console.CursorX];
        HalConsoleRenderCell(g_console.CursorX, g_console.CursorY,
                             (char)cell->Character, cell->Attribute);
    }
    for (SIZE_T i = 0; i < length; i++) {
        UINT8 ch = (UINT8)characters[i];
        if (g_ansi_state == 1) {
            g_ansi_state = ch == '[' ? 2 : 0;
            if (ch == 'c') {
                g_console.Attribute = 0x07;
                g_console.CursorX = g_console.CursorY = 0;
                con_fill(0, g_console.Width * g_console.Height, ' ', 0x07, TRUE, TRUE);
            }
            continue;
        }
        if (g_ansi_state == 2) {
            if (ch >= '0' && ch <= '9') {
                UINT32 index = g_ansi_parameter_count;
                if (index < 8) {
                    g_ansi_parameters[index] = (UINT16)(g_ansi_parameters[index] * 10 + ch - '0');
                    if (g_ansi_parameters[index] > 999) g_ansi_parameters[index] = 999;
                    g_ansi_have_parameter = TRUE;
                }
            } else if (ch == ';') {
                if (g_ansi_parameter_count < 7) g_ansi_parameter_count++;
                g_ansi_have_parameter = FALSE;
            } else if (ch >= 0x40 && ch <= 0x7e) {
                con_ansi_csi(ch);
                g_ansi_state = 0;
                g_ansi_parameter_count = 0;
                g_ansi_have_parameter = FALSE;
                memset(g_ansi_parameters, 0, sizeof(g_ansi_parameters));
            } else if (ch == 0x1b) {
                g_ansi_state = 1;
            }
            continue;
        }
        if (ch == 0x1b) {
            g_ansi_state = 1;
            continue;
        }
        con_put_character((char)ch, TRUE);
    }
    con_restart_cursor_blink();
    return length;
}

void ConAppendKernelTextWithAttribute(const char *text, SIZE_T length,
                                      UINT8 attribute)
{
    if (!g_console.Cells) return;

    /* Kernel boot output is appended in several differently coloured pieces.
     * Erase the previous caret before every piece, just as ConWrite does;
     * otherwise every intermediate caret remains painted as a stray underline
     * after the text has moved on. */
    if (length && g_console.CursorX < g_console.Width &&
        g_console.CursorY < g_console.Height) {
        CONSOLE_CELL *cell = &g_console.Cells[
            g_console.CursorY * g_console.Width + g_console.CursorX];
        HalConsoleRenderCell(g_console.CursorX, g_console.CursorY,
                             (char)cell->Character, cell->Attribute);
    }
    UINT8 previous_attribute = g_console.Attribute;
    g_console.Attribute = attribute;
    for (SIZE_T i = 0; i < length; i++) con_put_character(text[i], FALSE);
    g_console.Attribute = previous_attribute;
    con_restart_cursor_blink();
}

void ConAppendKernelText(const char *text, SIZE_T length)
{
    ConAppendKernelTextWithAttribute(text, length, g_console.Attribute);
}

NTSTATUS ConControl(UINT32 operation, UINT64 argument1, UINT64 argument2,
                    UINT64 argument3, UINT64 *result)
{
    NTSTATUS status = STATUS_SUCCESS;
    if (result) *result = 0;
    switch (operation) {
    case ConsoleControlGetMode:
        if (result) *result = g_console.InputMode | g_console.OutputMode;
        break;
    case ConsoleControlSetMode:
        g_console.InputMode = (UINT32)argument1 & 0xffff;
        g_console.OutputMode = (UINT32)argument1 & 0xffff;
        break;
    case ConsoleControlSetInputMode:
        g_console.InputMode = (UINT32)argument1 &
            (CON_INPUT_PROCESSED | CON_INPUT_LINE | CON_INPUT_ECHO);
        break;
    case ConsoleControlRedraw:
        ConRedraw();
        break;
    case ConsoleControlSetForegroundProcess: {
        POBJECT replacement = NULL;
        if (argument1) {
            status = ObReferenceObjectByHandle((HANDLE)(UINT_PTR)argument1,
                                               0, NULL, &replacement);
            if (!NT_SUCCESS(status)) break;
            if (!PsGetProcessFromObject(replacement)) {
                ObDereferenceObject(replacement);
                status = STATUS_INVALID_HANDLE;
                break;
            }
        }
        UINT64 flags = KiIrqSave();
        POBJECT previous = g_console.ForegroundProcess;
        g_console.ForegroundProcess = replacement;
        KiIrqRestore(flags);
        if (previous) ObDereferenceObject(previous);
        break;
    }
    case ConsoleControlGetScreenInfo: {
        CONSOLE_SCREEN_INFO *info = (CONSOLE_SCREEN_INFO *)(UINT_PTR)argument1;
        info->Width = (INT16)g_console.Width;
        info->Height = (INT16)g_console.Height;
        info->CursorX = (INT16)g_console.CursorX;
        info->CursorY = (INT16)g_console.CursorY;
        info->Attribute = g_console.Attribute;
        info->WindowLeft = info->WindowTop = 0;
        info->WindowRight = (INT16)(g_console.Width - 1);
        info->WindowBottom = (INT16)(g_console.Height - 1);
        info->MaximumWidth = info->Width;
        info->MaximumHeight = info->Height;
        break;
    }
    case ConsoleControlSetAttribute:
        con_restore_cursor_cell();
        g_console.Attribute = (UINT8)argument1;
        con_restart_cursor_blink();
        break;
    case ConsoleControlSetCursorPosition: {
        UINT32 x = (UINT16)argument1;
        UINT32 y = (UINT16)(argument1 >> 16);
        if (x >= g_console.Width || y >= g_console.Height) status = STATUS_INVALID_PARAMETER;
        else {
            con_restore_cursor_cell();
            g_console.CursorX = x; g_console.CursorY = y;
            con_restart_cursor_blink();
        }
        break;
    }
    case ConsoleControlStreamControl: {
        /* The serial log is also a live terminal stream. Mirror the shell's
         * line-editor cursor actions there without feeding escape bytes into
         * the native screen buffer. */
        const char *sequence = NULL;
        switch ((UINT32)argument1) {
        case 1: sequence = "\x1b[s"; break; /* save input-line origin */
        case 2: sequence = "\x1b[u"; break; /* restore input-line origin */
        case 3: sequence = "\x1b[J"; break; /* clear stale rendered tail */
        case 4: {
            UINT32 count = (UINT32)argument2;
            if (count) {
                char digits[10];
                UINT32 used = 0;
                do {
                    digits[used++] = (char)('0' + count % 10u);
                    count /= 10u;
                } while (count && used < sizeof(digits));
                HalSerialPutChar('\x1b');
                HalSerialPutChar('[');
                while (used) HalSerialPutChar(digits[--used]);
                HalSerialPutChar('D'); /* move left to the insertion point */
            }
            break;
        }
        default: status = STATUS_INVALID_PARAMETER; break;
        }
        if (sequence)
            while (*sequence) HalSerialPutChar(*sequence++);
        break;
    }
    case ConsoleControlGetCursorInfo: {
        CONSOLE_CURSOR_INFO *info = (CONSOLE_CURSOR_INFO *)(UINT_PTR)argument1;
        info->Size = g_console.CursorSize;
        info->Visible = g_console.CursorVisible;
        break;
    }
    case ConsoleControlSetCursorInfo: {
        CONSOLE_CURSOR_INFO *info = (CONSOLE_CURSOR_INFO *)(UINT_PTR)argument1;
        if (!info->Size || info->Size > 100) status = STATUS_INVALID_PARAMETER;
        else {
            con_restore_cursor_cell();
            g_console.CursorSize = info->Size;
            g_console.CursorVisible = info->Visible != 0;
            con_restart_cursor_blink();
        }
        break;
    }
    case ConsoleControlFillCharacter:
    case ConsoleControlFillAttribute: {
        UINT32 x = (UINT16)argument3;
        UINT32 y = (UINT16)(argument3 >> 16);
        if (x >= g_console.Width || y >= g_console.Height) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        UINT32 total = g_console.Width * g_console.Height;
        UINT32 first = y * g_console.Width + x;
        UINT32 count = (UINT32)argument2;
        if (count > total - first) count = total - first;
        con_fill(first, count, (UINT8)argument1, (UINT8)argument1,
                 operation == ConsoleControlFillCharacter,
                 operation == ConsoleControlFillAttribute);
        if (result) *result = count;
        break;
    }
    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    return status;
}

void ConUpdateCursor(UINT64 tick)
{
    if (!g_console.Cells || !g_console.CursorVisible) return;
    UINT64 phases = (tick - g_console.CursorBlinkTick) / CONSOLE_CURSOR_BLINK_TICKS;
    if (!phases) return;
    g_console.CursorBlinkTick += phases * CONSOLE_CURSOR_BLINK_TICKS;
    if (!(phases & 1)) return;
    con_restore_cursor_cell();
    g_console.CursorBlinkShown = !g_console.CursorBlinkShown;
    HalConsoleSetUserCursor(g_console.CursorX, g_console.CursorY,
                            g_console.CursorBlinkShown, g_console.CursorSize,
                            g_console.Attribute);
}
