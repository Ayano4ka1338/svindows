/*
 * hal/Console.c - early log sinks and user-console rendering backends.
 *
 * Terminal state lives in io/Console.c. HAL only touches hardware, keeping
 * kernel diagnostics independent from the process-visible screen buffer.
 */
#include <svindows/Hal.h>
#include <svindows/Gfx.h>
#include <svindows/Ke.h>

static UINT8 g_serial_attribute = 0xff;
static BOOLEAN g_graphics_mode;
static UINT32 g_framebuffer_users;

void HalConsoleSetGraphicsMode(BOOLEAN enabled)
{
    g_graphics_mode = enabled;
}

void HalConsoleAcquireFramebuffer(void)
{
    UINT64 flags = KiIrqSave();
    g_framebuffer_users++;
    KiIrqRestore(flags);
}

BOOLEAN HalConsoleReleaseFramebuffer(void)
{
    UINT64 flags = KiIrqSave();
    if (g_framebuffer_users) g_framebuffer_users--;
    BOOLEAN show_console = !g_framebuffer_users && !g_graphics_mode;
    KiIrqRestore(flags);
    return show_console;
}

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
    if (g_graphics_mode || g_framebuffer_users || !GfxAvailable()) return;
    GfxDrawChar(x * GFX_FONT_W, y * GFX_FONT_H, character,
                fb_color(attribute), fb_color(attribute >> 4));
}

void HalConsoleSetUserCursor(UINT32 x, UINT32 y, BOOLEAN visible, UINT32 size,
                             UINT8 attribute)
{
    if (g_graphics_mode || g_framebuffer_users || !visible || !GfxAvailable()) return;
    (void)size; /* Keep the public cursor-size value; render a slim text caret. */
    /* The insertion point is the left edge of the next cell. The centered
     * stroke in the '|' glyph made the caret look several pixels to the right
     * of the prompt on real displays, despite the logical console column being
     * correct. */
    GfxFillRect(x * GFX_FONT_W, y * GFX_FONT_H, 1, GFX_FONT_H,
                fb_color(attribute));
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

void HalConsolePutUserChar(char character, UINT8 attribute)
{
    /* Console.c owns the screen buffer and cursor. Mirroring output through a
     * second HAL cursor caused divergence after ANSI movement, tabs and scrolls
     * (visible as prompt/typed-text offset on real framebuffers). */
    HalConsolePutUserStreamChar(character, attribute);
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
    /* GOP/linear graphics mode has no active VGA text console. In particular,
     * scrolling the hidden B8000 aperture performs thousands of legacy device
     * reads while KeLog holds its lock with interrupts masked. Do not access
     * that aperture or the CRTC ports after the framebuffer takes over. */
    if (!GfxAvailable())
        HalVgaPutChar(c);
}

void HalConsoleWrite(const char *s)
{
    while (*s) HalConsolePutChar(*s++);
}
