/*
 * hal/Mouse.c - PS/2 mouse driver (IRQ12).
 *
 * Enables the auxiliary device on the 8042 controller. IRQ12 captures bytes;
 * a DPC decodes 3-byte packets into screen-clamped pointer coordinates and a
 * button bitmask. Consumers poll MouseState (watching MouseState.Seq for
 * motion) to route pointer input.
 */
#include <svindows/Input.h>
#include <svindows/Hal.h>
#include "Ps2.h"
#include "DeferredInput.h"

MOUSE_STATE MouseState;

static INT32 mouse_max_x = 1023;
static INT32 mouse_max_y = 767;

/* Relative-motion ring for the Linux evdev adapter. Each entry is one decoded
 * PS/2 packet: packed button transitions plus signed X and Y deltas. The
 * absolute MOUSE_STATE above keeps serving native consumers; this ring keeps
 * the raw deltas that a REL_X/REL_Y event device must report. */
#define MOUSE_REL_RING 128
typedef struct _MOUSE_REL_EVENT {
    INT16  Dx;
    INT16  Dy;
    UINT8  Buttons;   /* the packet's button bitmask (MOUSE_BUTTON_*) */
} MOUSE_REL_EVENT;
static volatile MOUSE_REL_EVENT mouse_rel[MOUSE_REL_RING];
static volatile UINT32 mouse_rel_head, mouse_rel_tail;

static void mouse_rel_push(INT32 dx, INT32 dy, UINT8 buttons)
{
    UINT32 next = (mouse_rel_head + 1) % MOUSE_REL_RING;
    if (next == mouse_rel_tail)
        return; /* drop on overflow */
    mouse_rel[mouse_rel_head].Dx = (INT16)dx;
    mouse_rel[mouse_rel_head].Dy = (INT16)dy;
    mouse_rel[mouse_rel_head].Buttons = buttons;
    mouse_rel_head = next;
}

/* Packet reassembly. */
static UINT8 pkt[3];
static UINT8 pkt_index;
static HAL_INPUT_BYTE_QUEUE mouse_bytes;
static KDPC mouse_dpc;

void HalMouseSetBounds(INT32 width, INT32 height)
{
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    UINT64 flags = KiIrqSave();
    mouse_max_x = width - 1;
    mouse_max_y = height - 1;
    MouseState.X = width / 2;
    MouseState.Y = height / 2;
    MouseState.Seq++;
    KiIrqRestore(flags);
}

UINT8 HalMouseWarp(INT32 *x, INT32 *y)
{
    UINT64 flags = KiIrqSave();
    if (*x < 0) *x = 0;
    if (*x > mouse_max_x) *x = mouse_max_x;
    if (*y < 0) *y = 0;
    if (*y > mouse_max_y) *y = mouse_max_y;
    MouseState.X = *x;
    MouseState.Y = *y;
    UINT8 buttons = MouseState.Buttons;
    KiIrqRestore(flags);
    return buttons;
}

static void mouse_apply(INT32 dx, INT32 dy, UINT8 buttons)
{
    INT32 x = MouseState.X + dx;
    INT32 y = MouseState.Y - dy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > mouse_max_x) x = mouse_max_x;
    if (y > mouse_max_y) y = mouse_max_y;
    MouseState.X = x;
    MouseState.Y = y;
    MouseState.Buttons = buttons & 0x07u;
    MouseState.Seq++;
    mouse_rel_push(dx, dy, MouseState.Buttons);
}

void HalMouseInjectRelative(INT32 dx, INT32 dy, UINT8 buttons)
{
    UINT64 flags = KiIrqSave();
    /* The shared packet path uses PS/2's positive-up convention. Convert the
       public HID convention (positive-down) before updating state/evdev. */
    mouse_apply(dx, -dy, buttons);
    KiIrqRestore(flags);
}

static void mouse_decode(UINT8 byte)
{

    /* Byte 0 always has bit 3 set; use it to resynchronize if we lose framing. */
    if (pkt_index == 0 && !(byte & 0x08))
        return;

    pkt[pkt_index++] = byte;
    if (pkt_index < 3)
        return;
    pkt_index = 0;

    UINT8 flags = pkt[0];

    /* Discard packets with an overflow bit set (garbage movement). */
    if (flags & 0xC0)
        return;

    /* 9-bit signed deltas: byte 1/2 with the sign bit from the flags. */
    INT32 dx = (INT32)pkt[1] - ((flags & 0x10) ? 256 : 0);
    INT32 dy = (INT32)pkt[2] - ((flags & 0x20) ? 256 : 0);

    /* PS/2 Y is positive-up; the shared HID-style delta is positive-down. */
    mouse_apply(dx, dy, flags & 0x07u);
}

static void mouse_deferred(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2)
{
    (void)dpc; (void)context; (void)arg1; (void)arg2;
    if (mouse_bytes.Lost) {
        pkt_index = 0;
        mouse_bytes.Lost = FALSE;
    }
    UINT8 byte;
    while (HalTakeInputByte(&mouse_bytes, &byte))
        mouse_decode(byte);
}

static void mouse_irq(void)
{
    HalQueueInputByte(&mouse_bytes, __inbyte(PS2_DATA));
    KeInsertQueueDpc(&mouse_dpc, NULL, NULL);
}

void HalInitializeMouse(void)
{
    Ps2ControllerInit();
    KeInitializeDpc(&mouse_dpc, mouse_deferred, NULL);

    /* Set defaults, then enable data reporting so the mouse streams packets. */
    Ps2MouseCommand(0xF6); /* set defaults */
    Ps2MouseCommand(0xF4); /* enable data reporting */

    HalRegisterIrqHandler(12, mouse_irq);
    HalUnmaskIrq(12);
}

BOOLEAN MouseReadRelEvent(INT32 *dx, INT32 *dy, UINT8 *buttons)
{
    UINT64 flags = KiIrqSave();
    if (mouse_rel_head == mouse_rel_tail) {
        KiIrqRestore(flags);
        return FALSE;
    }
    MOUSE_REL_EVENT event = mouse_rel[mouse_rel_tail];
    mouse_rel_tail = (mouse_rel_tail + 1) % MOUSE_REL_RING;
    KiIrqRestore(flags);
    if (dx) *dx = event.Dx;
    if (dy) *dy = event.Dy;
    if (buttons) *buttons = event.Buttons;
    return TRUE;
}
