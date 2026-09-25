/*
 * io/console.c - process-visible character console.
 *
 * This is deliberately above HAL: it owns the screen buffer, cursor,
 * attributes, modes and input events.  HAL is only a serial/framebuffer
 * renderer, so KeLog never modifies this state.
 */
#include <svindows/console.h>
#include <svindows/ex.h>
#include <svindows/gfx.h>
#include <svindows/hal.h>
#include <svindows/ke.h>
#include <svindows/rtl.h>
#include <nt/ntstatus.h>

#define CONSOLE_INPUT_CAPACITY 256

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
    CONSOLE_INPUT_EVENT Input[CONSOLE_INPUT_CAPACITY];
    volatile UINT32 InputHead;
    volatile UINT32 InputTail;
} CONSOLE_STATE;

static CONSOLE_STATE g_console;

static void con_render_all(void)
{
    for (UINT32 y = 0; y < g_console.Height; y++) {
        for (UINT32 x = 0; x < g_console.Width; x++) {
            CONSOLE_CELL *cell = &g_console.Cells[y * g_console.Width + x];
            HalConsoleRenderCell(x, y, (char)cell->Character, cell->Attribute);
        }
    }
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

static void con_put_character(char character)
{
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
            return;
        }
        if (character == '\t') {
            UINT32 spaces = 8 - (g_console.CursorX & 7);
            while (spaces--) con_put_character(' ');
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
    UINT32 next = (g_console.InputHead + 1) % CONSOLE_INPUT_CAPACITY;
    if (next != g_console.InputTail) {
        CONSOLE_INPUT_EVENT *event = &g_console.Input[g_console.InputHead];
        event->Character = (UINT8)character;
        event->KeyDown = TRUE;
        event->VirtualKey = (UINT8)character;
        event->ControlState = 0;
        g_console.InputHead = next;
    }
}

static char con_pop_input(void)
{
    char character = 0;
    if (g_console.InputHead != g_console.InputTail) {
        character = (char)g_console.Input[g_console.InputTail].Character;
        g_console.InputTail = (g_console.InputTail + 1) % CONSOLE_INPUT_CAPACITY;
    }
    return character;
}

SIZE_T ConRead(void *buffer, SIZE_T length)
{
    char *output = (char *)buffer;
    SIZE_T count = 0;
    while (count < length) {
        char character;
        while (!(character = con_pop_input())) KeYield();
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
    for (SIZE_T i = 0; i < length; i++) con_put_character(characters[i]);
    HalConsoleSetUserCursor(g_console.CursorX, g_console.CursorY,
                            g_console.CursorVisible, g_console.CursorSize,
                            g_console.Attribute);
    return length;
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
        g_console.Attribute = (UINT8)argument1;
        break;
    case ConsoleControlSetCursorPosition: {
        UINT32 x = (UINT16)argument1;
        UINT32 y = (UINT16)(argument1 >> 16);
        if (x >= g_console.Width || y >= g_console.Height) status = STATUS_INVALID_PARAMETER;
        else {
            g_console.CursorX = x; g_console.CursorY = y;
            HalConsoleSetUserCursor(x, y, g_console.CursorVisible,
                                    g_console.CursorSize,
                                    g_console.Attribute);
        }
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
            g_console.CursorSize = info->Size;
            g_console.CursorVisible = info->Visible != 0;
            HalConsoleSetUserCursor(g_console.CursorX, g_console.CursorY,
                                    g_console.CursorVisible,
                                    g_console.CursorSize,
                                    g_console.Attribute);
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
    /* The caret is kept visible and is repositioned on every output update.
     * Blinking is deliberately backend-local and can be enabled once the
     * input worker is no longer the display timer. */
    (void)tick;
}
