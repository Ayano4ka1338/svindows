/*
 * hal/Keyboard.c - PS/2 keyboard driver (IRQ1).
 *
 * IRQ1 captures raw set-1 bytes; a DPC tracks modifier state, translates
 * make codes and fills the character ring that KbdReadChar drains.
 */
#include <svindows/Input.h>
#include <svindows/Hal.h>
#include "Ps2.h"
#include "DeferredInput.h"

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
static volatile UINT32 game_ring[KBD_RING];
static volatile UINT32 game_head, game_tail;
static volatile UINT32 mirror_ring[KBD_RING];
static volatile UINT32 mirror_head, mirror_tail, mirror_users;

static BOOLEAN kbd_left_shift, kbd_right_shift;
static BOOLEAN kbd_caps, kbd_ctrl;
static BOOLEAN kbd_extended; /* an 0xE0 prefix is pending */
static HAL_INPUT_BYTE_QUEUE kbd_bytes;
static KDPC kbd_dpc;

static void kbd_input_push(UINT32 event)
{
    UINT32 next = (game_head + 1) % KBD_RING;
    if (next != game_tail) {
        game_ring[game_head] = event;
        game_head = next;
    }
    if (mirror_users) {
        next = (mirror_head + 1) % KBD_RING;
        if (next != mirror_tail) {
            mirror_ring[mirror_head] = event;
            mirror_head = next;
        }
    }
}

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

static void kbd_decode(UINT8 sc)
{

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
        kbd_input_push((UINT32)code | 0x0200 | (release ? 0x0100 : 0));
        if (code == 0x1D) { kbd_ctrl = !release; return; }
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
    kbd_input_push((UINT32)code | (release ? 0x0100 : 0));

    /* Shift make/break. */
    if (code == 0x2A || code == 0x36) {
        if (code == 0x2A) kbd_left_shift = !release;
        else kbd_right_shift = !release;
        return;
    }
    if (code == 0x1D) { kbd_ctrl = !release; return; }
    if (release)
        return;

    /* Caps lock toggles on make. */
    if (code == 0x3A) {
        kbd_caps = !kbd_caps;
        return;
    }

    BOOLEAN shifted = kbd_left_shift || kbd_right_shift;
    char c = shifted ? kbd_map_shift[code] : kbd_map[code];
    if (!c)
        return;

    /* Caps lock affects letters only, and XORs with shift. */
    if (kbd_caps && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if (kbd_caps && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');

    /* GUI characters share the raw transition stream, retaining make/char/
     * break order. ANSI navigation sequences belong only to console input. */
    char gui = c == '\n' ? '\r' : c;
    if (kbd_ctrl && ((gui >= 'a' && gui <= 'z') || (gui >= 'A' && gui <= 'Z')))
        gui &= 0x1F;
    kbd_input_push(KBD_INPUT_CHARACTER | (UINT8)gui);
    char console_char = c;
    if (kbd_ctrl && ((console_char >= 'a' && console_char <= 'z') ||
                     (console_char >= 'A' && console_char <= 'Z')))
        console_char &= 0x1F;
    kbd_push(console_char);
}

static void kbd_deferred(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2)
{
    (void)dpc; (void)context; (void)arg1; (void)arg2;
    if (kbd_bytes.Lost) {
        kbd_extended = kbd_left_shift = kbd_right_shift = kbd_ctrl = FALSE;
        kbd_bytes.Lost = FALSE;
    }
    UINT8 byte;
    while (HalTakeInputByte(&kbd_bytes, &byte))
        kbd_decode(byte);
}

static void kbd_irq(void)
{
    HalQueueInputByte(&kbd_bytes, __inbyte(PS2_DATA));
    KeInsertQueueDpc(&kbd_dpc, NULL, NULL);
}

void HalInitializeKeyboard(void)
{
    Ps2ControllerInit();
    KeInitializeDpc(&kbd_dpc, kbd_deferred, NULL);

    HalRegisterIrqHandler(1, kbd_irq);
    HalUnmaskIrq(1);
}

BOOLEAN KbdDataAvailable(void)
{
    return (BOOLEAN)(kbd_head != kbd_tail);
}

char KbdReadChar(void)
{
    UINT64 flags = KiIrqSave();
    if (kbd_head == kbd_tail) {
        KiIrqRestore(flags);
        return 0;
    }
    char c = kbd_ring[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_RING;
    KiIrqRestore(flags);
    return c;
}

BOOLEAN KbdReadInputEvent(UINT32 *event)
{
    UINT64 flags = KiIrqSave();
    if (game_head == game_tail) { KiIrqRestore(flags); return FALSE; }
    *event = game_ring[game_tail];
    game_tail = (game_tail + 1) % KBD_RING;
    KiIrqRestore(flags); return TRUE;
}

BOOLEAN KbdInputEventAvailable(void)
{
    UINT64 flags = KiIrqSave();
    BOOLEAN available = game_head != game_tail;
    KiIrqRestore(flags);
    return available;
}

void KbdInputMirrorAcquire(void)
{
    UINT64 flags = KiIrqSave();
    if (!mirror_users) mirror_head = mirror_tail = 0;
    mirror_users++;
    KiIrqRestore(flags);
}

void KbdInputMirrorRelease(void)
{
    UINT64 flags = KiIrqSave();
    if (mirror_users && !--mirror_users) mirror_head = mirror_tail = 0;
    KiIrqRestore(flags);
}

BOOLEAN KbdMirroredInputEventAvailable(void)
{
    UINT64 flags = KiIrqSave();
    BOOLEAN available = mirror_head != mirror_tail;
    KiIrqRestore(flags);
    return available;
}

BOOLEAN KbdReadMirroredInputEvent(UINT32 *event)
{
    UINT64 flags = KiIrqSave();
    if (mirror_head == mirror_tail) {
        KiIrqRestore(flags);
        return FALSE;
    }
    *event = mirror_ring[mirror_tail];
    mirror_tail = (mirror_tail + 1) % KBD_RING;
    KiIrqRestore(flags);
    return TRUE;
}
