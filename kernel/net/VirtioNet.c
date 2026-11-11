/* Legacy PCI VirtIO network transport (virtio 0.9 transitional interface).
 * RX/TX use split virtqueues backed by physically contiguous DMA memory.
 * Interrupts are not used yet: a PASSIVE_LEVEL worker polls used rings and
 * hands received frames to the registered network layer. */
#include <svindows/Net.h>
#include <svindows/Ke.h>
#include <svindows/Hal.h>
#include <svindows/Mm.h>
#include <svindows/Ex.h>
#include <svindows/Rtl.h>
#include <nt/Ntstatus.h>
#include "Pci.h"

#define VIRTIO_VENDOR_ID 0x1AF4u
#define VIRTIO_NET_DEVICE_ID 0x1000u
#define VIRTIO_NET_F_MAC (1u << 5)
#define VIRTIO_STATUS_ACKNOWLEDGE 1u
#define VIRTIO_STATUS_DRIVER      2u
#define VIRTIO_STATUS_DRIVER_OK   4u
#define VIRTIO_STATUS_FEATURES_OK  8u
#define VIRTQ_DESC_F_WRITE 2u
#define VIRTIO_NET_HDR_SIZE 10u
#define VIRTIO_MAX_QUEUE_SIZE 32768u
#define VIRTIO_QUEUE_ACTIVE_DESCRIPTORS 16u
#define VIRTIO_NET_BUFFER_SIZE 2048u
#define VIRTIO_RX_QUEUE 0u
#define VIRTIO_TX_QUEUE 1u
#define VIRTIO_POLL_TICKS 1u

#pragma pack(push, 1)
typedef struct _VIRTQ_DESC {
    UINT64 Address;
    UINT32 Length;
    UINT16 Flags;
    UINT16 Next;
} VIRTQ_DESC;

typedef struct _VIRTQ_USED_ELEMENT {
    UINT32 Id;
    UINT32 Length;
} VIRTQ_USED_ELEMENT;
#pragma pack(pop)

typedef struct _VIRTQ {
    UINT16 Index;
    UINT16 Size;
    UINT16 LastUsed;
    UINT16 *AvailFlags;
    volatile UINT16 *AvailIndex;
    volatile UINT16 *AvailRing;
    volatile UINT16 *UsedFlags;
    volatile UINT16 *UsedIndex;
    volatile VIRTQ_USED_ELEMENT *UsedRing;
    VIRTQ_DESC *Descriptors;
    UINT64 RingPhysical;
    UINT32 RingPages;
    UINT16 BuffersAllocated;
    UINT16 BufferCount;
    void **Buffers;
    UINT64 *BufferPhysical;
    UINT16 *FreeDescriptors;
    UINT16 FreeCount;
} VIRTQ;

static UINT16 g_io_base;
static VIRTQ g_rx_queue;
static VIRTQ g_tx_queue;
static UINT8 g_mac[NET_MAC_LENGTH];
static NET_INTERFACE_STATS g_stats;

static void count_frame(SV_NET_TRAFFIC_INFO *statistics, const UINT8 *frame,
                         UINT32 length)
{
    if (length < 6) return;
    BOOLEAN broadcast = TRUE;
    for (UINT32 i = 0; i < 6; ++i) if (frame[i] != 255) broadcast = FALSE;
    if (broadcast) {
        statistics->BroadcastPackets++; statistics->BroadcastBytes += length;
    } else if (frame[0] & 1) {
        statistics->MulticastPackets++; statistics->MulticastBytes += length;
    } else {
        statistics->UnicastPackets++; statistics->UnicastBytes += length;
    }
}
static NET_RECEIVE_HANDLER g_receive_handler;
static PVOID g_receive_context;
static BOOLEAN g_ready;
static KEVENT g_poll_event;
static const char *g_virtio_diagnostic = "VirtIO initialization has not run";

const char *VirtioNetGetDiagnostic(void)
{
    return g_virtio_diagnostic;
}

