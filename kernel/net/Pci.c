/* PCI configuration mechanism #1. Discovery is deliberately bus-oriented:
 * drivers receive a discovered function and never scan config space ad hoc. */
#include <svindows/Hal.h>
#include <svindows/Ke.h>
#include <svindows/Rtl.h>
#include "Pci.h"

#define PCI_CONFIG_ADDRESS 0x0CF8u
#define PCI_CONFIG_DATA    0x0CFcu
#define PCI_VENDOR_VIRTIO  0x1AF4u
#define PCI_DEVICE_VIRTIO_NET_LEGACY 0x1000u

static const char *g_pci_diagnostic = "PCI discovery has not run";

const char *PciGetDiagnostic(void)
{
    return g_pci_diagnostic;
}

static UINT32 pci_address(UINT8 bus, UINT8 device, UINT8 function,
                          UINT8 offset)
{
    return 0x80000000u | ((UINT32)bus << 16) |
           ((UINT32)device << 11) | ((UINT32)function << 8) |
           ((UINT32)offset & 0xFCu);
}

UINT32 PciReadConfig32(UINT8 bus, UINT8 device, UINT8 function,
                       UINT8 offset)
{
    /* CF8/CFC is one shared address/data transaction. Keep an interrupt
       handler from changing CF8 between the address write and data read. */
    UINT64 irq = KiIrqSave();
    __outdword(PCI_CONFIG_ADDRESS,
               pci_address(bus, device, function, offset));
    UINT32 value = __indword(PCI_CONFIG_DATA);
    KiIrqRestore(irq);
    return value;
}

void PciWriteConfig32(UINT8 bus, UINT8 device, UINT8 function,
                      UINT8 offset, UINT32 value)
{
    UINT64 irq = KiIrqSave();
    __outdword(PCI_CONFIG_ADDRESS,
               pci_address(bus, device, function, offset));
    __outdword(PCI_CONFIG_DATA, value);
    KiIrqRestore(irq);
}

BOOLEAN PciFindDeviceByClass(UINT8 class_code, UINT8 subclass,
                             UINT8 programming_interface,
                             PCI_DEVICE *device)
{
    if (!device) return FALSE;
    memset(device, 0, sizeof(*device));

    for (UINT32 bus = 0; bus < 256; ++bus) {
        for (UINT32 dev = 0; dev < 32; ++dev) {
            UINT32 id0 = PciReadConfig32((UINT8)bus, (UINT8)dev, 0, 0x00);
            if ((UINT16)id0 == 0xffffu) continue;
            UINT32 header = PciReadConfig32((UINT8)bus, (UINT8)dev, 0, 0x0c);
            UINT8 functions = ((header >> 16) & 0x80u) ? 8u : 1u;
            for (UINT8 fn = 0; fn < functions; ++fn) {
                UINT32 id = fn ? PciReadConfig32((UINT8)bus, (UINT8)dev,
                                                  fn, 0x00) : id0;
                if ((UINT16)id == 0xffffu) continue;
                UINT32 class_reg = PciReadConfig32((UINT8)bus, (UINT8)dev,
                                                    fn, 0x08);
                if ((UINT8)(class_reg >> 24) != class_code ||
                    (UINT8)(class_reg >> 16) != subclass ||
                    (UINT8)(class_reg >> 8) != programming_interface)
                    continue;

                device->Bus = (UINT8)bus;
                device->Device = (UINT8)dev;
                device->Function = fn;
                device->VendorId = (UINT16)id;
                device->DeviceId = (UINT16)(id >> 16);
                device->ClassCode = class_code;
                device->SubClass = subclass;
                device->ProgrammingInterface = programming_interface;
                device->HeaderType = (UINT8)(header >> 16);
                device->InterruptLine = (UINT8)PciReadConfig32(
                    (UINT8)bus, (UINT8)dev, fn, 0x3c);
                for (UINT8 bar = 0; bar < 6; ++bar)
                    device->Bar[bar] = PciReadConfig32(
                        (UINT8)bus, (UINT8)dev, fn,
                        (UINT8)(0x10u + bar * 4u));
                return TRUE;
            }
        }
    }
    return FALSE;
}

