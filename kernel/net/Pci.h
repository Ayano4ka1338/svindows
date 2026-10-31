#ifndef SVINDOWS_NET_PCI_H
#define SVINDOWS_NET_PCI_H
#include <nt/Ntdef.h>

typedef struct _PCI_DEVICE {
    UINT8 Bus;
    UINT8 Device;
    UINT8 Function;
    UINT16 VendorId;
    UINT16 DeviceId;
    UINT8 InterruptLine;
    UINT8 HeaderType;
    UINT32 Bar[6];
} PCI_DEVICE;

BOOLEAN PciFindVirtioNet(PCI_DEVICE *device);
const char *PciGetDiagnostic(void);
UINT32 PciReadConfig32(UINT8 bus, UINT8 device, UINT8 function,
                       UINT8 offset);
void PciWriteConfig32(UINT8 bus, UINT8 device, UINT8 function,
                      UINT8 offset, UINT32 value);

#endif
