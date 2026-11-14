/*
 * usb/Xhci.c - polled xHCI host-controller core.
 *
 * This starts the controller and owns the command/event rings. Keeping the
 * first implementation polled avoids depending on MSI/MSI-X before those
 * interrupt paths exist in the kernel.
 */
#include <svindows/Ke.h>
#include <svindows/Hal.h>
#include <svindows/Input.h>
#include <svindows/Mm.h>
#include <svindows/Rtl.h>
#include "../net/Pci.h"

#define XHCI_PCI_CLASS       0x0cu
#define XHCI_PCI_SUBCLASS    0x03u
#define XHCI_PCI_PROGIF      0x30u
#define XHCI_MMIO_BASE       0xffffc00000000000ULL
#define XHCI_MMIO_PAGES      16u
#define XHCI_TRBS_PER_RING   256u
#define XHCI_TRB_TYPE_SHIFT  10u
#define XHCI_TRB_CYCLE       1u
#define XHCI_TRB_TYPE_NOOP   23u
#define XHCI_TRB_TYPE_LINK   6u
#define XHCI_CMD_TIMEOUT     50000000u
#define XHCI_MAX_HID         8u
#define XHCI_DEVICE_CONTEXT_MAX 32u

#define XHCI_USBCMD          0x00u
#define XHCI_USBSTS          0x04u
#define XHCI_PAGESIZE        0x08u
#define XHCI_CRCR            0x18u
#define XHCI_DCBAAP          0x30u
#define XHCI_CONFIG          0x38u

typedef struct _XHCI_TRB {
    UINT64 Parameter;
    UINT32 Status;
    UINT32 Control;
} XHCI_TRB;

typedef struct _XHCI_ERST_ENTRY {
    UINT64 RingSegmentBase;
    UINT32 RingSegmentSize;
    UINT32 Reserved;
} XHCI_ERST_ENTRY;

typedef struct _XHCI_CAPABILITY_REGS {
    volatile UINT8  CapabilityLength;
    volatile UINT8  Reserved;
    volatile UINT16 InterfaceVersion;
    volatile UINT32 StructuralParameters1;
    volatile UINT32 StructuralParameters2;
    volatile UINT32 StructuralParameters3;
    volatile UINT32 CapabilityParameters1;
    volatile UINT32 DoorbellOffset;
    volatile UINT32 RuntimeOffset;
    volatile UINT32 CapabilityParameters2;
} XHCI_CAPABILITY_REGS;

typedef struct _XHCI_CONTROLLER {
    volatile UINT8 *Registers;
    UINT32 OperationalOffset;
    UINT32 DoorbellOffset;
    UINT32 RuntimeOffset;
    UINT32 MaxSlots;
    UINT32 MaxPorts;
    UINT32 MaxInterrupters;
    UINT64 DcbaaPhysical;
    UINT64 CommandRingPhysical;
    UINT64 EventRingPhysical;
    UINT64 ErstPhysical;
    XHCI_TRB *CommandRing;
    XHCI_TRB *EventRing;
    XHCI_ERST_ENTRY *Erst;
    UINT16 CommandProducer;
    UINT16 EventConsumer;
    UINT8 CommandCycle;
    UINT8 EventCycle;
    BOOLEAN Running;
} XHCI_CONTROLLER;

typedef struct _XHCI_HID_DEVICE {
    UINT8 SlotId;
    UINT8 Port;
    UINT8 Speed;
    UINT8 EndpointId;
    UINT8 InterfaceNumber;
    UINT8 Protocol; /* 1 keyboard, 2 mouse */
    UINT16 MaxPacket;
    UINT8 Interval;
    UINT8 ContextEntries;
    UINT64 InputContextPhysical;
    UINT64 OutputContextPhysical;
    UINT64 ControlRingPhysical;
    UINT64 TransferRingPhysical;
    UINT64 BufferPhysical;
    UINT64 ReportPhysical;
    UINT8 *InputContext;
    UINT8 *OutputContext;
    XHCI_TRB *ControlRing;
    XHCI_TRB *TransferRing;
    UINT8 *Buffer;
    UINT8 *Report;
    UINT16 TransferProducer;
    UINT8 TransferCycle;
    UINT16 ControlProducer;
    UINT8 ControlCycle;
    UINT8 PreviousReport[8];
    BOOLEAN Active;
    BOOLEAN TransferPending;
} XHCI_HID_DEVICE;

static XHCI_CONTROLLER g_xhci;
static XHCI_HID_DEVICE g_hid[XHCI_MAX_HID];
static UINT32 g_hid_count;

static BOOLEAN xhci_enumerate_ports(void);
static void xhci_poll_hid(void);

static UINT32 xhci_read32(UINT32 offset)
{
    return *(volatile UINT32 *)(g_xhci.Registers + offset);
}

static void xhci_write32(UINT32 offset, UINT32 value)
{
    *(volatile UINT32 *)(g_xhci.Registers + offset) = value;
}

static void xhci_write64(UINT32 offset, UINT64 value)
{
    xhci_write32(offset, (UINT32)value);
    xhci_write32(offset + 4, (UINT32)(value >> 32));
}

static void *xhci_alloc_dma(SIZE_T pages, UINT64 *physical)
{
    UINT64 pa = MmAllocatePagesBelow4G(pages);
    if (pa == MM_INVALID_PHYS)
        return NULL;
    void *va = MmPhysToVirt(pa);
    memset(va, 0, pages * PAGE_SIZE);
    *physical = pa;
    return va;
}

