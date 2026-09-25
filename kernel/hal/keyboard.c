/*
 * hal/keyboard.c - PS/2 keyboard driver (IRQ1).
 *
 * Reads set-1 scancodes from the 8042 data port on each IRQ1, tracks the shift
 * state, translates make codes to ASCII, and pushes characters into a small
 * ring buffer that KbdReadChar drains.
 */
#include <svindows/input.h>
#include <svindows/hal.h>
#include "ps2.h"

/* US-QWERTY set-1 make-code -> ASCII, unshifted and shifted. Index by the
 * low 7 bits of the scancode; 0 means "no character". */
static const char kbd_map[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,   '*', 0,   ' ',
};

static const char kbd_map_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,   '*', 0,   ' ',
};

#define KBD_RING 64
static volatile char kbd_ring[KBD_RING];
static volatile UINT32 kbd_head, kbd_tail; /* head == tail => empty */

static BOOLEAN kbd_shift;
static BOOLEAN kbd_caps;
static BOOLEAN kbd_extended; /* an 0xE0 prefix is pending */

static void kbd_push(char c)
{
    UINT32 next = (kbd_head + 1) % KBD_RING;
    if (next != kbd_tail) { /* drop on overflow */
        kbd_ring[kbd_head] = c;
        kbd_head = next;
    }
}

static void kbd_push_seq(const char *sequence)
{
    while (*sequence)
        kbd_push(*sequence++);
}

static void kbd_irq(void)
{
    UINT8 sc = __inbyte(PS2_DATA);

    /* 0xE0 introduces the grey navigation keys. Emit the same ANSI escape
     * sequences a serial terminal produces so the console line editor sees
     * arrows identically from both input paths. */
    if (sc == 0xE0) {
        kbd_extended = TRUE;
        return;
    }

    BOOLEAN release = (sc & 0x80) != 0;
    UINT8 code = sc & 0x7F;

    if (kbd_extended) {
        kbd_extended = FALSE;
        if (release)
            return;
        switch (code) {
        case 0x48: kbd_push_seq("\x1b[A"); return; /* Up    */
        case 0x50: kbd_push_seq("\x1b[B"); return; /* Down  */
        case 0x4D: kbd_push_seq("\x1b[C"); return; /* Right */
        case 0x4B: kbd_push_seq("\x1b[D"); return; /* Left  */
        case 0x47: kbd_push_seq("\x1b[H"); return; /* Home  */
        case 0x4F: kbd_push_seq("\x1b[F"); return; /* End   */
        case 0x53: kbd_push_seq("\x1b[3~"); return;/* Delete*/
        default: return;
        }
    }

    /* Shift make/break. */
    if (code == 0x2A || code == 0x36) {
        kbd_shift = !release;
        return;
    }
    if (release)
        return;

    /* Caps lock toggles on make. */
    if (code == 0x3A) {
        kbd_caps = !kbd_caps;
        return;
    }

    char c = kbd_shift ? kbd_map_shift[code] : kbd_map[code];
    if (!c)
        return;

    /* Caps lock affects letters only, and XORs with shift. */
    if (kbd_caps && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if (kbd_caps && c >= 'A' && c <= 'Z' && !kbd_shift)
        c = (char)(c - 'A' + 'a');

    kbd_push(c);
}

void HalInitializeKeyboard(void)
{
    Ps2ControllerInit();

    HalRegisterIrqHandler(1, kbd_irq);
    HalUnmaskIrq(1);
}

BOOLEAN KbdDataAvailable(void)
{
    return (BOOLEAN)(kbd_head != kbd_tail);
}

char KbdReadChar(void)
{
    if (kbd_head == kbd_tail)
        return 0;
    char c = kbd_ring[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_RING;
    return c;
}
