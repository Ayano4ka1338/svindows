/*
 * hal/console.c - early log sinks and user-console rendering backends.
 *
 * Terminal state lives in io/console.c. HAL only touches hardware, keeping
 * kernel diagnostics independent from the process-visible screen buffer.
 */
#include <svindows/hal.h>
#include <svindows/gfx.h>

#define FB_CURSOR_WIDTH   (GFX_FONT_W - 1)
#define FB_CURSOR_X_INSET 0

static UINT8 g_serial_attribute = 0xff;
static UINT32 g_user_column;
static UINT32 g_user_row;

static const UINT32 g_vga_rgb[16] = {
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa,
    0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
    0x555555, 0x5555ff, 0x55ff55, 0x55ffff,
    0xff5555, 0xff55ff, 0xffff55, 0xffffff,
};

static UINT32 fb_color(UINT8 index)
{
    UINT32 rgb = g_vga_rgb[index & 15];
    return GfxColor((UINT8)(rgb >> 16), (UINT8)(rgb >> 8), (UINT8)rgb);
}

void HalConsoleRenderCell(UINT32 x, UINT32 y, char character, UINT8 attribute)
{
    if (!GfxAvailable()) return;
    GfxDrawChar(x * GFX_FONT_W, y * GFX_FONT_H, character,
                fb_color(attribute), fb_color(attribute >> 4));
}

void HalConsoleSetUserCursor(UINT32 x, UINT32 y, BOOLEAN visible, UINT32 size,
                             UINT8 attribute)
{
    g_user_column = x;
    g_user_row = y;
    if (!visible || !GfxAvailable()) return;
    UINT32 height = (GFX_FONT_H * (size ? size : 1) + 99) / 100;
    if (!height) height = 1;
    if (height > GFX_FONT_H) height = GFX_FONT_H;
    /* Small cursor sizes are an underline.  Two pixels match the stroke
     * weight of the antialiased 9x20 font much better than a four-pixel bar. */
    if (size <= 25 && height > 2) height = 2;
    GfxFillRect(x * GFX_FONT_W + FB_CURSOR_X_INSET,
                y * GFX_FONT_H + GFX_FONT_H - height,
                FB_CURSOR_WIDTH, height, fb_color(attribute));
}

static void serial_number(UINT32 number)
{
    if (number >= 100) HalSerialPutChar((char)('0' + number / 100));
    if (number >= 10) HalSerialPutChar((char)('0' + (number / 10) % 10));
    HalSerialPutChar((char)('0' + number % 10));
}

void HalConsolePutUserStreamChar(char character, UINT8 attribute)
{
    if (attribute != g_serial_attribute) {
        UINT32 fg = attribute & 7;
        UINT32 bg = (attribute >> 4) & 7;
        HalSerialPutChar('\x1b'); HalSerialPutChar('[');
        serial_number((attribute & 8) ? 90 + fg : 30 + fg);
        HalSerialPutChar(';');
        serial_number((attribute & 0x80) ? 100 + bg : 40 + bg);
        HalSerialPutChar('m');
        g_serial_attribute = attribute;
    }
    if (character == '\f') {
        const char clear[] = "\x1b[2J\x1b[H";
        for (UINT32 i = 0; clear[i]; i++) HalSerialPutChar(clear[i]);
    } else {
        HalSerialPutChar(character);
    }
}

static void user_scroll(UINT8 attribute)
{
    UINT32 stride = GfxFramebuffer.Pitch / 4;
    UINT32 rows = GfxFramebuffer.Height / GFX_FONT_H;
    for (UINT32 y = GFX_FONT_H; y < GfxFramebuffer.Height; y++) {
        UINT32 *destination = GfxFramebuffer.Base +
                              (y - GFX_FONT_H) * stride;
        UINT32 *source = GfxFramebuffer.Base + y * stride;
        for (UINT32 x = 0; x < GfxFramebuffer.Width; x++)
            destination[x] = source[x];
    }
    GfxFillRect(0, (rows - 1) * GFX_FONT_H, GfxFramebuffer.Width,
                GFX_FONT_H, fb_color(attribute >> 4));
    g_user_row = rows - 1;
}

void HalConsolePutUserChar(char character, UINT8 attribute)
{
    HalConsolePutUserStreamChar(character, attribute);
    if (!GfxAvailable()) return;
    UINT32 columns = GfxFramebuffer.Width / GFX_FONT_W;
    UINT32 rows = GfxFramebuffer.Height / GFX_FONT_H;
    switch (character) {
    case '\r': g_user_column = 0; break;
    case '\n': g_user_column = 0; g_user_row++; break;
    case '\b':
        if (g_user_column) g_user_column--;
        HalConsoleRenderCell(g_user_column, g_user_row, ' ', attribute);
        break;
    case '\t': g_user_column = (g_user_column + 8) & ~7u; break;
    case '\f':
        GfxClear(fb_color(attribute >> 4));
        g_user_column = g_user_row = 0;
        break;
    default:
        if ((UINT8)character >= 32) {
            HalConsoleRenderCell(g_user_column, g_user_row, character,
                                 attribute);
            g_user_column++;
        }
        break;
    }
    if (g_user_column >= columns) { g_user_column = 0; g_user_row++; }
    if (g_user_row >= rows) user_scroll(attribute);
}

void HalConsoleUpdateCursor(UINT64 tick)
{
    (void)tick;
}

void HalInitializeConsole(void)
{
    HalInitializeSerial();
    HalInitializeVga();
}

void HalConsolePutChar(char c)
{
    HalSerialPutChar(c);
    HalVgaPutChar(c);
}

void HalConsoleWrite(const char *s)
{
    while (*s) HalConsolePutChar(*s++);
}
