/* Host-only check of console blinking with a deterministic clock. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <svindows/Console.h>
#include <svindows/Ex.h>
#include <svindows/Gfx.h>
#include <svindows/Ke.h>

GFX_FRAMEBUFFER GfxFramebuffer;
static UINT64 ticks;
static BOOLEAN shown;
static UINT32 cursor_x, cursor_y;
static char cells[25][80];
UINT64 KeGetTickCount(void) { return ticks; }
BOOLEAN GfxAvailable(void) { return FALSE; }
PVOID ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T size, ULONG tag)
{
    (void)type; (void)tag;
    return malloc(size);
}
NORETURN void KeBugCheck(UINT32 code, const char *message)
{
    (void)code; (void)message;
    abort();
}
void HalConsoleRenderCell(UINT32 x, UINT32 y, char c, UINT8 attribute)
{
    (void)attribute;
    assert(x < 80 && y < 25);
    cells[y][x] = c;
    if (x == cursor_x && y == cursor_y) shown = FALSE;
}
void HalConsoleSetUserCursor(UINT32 x, UINT32 y, BOOLEAN visible, UINT32 size,
                             UINT8 attribute)
{
    (void)size; (void)attribute;
    cursor_x = x; cursor_y = y; shown = visible;
}
void HalConsolePutUserChar(char c, UINT8 attribute)
{
    (void)attribute;
    assert(cursor_x < 80 && cursor_y < 25);
    cells[cursor_y][cursor_x++] = c;
}

int main(void)
{
    ConInitialize();
    ConWrite("C:\\>", 4);
    assert(shown);
    ticks = 49; ConUpdateCursor(ticks); assert(shown);
    ticks = 50; ConUpdateCursor(ticks); assert(!shown);
    ticks = 100; ConUpdateCursor(ticks); assert(shown);
    ticks = 140; ConWrite("a", 1); assert(shown);
    ticks = 150; ConUpdateCursor(ticks); assert(shown);
    ticks = 190; ConUpdateCursor(ticks); assert(!shown);
    assert(cells[0][4] == 'a');
    CONSOLE_CURSOR_INFO info = {20, 0};
    assert(ConControl(ConsoleControlSetCursorInfo, (UINT64)&info, 0, 0, NULL) == 0);
    ticks = 240; ConUpdateCursor(ticks); assert(!shown);
    info.Visible = 1;
    assert(ConControl(ConsoleControlSetCursorInfo, (UINT64)&info, 0, 0, NULL) == 0);
    assert(shown);
    ticks = 290; ConUpdateCursor(ticks); assert(!shown);
    puts("PASS console blink: 500ms phases, typing reset, explicit hide, text preservation");
    return 0;
}