static BOOLEAN xhci_handoff(UINT32 hccparams1)
{
    UINT32 offset = ((hccparams1 >> 16) & 0xffffu) * 4u;
    if (!offset)
        return TRUE;

    /* Extended capabilities are a linked list in dword units. */
    for (UINT32 steps = 0; offset && steps < 256; ++steps) {
        UINT32 header = xhci_read32(offset);
        UINT8 id = (UINT8)header;
        UINT8 next = (UINT8)(header >> 8);
        if (id == 1) { /* USB Legacy Support capability */
            UINT32 legacy = xhci_read32(offset);
            if (legacy & (1u << 16)) {
                xhci_write32(offset, legacy | (1u << 24));
                UINT32 spin;
                for (spin = 0; spin < XHCI_CMD_TIMEOUT; ++spin) {
                    if (!(xhci_read32(offset) & (1u << 16)))
                        break;
                    __pause();
                }
                if (spin == XHCI_CMD_TIMEOUT) {
                    KeLog("[usb]  firmware did not release xHCI ownership\n");
                    return FALSE;
                }
            }
            /* Disable legacy SMI sources after ownership has transferred. */
            xhci_write32(offset + 4, 0);
            return TRUE;
        }
        if (!next) break;
        offset += (UINT32)next * 4u;
    }
    return TRUE;
}

static BOOLEAN xhci_wait_status(UINT32 mask, BOOLEAN set)
{
    for (UINT32 spin = 0; spin < XHCI_CMD_TIMEOUT; ++spin) {
        BOOLEAN value = (xhci_read32(g_xhci.OperationalOffset + XHCI_USBSTS) &
                         mask) != 0;
        if (value == set) return TRUE;
        __pause();
    }
    return FALSE;
}

static void xhci_event_advance(void)
{
    ++g_xhci.EventConsumer;
    if (g_xhci.EventConsumer == XHCI_TRBS_PER_RING) {
        g_xhci.EventConsumer = 0;
        g_xhci.EventCycle ^= 1u;
    }
    UINT64 dequeue = g_xhci.EventRingPhysical +
                     (UINT64)g_xhci.EventConsumer * sizeof(XHCI_TRB);
    UINT32 interrupter = g_xhci.RuntimeOffset + 0x20u;
    xhci_write64(interrupter + 0x18u, dequeue | (1u << 3));
}

static BOOLEAN xhci_command_param(UINT32 type, UINT64 parameter,
                                  UINT32 status, UINT32 control,
                                  UINT8 *completion_code, UINT8 *slot_id)
{
    if (!g_xhci.Running) return FALSE;
    UINT16 index = g_xhci.CommandProducer;
    XHCI_TRB *trb = &g_xhci.CommandRing[index];
    trb->Parameter = parameter;
    trb->Status = status;
    trb->Control = control | (type << XHCI_TRB_TYPE_SHIFT) |
                   (g_xhci.CommandCycle ? XHCI_TRB_CYCLE : 0);
    __asm__ volatile("mfence" ::: "memory");

    ++g_xhci.CommandProducer;
    if (g_xhci.CommandProducer == XHCI_TRBS_PER_RING - 1) {
        XHCI_TRB *link = &g_xhci.CommandRing[XHCI_TRBS_PER_RING - 1];
        link->Parameter = g_xhci.CommandRingPhysical;
        link->Status = 0;
        link->Control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
                        (1u << 1) |
                        (g_xhci.CommandCycle ? XHCI_TRB_CYCLE : 0);
        g_xhci.CommandProducer = 0;
        g_xhci.CommandCycle ^= 1u;
    }

    *(volatile UINT32 *)(g_xhci.Registers + g_xhci.DoorbellOffset) = 0;
    for (UINT32 spin = 0; spin < XHCI_CMD_TIMEOUT; ++spin) {
        XHCI_TRB *event = &g_xhci.EventRing[g_xhci.EventConsumer];
        __asm__ volatile("lfence" ::: "memory");
        if ((event->Control & 1u) != g_xhci.EventCycle) {
            __pause();
            continue;
        }

        UINT32 event_type = (event->Control >> XHCI_TRB_TYPE_SHIFT) & 0x3fu;
        UINT8 cc = (UINT8)(event->Status >> 24);
        UINT8 sid = (UINT8)(event->Control >> 24);
        UINT64 command_pointer = event->Parameter & ~0xFULL;
        xhci_event_advance();
        if (event_type != 33u) { /* Command Completion Event */
            continue;
        }
        if (completion_code) *completion_code = cc;
        if (slot_id) *slot_id = sid;
        return cc == 1u && command_pointer ==
               (g_xhci.CommandRingPhysical + (UINT64)index * sizeof(XHCI_TRB));
    }
    return FALSE;
}

static BOOLEAN xhci_command(UINT32 type, UINT32 control,
                            UINT8 *completion_code, UINT8 *slot_id)
{
    return xhci_command_param(type, 0, 0, control,
                              completion_code, slot_id);
}

static UINT32 xhci_context_size(void)
{
    return (xhci_read32(0x10u) & (1u << 2)) ? 64u : 32u;
}

static UINT32 *xhci_context_dwords(UINT8 *context, UINT32 context_index)
{
    return (UINT32 *)(context + xhci_context_size() * context_index);
}

static void xhci_ring_write(XHCI_TRB *ring, UINT64 ring_physical,
                            UINT16 *producer, UINT8 *cycle,
                            UINT64 parameter, UINT32 status, UINT32 control)
{
    XHCI_TRB *trb = &ring[*producer];
    trb->Parameter = parameter;
    trb->Status = status;
    trb->Control = control | (*cycle ? XHCI_TRB_CYCLE : 0);
    ++*producer;
    if (*producer == 63) {
        XHCI_TRB *link = &ring[63];
        link->Parameter = ring_physical;
        link->Status = 0;
        link->Control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
                        (1u << 1) | (*cycle ? XHCI_TRB_CYCLE : 0);
        *producer = 0;
        *cycle ^= 1u;
    }
}