static UINT16 virtio_read16(UINT16 reg) { return __inword((UINT16)(g_io_base + reg)); }
static UINT32 virtio_read32(UINT16 reg) { return __indword((UINT16)(g_io_base + reg)); }
static void virtio_write16(UINT16 reg, UINT16 value)
{ __outword((UINT16)(g_io_base + reg), value); }
static void virtio_write32(UINT16 reg, UINT32 value)
{ __outdword((UINT16)(g_io_base + reg), value); }

static UINT32 align_up_u32(UINT32 value, UINT32 alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

/* DMA addresses are physical bus addresses; CPU pointers must always use the
 * permanent kernel direct map.  Do not use the early-boot identity fallback
 * from MmPhysToVirt() in a live device worker: a stale/early direct-map base
 * would turn a DMA physical address into a low, unmapped CPU pointer.  The
 * network driver is initialized only after VMM has installed this mapping. */
static void *virtio_dma_to_virt(UINT64 physical)
{
    UINT64 limit = MmTotalPages() << PAGE_SHIFT;
    if (MmDirectMapBase != MM_DIRECT_MAP_BASE || physical >= limit)
        return NULL;
    UINT64 virtual_address = MM_DIRECT_MAP_BASE + physical;
    if (virtual_address < MM_DIRECT_MAP_BASE)
        return NULL;
    return (void *)(ULONG_PTR)virtual_address;
}

static void queue_cleanup(VIRTQ *queue)
{
    /* Two 2-KiB descriptors share one 4-KiB DMA page. Free each page once. */
    if (queue->BufferPhysical) {
        for (UINT16 i = 0; i < queue->BuffersAllocated; i += 2u)
            if (queue->BufferPhysical[i] != MM_INVALID_PHYS)
                MmFreePages(queue->BufferPhysical[i], 1);
    }
    if (queue->Buffers) ExFreePool(queue->Buffers);
    if (queue->BufferPhysical) ExFreePool(queue->BufferPhysical);
    if (queue->FreeDescriptors) ExFreePool(queue->FreeDescriptors);
    if (queue->RingPhysical && queue->RingPhysical != MM_INVALID_PHYS)
        MmFreePages(queue->RingPhysical, queue->RingPages);
    memset(queue, 0, sizeof(*queue));
}

static BOOLEAN queue_allocate(VIRTQ *queue, UINT16 index,
                              const char **failure_reason)
{
    if (failure_reason) *failure_reason = "unknown virtqueue initialization failure";
    memset(queue, 0, sizeof(*queue));
    virtio_write16(0x0E, index); /* Queue Select */
    UINT16 size = virtio_read16(0x0C);
    if (!size || size > VIRTIO_MAX_QUEUE_SIZE || (size & (size - 1u))) {
        if (failure_reason) *failure_reason = "device reported an invalid or unsupported virtqueue size";
        KeLog("[net] virtio queue %u reported unsupported size %u (maximum %u)\n",
              index, size, VIRTIO_MAX_QUEUE_SIZE);
        return FALSE;
    }

    /* Keep the device's full ring geometry, but use a conservative DMA
       window. Each pair of descriptors consumes one DMA32 page; 16 active
       descriptors therefore need only eight pages per queue. This leaves
       enough low-memory headroom for the kernel and avoids failing the whole
       network bring-up on machines with fragmented or modest DMA32 RAM. */
    UINT16 buffer_count = size < VIRTIO_QUEUE_ACTIVE_DESCRIPTORS ?
                          size : VIRTIO_QUEUE_ACTIVE_DESCRIPTORS;
    UINT32 desc_bytes = (UINT32)size * sizeof(VIRTQ_DESC);
    UINT32 avail_bytes = 4u + (UINT32)size * 2u + 2u;
    UINT32 used_offset = align_up_u32(desc_bytes + avail_bytes, PAGE_SIZE);
    UINT32 used_bytes = 4u + (UINT32)size * sizeof(VIRTQ_USED_ELEMENT) + 2u;
    UINT32 total_bytes = used_offset + used_bytes;
    UINT32 pages = (total_bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    UINT64 physical = MmAllocatePagesBelow4G(pages);
    if (physical == MM_INVALID_PHYS || physical > 0xFFFFFFFFULL ||
        physical + (UINT64)pages * PAGE_SIZE > 0x100000000ULL) {
        if (physical != MM_INVALID_PHYS) MmFreePages(physical, pages);
        if (failure_reason) *failure_reason = "no contiguous DMA32 memory for the virtqueue ring";
        KeLog("[net] virtio queue %u ring allocation failed: pages=%u free=%lu\n",
              index, pages, (unsigned long)MmFreePageCount());
        return FALSE;
    }

    UINT8 *base = (UINT8 *)virtio_dma_to_virt(physical);
    if (!base) {
        MmFreePages(physical, pages);
        if (failure_reason) *failure_reason = "virtqueue ring has no direct-map virtual address";
        KeLog("[net] virtio queue %u ring direct-map failed: phys=0x%lx\n",
              index, (unsigned long)physical);
        return FALSE;
    }
    memset(base, 0, (SIZE_T)pages * PAGE_SIZE);
    queue->Index = index;
    queue->Size = size;
    queue->BufferCount = buffer_count;
    queue->RingPhysical = physical;
    queue->RingPages = pages;
    queue->Descriptors = (VIRTQ_DESC *)base;
    queue->AvailFlags = (UINT16 *)(base + desc_bytes);
    queue->AvailIndex = (volatile UINT16 *)(base + desc_bytes + 2u);
    queue->AvailRing = (volatile UINT16 *)(base + desc_bytes + 4u);
    UINT8 *used = base + used_offset;
    queue->UsedFlags = (volatile UINT16 *)used;
    queue->UsedIndex = (volatile UINT16 *)(used + 2u);
    queue->UsedRing = (volatile VIRTQ_USED_ELEMENT *)(used + 4u);

    queue->Buffers = ExAllocatePoolWithTag(NonPagedPool,
        (SIZE_T)size * sizeof(void *), 'rVnS');
    queue->BufferPhysical = ExAllocatePoolWithTag(NonPagedPool,
        (SIZE_T)size * sizeof(UINT64), 'pVnS');
    if (!queue->Buffers || !queue->BufferPhysical) {
        if (failure_reason) *failure_reason = "nonpaged metadata allocation for virtqueue buffers failed";
        KeLog("[net] virtio queue %u metadata allocation failed: descriptors=%u\n",
              index, size);
        queue_cleanup(queue);
        return FALSE;
    }
    memset(queue->Buffers, 0, (SIZE_T)size * sizeof(void *));
    for (UINT16 i = 0; i < size; ++i)
        queue->BufferPhysical[i] = MM_INVALID_PHYS;

    if (index == VIRTIO_TX_QUEUE) {
        queue->FreeDescriptors = ExAllocatePoolWithTag(NonPagedPool,
            (SIZE_T)buffer_count * sizeof(UINT16), 'fVnS');
        if (!queue->FreeDescriptors) {
            if (failure_reason) *failure_reason = "nonpaged TX descriptor pool allocation failed";
            KeLog("[net] virtio TX free-descriptor allocation failed: count=%u\n",
                  buffer_count);
            queue_cleanup(queue);
            return FALSE;
        }
    }

    /* Each page carries two non-overlapping 2-KiB DMA buffers. That is large
       enough for a 1514-byte Ethernet frame plus the 10-byte VirtIO header. */
    for (UINT16 i = 0; i < buffer_count; i += 2u) {
        UINT64 buffer_phys = MmAllocatePageBelow4G();
        if (buffer_phys == MM_INVALID_PHYS ||
            buffer_phys + PAGE_SIZE > 0x100000000ULL) {
            if (buffer_phys != MM_INVALID_PHYS) MmFreePages(buffer_phys, 1);
            if (failure_reason) *failure_reason = "DMA32 page allocation for RX/TX buffers failed";
            KeLog("[net] virtio queue %u buffer allocation failed: allocated=%u/%u free=%lu\n",
                  index, queue->BuffersAllocated, buffer_count,
                  (unsigned long)MmFreePageCount());
            queue_cleanup(queue);
            return FALSE;
        }
        UINT8 *buffer_base = (UINT8 *)virtio_dma_to_virt(buffer_phys);
        if (!buffer_base) {
            MmFreePages(buffer_phys, 1);
            if (failure_reason) *failure_reason = "DMA buffer has no direct-map virtual address";
            queue_cleanup(queue);
            return FALSE;
        }
        for (UINT16 slot = 0; slot < 2u && (UINT16)(i + slot) < buffer_count; ++slot) {
            UINT16 descriptor = (UINT16)(i + slot);
            UINT32 offset = (UINT32)slot * VIRTIO_NET_BUFFER_SIZE;
            UINT64 descriptor_phys = buffer_phys + offset;
            queue->Buffers[descriptor] = buffer_base + offset;
            queue->BufferPhysical[descriptor] = descriptor_phys;
            queue->BuffersAllocated++;
            if (index == VIRTIO_RX_QUEUE) {
                queue->Descriptors[descriptor].Address = descriptor_phys;
                queue->Descriptors[descriptor].Length = VIRTIO_NET_BUFFER_SIZE;
                queue->Descriptors[descriptor].Flags = VIRTQ_DESC_F_WRITE;
                queue->Descriptors[descriptor].Next = 0;
                queue->AvailRing[descriptor] = descriptor;
            } else {
                queue->FreeDescriptors[queue->FreeCount++] =
                    (UINT16)(buffer_count - 1u - descriptor);
            }
        }
    }

    if (index == VIRTIO_RX_QUEUE) {
        __sync_synchronize();
        *queue->AvailIndex = buffer_count;
    }

    UINT32 pfn = (UINT32)(physical >> PAGE_SHIFT);
    virtio_write32(0x08, pfn); /* Queue PFN */
    return TRUE;
}

static void virtio_reap_tx(void)
{
    while (g_tx_queue.LastUsed != *g_tx_queue.UsedIndex) {
        __sync_synchronize();
        VIRTQ_USED_ELEMENT element =
            g_tx_queue.UsedRing[g_tx_queue.LastUsed % g_tx_queue.Size];
        g_tx_queue.LastUsed++;
        if (element.Id < g_tx_queue.BufferCount &&
            g_tx_queue.FreeCount < g_tx_queue.BufferCount)
            g_tx_queue.FreeDescriptors[g_tx_queue.FreeCount++] =
                (UINT16)element.Id;
    }
}

static BOOLEAN virtio_queue_cpu_mapping_valid(const VIRTQ *queue)
{
    if (!queue || !queue->Descriptors || !queue->AvailIndex ||
        !queue->AvailRing || !queue->UsedIndex || !queue->UsedRing ||
        !queue->Buffers)
        return FALSE;
    if ((UINT64)(ULONG_PTR)queue->Descriptors < MM_DIRECT_MAP_BASE ||
        (UINT64)(ULONG_PTR)queue->AvailIndex < MM_DIRECT_MAP_BASE ||
        (UINT64)(ULONG_PTR)queue->AvailRing < MM_DIRECT_MAP_BASE ||
        (UINT64)(ULONG_PTR)queue->UsedIndex < MM_DIRECT_MAP_BASE ||
        (UINT64)(ULONG_PTR)queue->UsedRing < MM_DIRECT_MAP_BASE)
        return FALSE;
    for (UINT16 i = 0; i < queue->BufferCount; ++i)
        if (!queue->Buffers[i] ||
            (UINT64)(ULONG_PTR)queue->Buffers[i] < MM_DIRECT_MAP_BASE)
            return FALSE;
    return TRUE;
}

static void virtio_poll_rx(void)
{
    /* A DMA address must never be dereferenced by the CPU. If queue metadata
       is damaged, stop the worker rather than faulting on a low physical VA. */
    if (!virtio_queue_cpu_mapping_valid(&g_rx_queue)) {
        g_virtio_diagnostic = "RX virtqueue contains a non-virtual CPU pointer";
        return;
    }
    for (;;) {
        UINT16 descriptor;
        UINT32 length;
        void *buffer;
        UINT64 irq = KiIrqSave();
        if (g_rx_queue.LastUsed == *g_rx_queue.UsedIndex) {
            KiIrqRestore(irq);
            break;
        }
        __sync_synchronize();
        VIRTQ_USED_ELEMENT element =
            g_rx_queue.UsedRing[g_rx_queue.LastUsed % g_rx_queue.Size];
        g_rx_queue.LastUsed++;
        descriptor = (UINT16)element.Id;
        length = element.Length;
        if (descriptor >= g_rx_queue.BufferCount) {
            g_stats.RxDropped++;
            KiIrqRestore(irq);
            continue;
        }
        buffer = g_rx_queue.Buffers[descriptor];
        KiIrqRestore(irq);

        if (length >= VIRTIO_NET_HDR_SIZE &&
            length <= VIRTIO_NET_BUFFER_SIZE &&
            length - VIRTIO_NET_HDR_SIZE <= NET_MAX_FRAME) {
            UINT32 frame_length = length - VIRTIO_NET_HDR_SIZE;
            const UINT8 *frame = (const UINT8 *)buffer + VIRTIO_NET_HDR_SIZE;
            irq = KiIrqSave();
            NET_RECEIVE_HANDLER handler = g_receive_handler;
            PVOID handler_context = g_receive_context;
            KiIrqRestore(irq);
            if (handler) {
                handler(frame, frame_length, handler_context);
                irq = KiIrqSave();
                g_stats.RxPackets++;
                g_stats.RxBytes += frame_length;
                count_frame(&g_stats.RxTraffic, frame, frame_length);
                KiIrqRestore(irq);
            } else {
                irq = KiIrqSave();
                g_stats.RxDropped++;
                KiIrqRestore(irq);
            }
        } else {
            irq = KiIrqSave();
            g_stats.RxDropped++;
            KiIrqRestore(irq);
        }

        irq = KiIrqSave();
        g_rx_queue.AvailRing[*g_rx_queue.AvailIndex % g_rx_queue.Size] = descriptor;
        __sync_synchronize();
        (*g_rx_queue.AvailIndex)++;
        __sync_synchronize();
        KiIrqRestore(irq);
        virtio_write16(0x10, VIRTIO_RX_QUEUE);
    }
}

static void VirtioNetWorker(PVOID context)
{
    (void)context;
    for (;;) {
        virtio_poll_rx();
        UINT64 irq = KiIrqSave();
        virtio_reap_tx();
        KiIrqRestore(irq);
        (void)KeWaitForSingleObjectTimeout(&g_poll_event.Header,
                                           VIRTIO_POLL_TICKS);
    }
}

BOOLEAN VirtioNetInitialize(void)
{
    PCI_DEVICE device;
    g_virtio_diagnostic = "searching for PCI VirtIO-net";
    if (!PciFindVirtioNet(&device)) {
        g_virtio_diagnostic = PciGetDiagnostic();
        KeLog("[net] VirtIO discovery failed: %s\n", g_virtio_diagnostic);
        return FALSE;
    }
    if (device.VendorId != VIRTIO_VENDOR_ID ||
        device.DeviceId != VIRTIO_NET_DEVICE_ID) {
        g_virtio_diagnostic = "selected PCI function is not legacy VirtIO-net 1af4:1000";
        KeLog("[net] unsupported PCI ID %04x:%04x\n",
              device.VendorId, device.DeviceId);
        return FALSE;
    }

    UINT32 bar0 = device.Bar[0];
    if (!(bar0 & 1u)) {
        g_virtio_diagnostic = "BAR0 is not an I/O BAR; device did not expose legacy transport";
        KeLog("[net] legacy VirtIO-net BAR0 is not an I/O BAR (0x%08x); use QEMU machine=pc with legacy enabled\n",
              (unsigned)bar0);
        return FALSE;
    }
    g_io_base = (UINT16)(bar0 & ~3u);
    if (!g_io_base || g_io_base > 0xFF00u) {
        g_virtio_diagnostic = "BAR0 contains an invalid or unassigned I/O port base";
        KeLog("[net] invalid VirtIO I/O base 0x%04x from BAR0=0x%08x\n",
              g_io_base, (unsigned)bar0);
        return FALSE;
    }

    /* PCI command is the low half of 0x04. Do not echo the status half back:
       PCI status bits are write-one-to-clear. Enable I/O decoding and DMA. */
    UINT32 command_status = PciReadConfig32(device.Bus, device.Device,
                                            device.Function, 0x04);
    UINT16 command = (UINT16)command_status;
    command |= 0x0005u; /* PCI_COMMAND_IO | PCI_COMMAND_MASTER */
    PciWriteConfig32(device.Bus, device.Device, device.Function, 0x04,
                     (UINT32)command);
    command_status = PciReadConfig32(device.Bus, device.Device,
                                     device.Function, 0x04);
    if (((UINT16)command_status & 0x0005u) != 0x0005u) {
        g_virtio_diagnostic = "PCI refused I/O-space or bus-master enable";
        KeLog("[net] PCI command enable failed: command=0x%04x\n",
              (unsigned)(UINT16)command_status);
        return FALSE;
    }

    __outbyte((UINT16)(g_io_base + 0x12), 0); /* reset */
    /* Acknowledge the reset before reusing legacy queue state. */
    UINT8 reset_status = 0xFFu;
    for (UINT32 retry = 0; retry < 1000000u; ++retry) {
        reset_status = __inbyte((UINT16)(g_io_base + 0x12));
        if (reset_status == 0) break;
        __pause();
    }
    if (reset_status != 0) {
        g_virtio_diagnostic = "VirtIO legacy device did not acknowledge reset";
        KeLog("[net] VirtIO device did not reset (status=0x%02x)\n",
              (unsigned)reset_status);
        return FALSE;
    }
    __outbyte((UINT16)(g_io_base + 0x12),
              VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    UINT32 features = virtio_read32(0x00);
    if (!(features & VIRTIO_NET_F_MAC)) {
        g_virtio_diagnostic = "VirtIO host features do not include VIRTIO_NET_F_MAC";
        KeLog("[net] virtio-net: host features 0x%08x do not expose VIRTIO_NET_F_MAC\n",
              (unsigned)features);
        return FALSE;
    }
    /* Complete the feature-negotiation phase before touching virtqueues.
       Kyronix and the VirtIO 1.0 transition rules both set FEATURES_OK after
       writing the guest feature bitmap. A device that clears the bit rejected
       the selected feature set; do not continue with an undefined transport. */
    virtio_write32(0x04, features & VIRTIO_NET_F_MAC);
    UINT8 negotiated_status = (UINT8)(VIRTIO_STATUS_ACKNOWLEDGE |
        VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    __outbyte((UINT16)(g_io_base + 0x12), negotiated_status);
    UINT8 device_status = __inbyte((UINT16)(g_io_base + 0x12));
    if (!(device_status & VIRTIO_STATUS_FEATURES_OK)) {
        g_virtio_diagnostic = "VirtIO device rejected FEATURES_OK after feature negotiation";
        KeLog("[net] virtio-net: FEATURES_OK rejected (status=0x%02x features=0x%08x)\n",
              (unsigned)device_status, (unsigned)features);
        __outbyte((UINT16)(g_io_base + 0x12), 0x80u);
        return FALSE;
    }
    for (UINT32 i = 0; i < NET_MAC_LENGTH; ++i)
        g_mac[i] = __inbyte((UINT16)(g_io_base + 0x14u + i));

    const char *queue_failure = NULL;
    if (!queue_allocate(&g_rx_queue, VIRTIO_RX_QUEUE, &queue_failure)) {
        g_virtio_diagnostic = queue_failure ? queue_failure : "VirtIO RX queue initialization failed";
        KeLog("[net] virtio-net: RX virtqueue initialization failed: %s\n",
              g_virtio_diagnostic);
        queue_cleanup(&g_rx_queue);
        queue_cleanup(&g_tx_queue);
        __outbyte((UINT16)(g_io_base + 0x12), 0x80u);
        return FALSE;
    }
    if (!queue_allocate(&g_tx_queue, VIRTIO_TX_QUEUE, &queue_failure)) {
        g_virtio_diagnostic = queue_failure ? queue_failure : "VirtIO TX queue initialization failed";
        KeLog("[net] virtio-net: TX virtqueue initialization failed: %s\n",
              g_virtio_diagnostic);
        queue_cleanup(&g_rx_queue);
        queue_cleanup(&g_tx_queue);
        __outbyte((UINT16)(g_io_base + 0x12), 0x80u); /* FAILED */
        return FALSE;
    }

    __outbyte((UINT16)(g_io_base + 0x12),
              negotiated_status | VIRTIO_STATUS_DRIVER_OK);
    device_status = __inbyte((UINT16)(g_io_base + 0x12));
    if (!(device_status & VIRTIO_STATUS_DRIVER_OK)) {
        g_virtio_diagnostic = "VirtIO device did not enter DRIVER_OK state";
        KeLog("[net] virtio-net: DRIVER_OK rejected (status=0x%02x)\n",
              (unsigned)device_status);
        __outbyte((UINT16)(g_io_base + 0x12), 0x80u);
        queue_cleanup(&g_rx_queue);
        queue_cleanup(&g_tx_queue);
        return FALSE;
    }
    KeInitializeEvent(&g_poll_event, FALSE, FALSE);
    if (!KeCreateThread("NetRxTx", VirtioNetWorker, NULL, 8)) {
        g_virtio_diagnostic = "could not create VirtIO RX/TX worker thread";
        KeLog("[net] virtio-net: failed to create RX/TX worker thread\n");
        g_ready = FALSE;
        __outbyte((UINT16)(g_io_base + 0x12), 0x80u);
        queue_cleanup(&g_rx_queue);
        queue_cleanup(&g_tx_queue);
        return FALSE;
    }
    g_ready = TRUE;
    g_virtio_diagnostic = "legacy VirtIO-net initialized; RX/TX queues active";
    KeLog("[net] VirtIO-net ready: MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
          g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    virtio_write16(0x10, VIRTIO_RX_QUEUE);
    return TRUE;
}

BOOLEAN NetIsAvailable(void) { return g_ready; }
const UINT8 *NetGetMacAddress(void) { return g_ready ? g_mac : NULL; }

void NetGetStatistics(NET_INTERFACE_STATS *statistics)
{
    if (!statistics) return;
    UINT64 irq = KiIrqSave();
    *statistics = g_stats;
    KiIrqRestore(irq);
}

void NetSetReceiveHandler(NET_RECEIVE_HANDLER handler, PVOID context)
{
    UINT64 irq = KiIrqSave();
    g_receive_handler = handler;
    g_receive_context = context;
    KiIrqRestore(irq);
}

BOOLEAN NetTransmitFrame(const void *frame, UINT32 length)
{
    if (!g_ready || !frame || !length || length > NET_MAX_FRAME) return FALSE;
    UINT64 irq = KiIrqSave();
    virtio_reap_tx();
    if (!g_tx_queue.FreeCount) {
        g_stats.TxDropped++;
        KiIrqRestore(irq);
        return FALSE;
    }
    UINT16 id = g_tx_queue.FreeDescriptors[--g_tx_queue.FreeCount];
    UINT8 *buffer = (UINT8 *)g_tx_queue.Buffers[id];
    memset(buffer, 0, VIRTIO_NET_HDR_SIZE);
    memcpy(buffer + VIRTIO_NET_HDR_SIZE, frame, length);
    VIRTQ_DESC *descriptor = &g_tx_queue.Descriptors[id];
    descriptor->Address = g_tx_queue.BufferPhysical[id];
    descriptor->Length = VIRTIO_NET_HDR_SIZE + length;
    descriptor->Flags = 0;
    descriptor->Next = 0;
    g_tx_queue.AvailRing[*g_tx_queue.AvailIndex % g_tx_queue.Size] = id;
    __sync_synchronize();
    (*g_tx_queue.AvailIndex)++;
    __sync_synchronize();
    g_stats.TxPackets++;
    g_stats.TxBytes += length;
    count_frame(&g_stats.TxTraffic, frame, length);
    KiIrqRestore(irq);
    virtio_write16(0x10, VIRTIO_TX_QUEUE);
    return TRUE;
}