BOOLEAN PciFindVirtioNet(PCI_DEVICE *device)
{
    BOOLEAN saw_virtio = FALSE;
    BOOLEAN saw_modern_net = FALSE;
    BOOLEAN saw_any_device = FALSE;
    if (!device) {
        g_pci_diagnostic = "PCI discovery called with a null result buffer";
        return FALSE;
    }
    memset(device, 0, sizeof(*device));
    g_pci_diagnostic = "scanning PCI buses 0-255";

    for (UINT32 bus = 0; bus < 256; ++bus) {
        for (UINT32 dev = 0; dev < 32; ++dev) {
            UINT32 id0 = PciReadConfig32((UINT8)bus, (UINT8)dev, 0, 0x00);
            if ((UINT16)id0 == 0xFFFFu) continue;
            saw_any_device = TRUE;

            UINT32 hdr = PciReadConfig32((UINT8)bus, (UINT8)dev, 0, 0x0C);
            UINT8 functions = ((hdr >> 16) & 0x80u) ? 8u : 1u;
            for (UINT8 fn = 0; fn < functions; ++fn) {
                UINT32 id = (fn == 0) ? id0 :
                    PciReadConfig32((UINT8)bus, (UINT8)dev, fn, 0x00);
                UINT16 vendor = (UINT16)id;
                UINT16 device_id = (UINT16)(id >> 16);
                if (vendor == 0xFFFFu) continue;

                if (vendor == PCI_VENDOR_VIRTIO) {
                    saw_virtio = TRUE;
                    KeLog("[pci] VirtIO function %02x:%02x.%u id=%04x:%04x\n",
                          (unsigned)bus, (unsigned)dev, (unsigned)fn,
                          (unsigned)vendor, (unsigned)device_id);
                }

                if (vendor == PCI_VENDOR_VIRTIO &&
                    device_id == PCI_DEVICE_VIRTIO_NET_LEGACY) {
                    device->Bus = (UINT8)bus;
                    device->Device = (UINT8)dev;
                    device->Function = fn;
                    device->VendorId = vendor;
                    device->DeviceId = device_id;
                    UINT32 header = PciReadConfig32((UINT8)bus, (UINT8)dev,
                                                    fn, 0x0C);
                    device->HeaderType = (UINT8)(header >> 16);
                    device->InterruptLine = (UINT8)PciReadConfig32(
                        (UINT8)bus, (UINT8)dev, fn, 0x3C);
                    for (UINT8 bar = 0; bar < 6; ++bar)
                        device->Bar[bar] = PciReadConfig32(
                            (UINT8)bus, (UINT8)dev, fn,
                            (UINT8)(0x10 + bar * 4));
                    g_pci_diagnostic = "legacy VirtIO-net 1af4:1000 found";
                    KeLog("[pci] legacy VirtIO-net selected at %02x:%02x.%u BAR0=0x%08x\n",
                          (unsigned)bus, (unsigned)dev, (unsigned)fn,
                          (unsigned)device->Bar[0]);
                    return TRUE;
                }

                /* 0x1041 is the modern-only VirtIO network PCI ID. The
                   current transport needs 0x1000's legacy I/O BAR. */
                if (vendor == PCI_VENDOR_VIRTIO && device_id == 0x1041u)
                    saw_modern_net = TRUE;
            }
        }
    }

    if (saw_modern_net) {
        g_pci_diagnostic = "found modern-only 1af4:1041; driver currently needs legacy 1af4:1000";
        KeLog("[pci] found modern-only VirtIO-net (1af4:1041); legacy transport requires 1af4:1000\n");
    } else if (saw_virtio) {
        g_pci_diagnostic = "VirtIO devices exist, but no legacy network 1af4:1000 was found";
        KeLog("[pci] VirtIO devices found, but no legacy network function (1af4:1000)\n");
    } else if (saw_any_device) {
        g_pci_diagnostic = "PCI devices found; no VirtIO vendor 1af4 function detected";
        KeLog("[pci] PCI devices found, but no VirtIO PCI functions (vendor 1af4)\n");
    } else {
        g_pci_diagnostic = "PCI config scan returned no devices; check PCI host bridge/QEMU machine";
        KeLog("[pci] no PCI functions found; verify PCI host bridge and QEMU machine\n");
    }
    return FALSE;
}