static BOOLEAN xhci_wait_transfer(UINT8 slot_id, UINT8 endpoint_id,
                                  UINT8 *completion_code)
{
    for (UINT32 spin = 0; spin < XHCI_CMD_TIMEOUT; ++spin) {
        XHCI_TRB *event = &g_xhci.EventRing[g_xhci.EventConsumer];
        __asm__ volatile("lfence" ::: "memory");
        if ((event->Control & 1u) != g_xhci.EventCycle) {
            __pause();
            continue;
        }
        UINT32 type = (event->Control >> XHCI_TRB_TYPE_SHIFT) & 0x3fu;
        UINT8 cc = (UINT8)(event->Status >> 24);
        UINT8 ep = (UINT8)((event->Control >> 16) & 0x1fu);
        UINT8 sid = (UINT8)(event->Control >> 24);
        xhci_event_advance();
        if (type != 32u || sid != slot_id || ep != endpoint_id)
            continue;
        if (completion_code) *completion_code = cc;
        return cc == 1u;
    }
    return FALSE;
}

static BOOLEAN xhci_control_transfer(XHCI_HID_DEVICE *dev,
                                     UINT8 request_type, UINT8 request,
                                     UINT16 value, UINT16 index,
                                     UINT16 length)
{
    UINT64 setup = (UINT64)request_type | ((UINT64)request << 8) |
                   ((UINT64)value << 16) | ((UINT64)index << 32) |
                   ((UINT64)length << 48);
    UINT32 setup_trt = length ? ((request_type & 0x80u) ? 3u : 2u) : 0u;
    xhci_ring_write(dev->ControlRing, dev->ControlRingPhysical,
                    &dev->ControlProducer, &dev->ControlCycle,
                    setup, 8u,
                    (2u << XHCI_TRB_TYPE_SHIFT) | (1u << 6) | (1u << 4) |
                    (setup_trt << 16));
    if (length) {
        xhci_ring_write(dev->ControlRing, dev->ControlRingPhysical,
                        &dev->ControlProducer, &dev->ControlCycle,
                        dev->BufferPhysical, length,
                        (3u << XHCI_TRB_TYPE_SHIFT) | (1u << 4) |
                        ((request_type & 0x80u) ? (1u << 16) : 0));
    }
    xhci_ring_write(dev->ControlRing, dev->ControlRingPhysical,
                    &dev->ControlProducer, &dev->ControlCycle, 0, 0,
                    (4u << XHCI_TRB_TYPE_SHIFT) | (1u << 5) |
                    ((!length || (request_type & 0x80u)) ? 0 : (1u << 16)));
    __asm__ volatile("mfence" ::: "memory");
    *(volatile UINT32 *)(g_xhci.Registers + g_xhci.DoorbellOffset +
                         (UINT32)dev->SlotId * 4u) = 1u;
    UINT8 cc = 0;
    BOOLEAN ok = xhci_wait_transfer(dev->SlotId, 1, &cc);
    if (!ok) {
        UINT32 *ep0 = xhci_context_dwords(dev->OutputContext, 1);
        KeLog("[usb]  control request %02x/%02x len=%u failed cc=%u slot=%u "
              "USBSTS=%08x EP0=%08x/%08x ER=%08x\n",
              request_type, request, length, cc, dev->SlotId,
              xhci_read32(g_xhci.OperationalOffset + XHCI_USBSTS),
              ep0[0], ep0[2],
              g_xhci.EventRing[g_xhci.EventConsumer].Control);
    }
    return ok;
}

static BOOLEAN xhci_address_device(XHCI_HID_DEVICE *dev, BOOLEAN bsr,
                                   UINT16 ep0_packet)
{
    memset(dev->InputContext, 0, PAGE_SIZE);
    UINT32 *control = xhci_context_dwords(dev->InputContext, 0);
    UINT32 *slot = xhci_context_dwords(dev->InputContext, 1);
    UINT32 *ep0 = xhci_context_dwords(dev->InputContext, 2);
    control[1] = 3u; /* add slot and default control endpoint contexts */
    slot[0] = ((UINT32)dev->Speed << 20) | (1u << 27);
    slot[1] = (UINT32)dev->Port << 16;
    ep0[1] = (3u << 1) | (4u << 3) | ((UINT32)ep0_packet << 16);
    ep0[2] = (UINT32)dev->ControlRingPhysical | 1u;
    ep0[3] = (UINT32)(dev->ControlRingPhysical >> 32);
    ep0[4] = 8u;
    __asm__ volatile("mfence" ::: "memory");
    UINT32 command_control = (11u << XHCI_TRB_TYPE_SHIFT) |
                             ((UINT32)dev->SlotId << 24) |
                             (bsr ? (1u << 9) : 0);
    return xhci_command_param(11u, dev->InputContextPhysical, 0,
                              command_control, NULL, NULL);
}

static BOOLEAN xhci_get_descriptor(XHCI_HID_DEVICE *dev, UINT8 type,
                                   UINT8 descriptor_index, UINT16 length)
{
    return xhci_control_transfer(dev, 0x80, 6,
                                 (UINT16)(((UINT16)type << 8) | descriptor_index),
                                 0, length);
}

static BOOLEAN xhci_configure_interrupt_endpoint(XHCI_HID_DEVICE *dev)
{
    memset(dev->InputContext, 0, PAGE_SIZE);
    UINT32 *control = xhci_context_dwords(dev->InputContext, 0);
    UINT32 *slot = xhci_context_dwords(dev->InputContext, 1);
    UINT32 *ep = xhci_context_dwords(dev->InputContext, dev->EndpointId + 1u);
    UINT32 context_entries = dev->EndpointId > 1 ? dev->EndpointId : 1;
    control[1] = (1u << 0) | (1u << dev->EndpointId);
    memcpy(slot, xhci_context_dwords(dev->OutputContext, 0),
           xhci_context_size());
    slot[0] = (slot[0] & ~(0x1fu << 27)) | (context_entries << 27);
    UINT32 interval = dev->Interval;
    if (dev->Speed == 1 || dev->Speed == 2) {
        UINT32 frames = interval ? interval : 1u;
        interval = 3;
        UINT32 microframes = frames * 8u;
        while ((1u << interval) < microframes && interval < 15u) ++interval;
    } else if (dev->Speed == 3) {
        interval = interval ? interval - 1u : 0;
    } else {
        interval = interval ? interval - 1u : 0;
    }
    if (interval > 15) interval = 15;
    UINT32 ep_type = (dev->EndpointId & 1u) ? 7u : 3u;
    ep[0] = interval << 16;
    ep[1] = (3u << 1) | (ep_type << 3) |
            ((UINT32)dev->MaxPacket << 16);
    ep[2] = (UINT32)dev->TransferRingPhysical | 1u;
    ep[3] = (UINT32)(dev->TransferRingPhysical >> 32);
    ep[4] = dev->MaxPacket;
    __asm__ volatile("mfence" ::: "memory");
    return xhci_command_param(12u, dev->InputContextPhysical, 0,
                (12u << XHCI_TRB_TYPE_SHIFT) | ((UINT32)dev->SlotId << 24),
                NULL, NULL);
}

static UINT8 xhci_key_scancode(UINT8 usage, BOOLEAN *extended)
{
    *extended = FALSE;
    if (usage >= 0x04 && usage <= 0x1d) {
        static const UINT8 letters[26] = {
            0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,
            0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,
            0x16,0x2f,0x11,0x2d,0x15,0x2c
        };
        return letters[usage - 0x04];
    }
    if (usage >= 0x1e && usage <= 0x27) {
        static const UINT8 digits[10] = { 0x02,0x03,0x04,0x05,0x06,
                                          0x07,0x08,0x09,0x0a,0x0b };
        return digits[usage - 0x1e];
    }
    switch (usage) {
    case 0x28: return 0x1c; case 0x29: return 0x01;
    case 0x2a: return 0x0e; case 0x2b: return 0x0f;
    case 0x2c: return 0x39; case 0x2d: return 0x0c;
    case 0x2e: return 0x0d; case 0x2f: return 0x1a;
    case 0x30: return 0x1b; case 0x31: return 0x2b;
    case 0x33: return 0x27; case 0x34: return 0x28;
    case 0x35: return 0x29; case 0x36: return 0x33;
    case 0x37: return 0x34; case 0x38: return 0x35;
    case 0x39: return 0x3a; case 0x3a: return 0x3b;
    case 0x3b: return 0x3c; case 0x3c: return 0x3d;
    case 0x3d: return 0x3e; case 0x3e: return 0x3f;
    case 0x3f: return 0x40; case 0x40: return 0x41;
    case 0x41: return 0x42; case 0x42: return 0x43;
    case 0x43: return 0x44; case 0x44: return 0x57;
    case 0x45: return 0x58; case 0x49: *extended = TRUE; return 0x52;
    case 0x4a: *extended = TRUE; return 0x47;
    case 0x4b: *extended = TRUE; return 0x49;
    case 0x4c: *extended = TRUE; return 0x53;
    case 0x4d: *extended = TRUE; return 0x4f;
    case 0x4e: *extended = TRUE; return 0x51;
    case 0x4f: *extended = TRUE; return 0x4d;
    case 0x50: *extended = TRUE; return 0x4b;
    case 0x51: *extended = TRUE; return 0x50;
    case 0x52: *extended = TRUE; return 0x48;
    default: return 0;
    }
}

static void xhci_emit_key(UINT8 usage, BOOLEAN release)
{
    BOOLEAN extended;
    UINT8 scan = xhci_key_scancode(usage, &extended);
    if (!scan) return;
    if (extended) HalKeyboardInjectSet1Byte(0xe0);
    HalKeyboardInjectSet1Byte((UINT8)(scan | (release ? 0x80u : 0)));
}

static void xhci_keyboard_report(XHCI_HID_DEVICE *dev)
{
    UINT8 *now = dev->Report;
    UINT8 *old = dev->PreviousReport;
    static const UINT8 mod_scan[8] = { 0x1d,0x2a,0x38,0x5b,
                                       0x1d,0x36,0x38,0x5c };
    for (UINT8 bit = 0; bit < 8; ++bit) {
        BOOLEAN before = (old[0] & (1u << bit)) != 0;
        BOOLEAN after = (now[0] & (1u << bit)) != 0;
        if (before == after) continue;
        if (bit == 3 || bit == 7) continue; /* GUI keys are not translated yet */
        if (bit >= 4) HalKeyboardInjectSet1Byte(0xe0);
        HalKeyboardInjectSet1Byte((UINT8)(mod_scan[bit] |
                                  (after ? 0 : 0x80u)));
    }
    BOOLEAN rollover = FALSE;
    for (UINT8 i = 2; i < 8; ++i)
        if (now[i] == 1 || now[i] == 2 || now[i] == 3) rollover = TRUE;
    if (!rollover) {
        for (UINT8 i = 2; i < 8; ++i) {
            UINT8 key = old[i];
            if (!key || key < 4) continue;
            BOOLEAN found = FALSE;
            for (UINT8 j = 2; j < 8; ++j) if (now[j] == key) found = TRUE;
            if (!found) xhci_emit_key(key, TRUE);
        }
        for (UINT8 i = 2; i < 8; ++i) {
            UINT8 key = now[i];
            if (!key || key < 4) continue;
            BOOLEAN found = FALSE;
            for (UINT8 j = 2; j < 8; ++j) if (old[j] == key) found = TRUE;
            if (!found) xhci_emit_key(key, FALSE);
        }
    }
    memcpy(old, now, 8);
}

static void xhci_mouse_report(XHCI_HID_DEVICE *dev)
{
    UINT8 buttons = dev->Report[0] & 7u;
    INT32 dx = (INT8)dev->Report[1];
    INT32 dy = (INT8)dev->Report[2];
    HalMouseInjectRelative(dx, dy, buttons);
}

static void xhci_queue_interrupt(XHCI_HID_DEVICE *dev)
{
    xhci_ring_write(dev->TransferRing, dev->TransferRingPhysical,
                    &dev->TransferProducer, &dev->TransferCycle,
                    dev->ReportPhysical, dev->MaxPacket,
                    (1u << XHCI_TRB_TYPE_SHIFT) | (1u << 5));
    __asm__ volatile("mfence" ::: "memory");
    *(volatile UINT32 *)(g_xhci.Registers + g_xhci.DoorbellOffset +
                         (UINT32)dev->SlotId * 4u) = dev->EndpointId;
    dev->TransferPending = TRUE;
}

static BOOLEAN xhci_setup_hid(XHCI_HID_DEVICE *dev)
{
    UINT64 in_pa = 0, out_pa = 0, control_pa = 0, ring_pa = 0;
    UINT64 buffer_pa = 0, report_pa = 0;
    dev->InputContext = xhci_alloc_dma(1, &in_pa);
    dev->OutputContext = xhci_alloc_dma(1, &out_pa);
    dev->ControlRing = xhci_alloc_dma(1, &control_pa);
    dev->TransferRing = xhci_alloc_dma(1, &ring_pa);
    dev->Buffer = xhci_alloc_dma(1, &buffer_pa);
    dev->Report = xhci_alloc_dma(1, &report_pa);
    if (!dev->InputContext || !dev->OutputContext || !dev->ControlRing ||
        !dev->TransferRing ||
        !dev->Buffer || !dev->Report) {
        KeLog("[usb]  HID port %u: DMA allocation failed\n", dev->Port);
        return FALSE;
    }
    dev->InputContextPhysical = in_pa;
    dev->OutputContextPhysical = out_pa;
    dev->ControlRingPhysical = control_pa;
    dev->TransferRingPhysical = ring_pa;
    dev->BufferPhysical = buffer_pa;
    dev->ReportPhysical = report_pa;
    dev->TransferCycle = dev->ControlCycle = 1;
    dev->ContextEntries = 1;
    dev->ControlRing[63].Parameter = control_pa;
    dev->ControlRing[63].Control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | (1u << 1) | 1u;
    dev->TransferRing[63].Parameter = ring_pa;
    dev->TransferRing[63].Control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | (1u << 1) | 1u;

    UINT8 completion = 0, slot_id = 0;
    if (!xhci_command(XHCI_TRB_TYPE_NOOP, 0, NULL, NULL)) {
        KeLog("[usb]  HID port %u: command ring stopped responding\n", dev->Port);
        return FALSE;
    }
    /* Enable Slot command (TRB type 9). */
    if (!xhci_command_param(9u, 0, 0,
             (9u << XHCI_TRB_TYPE_SHIFT), &completion, &slot_id) || !slot_id)
        { KeLog("[usb]  HID port %u: Enable Slot failed cc=%u\n",
                dev->Port, completion); return FALSE; }
    dev->SlotId = slot_id;
    ((UINT64 *)MmPhysToVirt(g_xhci.DcbaaPhysical))[slot_id] = out_pa;

    UINT32 portsc = xhci_read32(g_xhci.OperationalOffset + 0x400u +
                                ((UINT32)dev->Port - 1u) * 0x10u);
    dev->Speed = (UINT8)((portsc >> 10) & 0xfu);
    UINT16 packet = dev->Speed == 1 ? 8 : dev->Speed == 2 ? 8 :
                    dev->Speed == 3 ? 64 : 512;
    if (!xhci_address_device(dev, TRUE, packet)) {
        KeLog("[usb]  HID port %u: initial Address Device failed\n", dev->Port);
        return FALSE;
    }
    memset(dev->Buffer, 0, PAGE_SIZE);
    if (!xhci_get_descriptor(dev, 1, 0, 18)) {
        KeLog("[usb]  HID port %u: device descriptor transfer failed\n", dev->Port);
        return FALSE;
    }
    if (dev->Buffer[0] < 18) {
        KeLog("[usb]  HID port %u: invalid device descriptor\n", dev->Port);
        return FALSE;
    }
    UINT16 max_packet = dev->Buffer[7];
    if (dev->Speed >= 4) max_packet = (UINT16)(1u << (dev->Buffer[7] & 0xfu));
    if (max_packet != packet) {
        UINT32 *ctrl = xhci_context_dwords(dev->InputContext, 0);
        UINT32 *slot = xhci_context_dwords(dev->InputContext, 1);
        UINT32 *ep0 = xhci_context_dwords(dev->InputContext, 2);
        ctrl[1] = 1u << 1;
        memcpy(slot, xhci_context_dwords(dev->OutputContext, 0),
               xhci_context_size());
        ep0[1] = (3u << 1) | (4u << 3) | ((UINT32)max_packet << 16);
        ep0[2] = (UINT32)dev->ControlRingPhysical | 1u;
        ep0[3] = (UINT32)(dev->ControlRingPhysical >> 32);
        ep0[4] = 8u;
        if (!xhci_command_param(13u, dev->InputContextPhysical, 0,
             (13u << XHCI_TRB_TYPE_SHIFT) | ((UINT32)dev->SlotId << 24),
             NULL, NULL)) {
            KeLog("[usb]  HID port %u: Evaluate Context failed\n", dev->Port);
            return FALSE;
        }
    }
    if (!xhci_address_device(dev, FALSE, max_packet)) {
        KeLog("[usb]  HID port %u: USB address assignment failed\n", dev->Port);
        return FALSE;
    }

    memset(dev->Buffer, 0, PAGE_SIZE);
    if (!xhci_get_descriptor(dev, 2, 0, 9)) {
        KeLog("[usb]  HID port %u: configuration header failed\n", dev->Port);
        return FALSE;
    }
    UINT16 total_length = (UINT16)(dev->Buffer[2] | (dev->Buffer[3] << 8));
    if (total_length < 9 || total_length > 512) {
        KeLog("[usb]  HID port %u: invalid configuration length %u\n",
              dev->Port, total_length);
        return FALSE;
    }
    memset(dev->Buffer, 0, PAGE_SIZE);
    if (!xhci_get_descriptor(dev, 2, 0, total_length)) {
        KeLog("[usb]  HID port %u: configuration descriptor failed\n", dev->Port);
        return FALSE;
    }
    UINT8 configuration_value = 1;
    UINT8 endpoint_address = 0;
    UINT8 endpoint_interval = 1;
    UINT16 endpoint_packet = 0;
    UINT8 interface_number = 0, protocol = 0;
    BOOLEAN found = FALSE;
    for (UINT32 at = 0; at + 2 <= total_length;) {
        UINT8 len = dev->Buffer[at], type = dev->Buffer[at + 1];
        if (len < 2 || at + len > total_length) break;
        if (type == 2 && len >= 9) configuration_value = dev->Buffer[at + 5];
        if (type == 4 && len >= 9 && dev->Buffer[at + 5] == 3 &&
            dev->Buffer[at + 6] == 1 &&
            (dev->Buffer[at + 7] == 1 || dev->Buffer[at + 7] == 2)) {
            interface_number = dev->Buffer[at + 2];
            protocol = dev->Buffer[at + 7];
            found = TRUE;
        } else if (type == 5 && len >= 7 && found &&
                   (dev->Buffer[at + 2] & 0x80u) &&
                   (dev->Buffer[at + 3] & 3u) == 3u) {
            endpoint_address = dev->Buffer[at + 2];
            endpoint_packet = (UINT16)(dev->Buffer[at + 4] |
                                       (dev->Buffer[at + 5] << 8));
            endpoint_interval = dev->Buffer[at + 6];
            break;
        } else if (type == 4) {
            found = FALSE;
        }
        at += len;
    }
    if (!endpoint_address || !endpoint_packet || !protocol) {
        KeLog("[usb]  port %u device has no HID boot keyboard/mouse interface\n",
              dev->Port);
        return FALSE;
    }

    dev->InterfaceNumber = interface_number;
    dev->Protocol = protocol;
    dev->EndpointId = (UINT8)(((endpoint_address & 0xfu) * 2u) + 1u);
    dev->MaxPacket = endpoint_packet & 0x7ffu;
    if (dev->MaxPacket > 64) dev->MaxPacket = 64;
    dev->Interval = endpoint_interval;
    if (dev->EndpointId + 1u > dev->ContextEntries)
        dev->ContextEntries = dev->EndpointId + 1u;

    if (!xhci_control_transfer(dev, 0, 9, configuration_value, 0, 0))
        { KeLog("[usb]  HID port %u: SET_CONFIGURATION failed\n", dev->Port);
          return FALSE; }
    if (!xhci_control_transfer(dev, 0x21, 0x0b, 0, interface_number, 0)) {
        KeLog("[usb]  HID port %u: SET_PROTOCOL boot failed\n", dev->Port);
        return FALSE;
    }
    (void)xhci_control_transfer(dev, 0x21, 0x0a, 0, interface_number, 0);
    if (!xhci_configure_interrupt_endpoint(dev)) {
        KeLog("[usb]  HID port %u: Configure Endpoint failed\n", dev->Port);
        return FALSE;
    }
    dev->Active = TRUE;
    KeLog("[usb]  HID %s attached on xHCI port %u, slot %u, endpoint %u\n",
          protocol == 1 ? "keyboard" : "mouse", dev->Port, dev->SlotId,
          dev->EndpointId);
    xhci_queue_interrupt(dev);
    return TRUE;
}

static BOOLEAN xhci_enumerate_ports(void)
{
    g_hid_count = 0;
    for (UINT32 port = 1; port <= g_xhci.MaxPorts &&
         g_hid_count < XHCI_MAX_HID; ++port) {
        UINT32 portoff = g_xhci.OperationalOffset + 0x400u +
                         (port - 1u) * 0x10u;
        UINT32 portsc = xhci_read32(portoff);
        if (xhci_read32(0x10u) & (1u << 3)) {
            portsc |= 1u << 9;
            xhci_write32(portoff, portsc); /* xHC controls root port power */
        }
        if (!(portsc & 1u)) continue;
        UINT8 speed = (UINT8)((portsc >> 10) & 0xfu);
        if (speed >= 4) {
            xhci_write32(portoff, portsc | (1u << 31)); /* warm reset */
        } else {
            xhci_write32(portoff, portsc | (1u << 4)); /* port reset */
        }
        BOOLEAN reset_done = FALSE;
        for (UINT32 spin = 0; spin < XHCI_CMD_TIMEOUT / 100u; ++spin) {
            portsc = xhci_read32(portoff);
            if ((portsc & 1u) && !(portsc & ((1u << 4) | (1u << 31)))) {
                reset_done = TRUE;
                break;
            }
            __pause();
        }
        if (!reset_done) {
            KeLog("[usb]  xHCI port %u reset timed out\n", port);
            continue;
        }
        portsc = xhci_read32(portoff);
        xhci_write32(portoff, (portsc & 0x0e00c200u) |
                     (portsc & 0x00fe0000u));
        XHCI_HID_DEVICE *dev = &g_hid[g_hid_count];
        memset(dev, 0, sizeof(*dev));
        dev->Port = (UINT8)port;
        if (xhci_setup_hid(dev)) ++g_hid_count;
        else KeLog("[usb]  device on port %u is not a supported boot HID\n", port);
    }
    return TRUE;
}

static void xhci_poll_hid(void)
{
    for (UINT32 i = 0; i < g_hid_count; ++i) {
        XHCI_HID_DEVICE *dev = &g_hid[i];
        if (!dev->Active || !dev->TransferPending) continue;
        XHCI_TRB *event = &g_xhci.EventRing[g_xhci.EventConsumer];
        __asm__ volatile("lfence" ::: "memory");
        if ((event->Control & 1u) != g_xhci.EventCycle) continue;
        UINT32 type = (event->Control >> XHCI_TRB_TYPE_SHIFT) & 0x3fu;
        UINT8 ep = (UINT8)((event->Control >> 16) & 0x1fu);
        UINT8 sid = (UINT8)(event->Control >> 24);
        if (type != 32u || sid != dev->SlotId || ep != dev->EndpointId)
            continue;
        UINT8 completion = (UINT8)(event->Status >> 24);
        xhci_event_advance();
        dev->TransferPending = FALSE;
        if (completion == 1u) {
            if (dev->Protocol == 1) xhci_keyboard_report(dev);
            else xhci_mouse_report(dev);
        }
        memset(dev->Report, 0, PAGE_SIZE);
        xhci_queue_interrupt(dev);
    }
}

static BOOLEAN xhci_start(PCI_DEVICE *pci)
{
    UINT32 bar0 = pci->Bar[0];
    if ((bar0 & 1u) || (bar0 & 0xfffffff0u) == 0)
        return FALSE;

    UINT64 physical = bar0 & 0xfffffff0u;
    if (((bar0 >> 1) & 3u) == 2u) {
        physical |= (UINT64)pci->Bar[1] << 32;
    }
    if (physical >> 52) return FALSE;

    UINT32 command = PciReadConfig32(pci->Bus, pci->Device, pci->Function, 4);
    PciWriteConfig32(pci->Bus, pci->Device, pci->Function, 4,
                     (command & 0xffffu) | (1u << 1) | (1u << 2));

    UINT64 page = physical & ~(PAGE_SIZE - 1u);
    UINT64 offset = physical & (PAGE_SIZE - 1u);
    for (UINT32 i = 0; i < XHCI_MMIO_PAGES; ++i) {
        if (!MmMapPage(XHCI_MMIO_BASE + (UINT64)i * PAGE_SIZE,
                       page + (UINT64)i * PAGE_SIZE,
                       PTE_WRITE | PTE_PCD | PTE_PWT)) {
            KeLog("[usb]  cannot map xHCI MMIO at 0x%lx\n",
                  (unsigned long)physical);
            return FALSE;
        }
    }
    g_xhci.Registers = (volatile UINT8 *)(ULONG_PTR)(XHCI_MMIO_BASE + offset);

    volatile XHCI_CAPABILITY_REGS *cap =
        (volatile XHCI_CAPABILITY_REGS *)g_xhci.Registers;
    UINT8 cap_length = cap->CapabilityLength;
    UINT32 hcs1 = cap->StructuralParameters1;
    UINT32 hcs2 = cap->StructuralParameters2;
    UINT32 hcc1 = cap->CapabilityParameters1;
    g_xhci.OperationalOffset = cap_length;
    g_xhci.DoorbellOffset = cap->DoorbellOffset & ~3u;
    g_xhci.RuntimeOffset = cap->RuntimeOffset & ~0x1fu;
    g_xhci.MaxSlots = hcs1 & 0xffu;
    g_xhci.MaxInterrupters = (hcs1 >> 8) & 0x7ffu;
    g_xhci.MaxPorts = hcs1 >> 24;

    if (cap_length < sizeof(*cap) || !g_xhci.MaxSlots ||
        !g_xhci.MaxPorts || !g_xhci.DoorbellOffset ||
        !g_xhci.RuntimeOffset || (xhci_read32(g_xhci.OperationalOffset +
          XHCI_PAGESIZE) & 1u) == 0) {
        KeLog("[usb]  xHCI reports unsupported capability registers\n");
        return FALSE;
    }

    if (!xhci_handoff(hcc1)) return FALSE;

    UINT32 usbcmd = xhci_read32(g_xhci.OperationalOffset + XHCI_USBCMD);
    if (usbcmd & 1u) {
        xhci_write32(g_xhci.OperationalOffset + XHCI_USBCMD,
                     usbcmd & ~1u);
    }
    if (!xhci_wait_status(1u, TRUE)) {
        KeLog("[usb]  xHCI did not halt\n");
        return FALSE;
    }

    xhci_write32(g_xhci.OperationalOffset + XHCI_USBCMD,
                 xhci_read32(g_xhci.OperationalOffset + XHCI_USBCMD) |
                 (1u << 1));
    for (UINT32 spin = 0; spin < XHCI_CMD_TIMEOUT; ++spin) {
        if (!(xhci_read32(g_xhci.OperationalOffset + XHCI_USBCMD) & (1u << 1)))
            break;
        __pause();
        if (spin == XHCI_CMD_TIMEOUT - 1) {
            KeLog("[usb]  xHCI reset timed out\n");
            return FALSE;
        }
    }
    if (!xhci_wait_status(1u << 11, FALSE)) {
        KeLog("[usb]  xHCI stayed Not Ready after reset\n");
        return FALSE;
    }

    UINT32 page_size = xhci_read32(g_xhci.OperationalOffset + XHCI_PAGESIZE);
    UINT32 page_bit = 0;
    while (page_bit < 16 && !(page_size & (1u << page_bit))) ++page_bit;
    if (page_bit != 0) {
        KeLog("[usb]  xHCI page size is unsupported\n");
        return FALSE;
    }

    UINT64 dcbaa_pa = 0, command_pa = 0, event_pa = 0, erst_pa = 0;
    UINT64 dcbaa_pages = ((UINT64)g_xhci.MaxSlots + 1u) * sizeof(UINT64);
    dcbaa_pages = (dcbaa_pages + PAGE_SIZE - 1u) / PAGE_SIZE;
    UINT64 *dcbaa = xhci_alloc_dma((SIZE_T)dcbaa_pages, &dcbaa_pa);
    g_xhci.CommandRing = xhci_alloc_dma(1, &command_pa);
    g_xhci.EventRing = xhci_alloc_dma(1, &event_pa);
    g_xhci.Erst = xhci_alloc_dma(1, &erst_pa);
    if (!dcbaa || !g_xhci.CommandRing || !g_xhci.EventRing || !g_xhci.Erst) {
        KeLog("[usb]  cannot allocate xHCI DMA rings\n");
        return FALSE;
    }
    UINT32 scratchpads = (((hcs2 >> 27) & 0x1fu) << 5) |
                         ((hcs2 >> 21) & 0x1fu);
    if (scratchpads) {
        UINT64 array_pa = 0, scratch_pa = 0;
        SIZE_T array_pages =
            ((SIZE_T)scratchpads * sizeof(UINT64) + PAGE_SIZE - 1u) / PAGE_SIZE;
        UINT64 *array = xhci_alloc_dma(array_pages, &array_pa);
        UINT8 *scratch = xhci_alloc_dma(scratchpads, &scratch_pa);
        if (!array || !scratch) {
            KeLog("[usb]  cannot allocate %u xHCI scratchpad pages\n",
                  scratchpads);
            return FALSE;
        }
        for (UINT32 i = 0; i < scratchpads; ++i)
            array[i] = scratch_pa + (UINT64)i * PAGE_SIZE;
        dcbaa[0] = array_pa;
    }
    g_xhci.DcbaaPhysical = dcbaa_pa;
    g_xhci.CommandRingPhysical = command_pa;
    g_xhci.EventRingPhysical = event_pa;
    g_xhci.ErstPhysical = erst_pa;
    g_xhci.CommandCycle = g_xhci.EventCycle = 1;
    g_xhci.CommandProducer = g_xhci.EventConsumer = 0;

    XHCI_TRB *link = &g_xhci.CommandRing[XHCI_TRBS_PER_RING - 1];
    link->Parameter = command_pa;
    link->Control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
                    (1u << 1) | XHCI_TRB_CYCLE;
    g_xhci.Erst[0].RingSegmentBase = event_pa;
    g_xhci.Erst[0].RingSegmentSize = XHCI_TRBS_PER_RING;

    xhci_write64(g_xhci.OperationalOffset + XHCI_DCBAAP, dcbaa_pa);
    xhci_write64(g_xhci.OperationalOffset + XHCI_CRCR,
                 command_pa | XHCI_TRB_CYCLE);
    xhci_write32(g_xhci.OperationalOffset + XHCI_CONFIG,
                 g_xhci.MaxSlots);

    UINT32 interrupter = g_xhci.RuntimeOffset + 0x20u;
    xhci_write32(interrupter + 0x08u, 1);
    xhci_write64(interrupter + 0x10u, erst_pa);
    xhci_write64(interrupter + 0x18u, event_pa | (1u << 3));
    xhci_write32(interrupter + 0x00u, 0); /* polled: keep IMAN.IE clear */

    xhci_write32(g_xhci.OperationalOffset + XHCI_USBCMD, 1u);
    if (!xhci_wait_status(1u, FALSE)) {
        KeLog("[usb]  xHCI did not enter the running state\n");
        return FALSE;
    }
    g_xhci.Running = TRUE;

    UINT8 completion = 0, slot = 0;
    if (!xhci_command(XHCI_TRB_TYPE_NOOP, 0, &completion, &slot)) {
        KeLog("[usb]  xHCI command ring self-check failed (cc=%u)\n",
              completion);
        return FALSE;
    }

    /* Count connected root ports. They are reset and enumerated below. */
    UINT32 connected = 0;
    for (UINT32 port = 1; port <= g_xhci.MaxPorts; ++port) {
        UINT32 portsc_offset = g_xhci.OperationalOffset + 0x400u +
                               (port - 1u) * 0x10u;
        UINT32 portsc = xhci_read32(portsc_offset);
        if (!(portsc & 1u)) continue;
        ++connected;
    }

    KeLog("[usb]  xHCI %u.%u at %02x:%02x.%u running; %u slots, %u ports, "
          "command ring OK, %u connected\n",
          (unsigned)(cap->InterfaceVersion >> 8),
          (unsigned)(cap->InterfaceVersion & 0xffu), pci->Bus, pci->Device,
          pci->Function, g_xhci.MaxSlots, g_xhci.MaxPorts, connected);
    (void)hcs2;
    return TRUE;
}

BOOLEAN UsbInitialize(void)
{
    PCI_DEVICE pci;
    memset(&g_xhci, 0, sizeof(g_xhci));
    if (!PciFindDeviceByClass(XHCI_PCI_CLASS, XHCI_PCI_SUBCLASS,
                              XHCI_PCI_PROGIF, &pci)) {
        KeLog("[usb]  no PCI xHCI host controller found\n");
        return FALSE;
    }
    if (!xhci_start(&pci)) {
        g_xhci.Running = FALSE;
        return FALSE;
    }
    (void)xhci_enumerate_ports();
    return TRUE;
}

void UsbPollInput(void)
{
    if (g_xhci.Running)
        xhci_poll_hid();
}
