#ifndef _SVINDOWS_NET_H_
#define _SVINDOWS_NET_H_

#include <nt/Ntdef.h>
#include <abi/NetQuery.h>

#define NET_MAC_LENGTH 6u
#define NET_MAX_FRAME  1514u

typedef struct _NET_INTERFACE_STATS {
    UINT64 RxPackets;
    UINT64 RxBytes;
    UINT64 RxDropped;
    UINT64 TxPackets;
    UINT64 TxBytes;
    UINT64 TxDropped;
    SV_NET_TRAFFIC_INFO RxTraffic, TxTraffic;
} NET_INTERFACE_STATS;

/* Initializes the lwIP protocol core and attempts to attach the first supported Ethernet interface. */
BOOLEAN NetInitialize(void);
const char *NetGetInitStatus(void);
BOOLEAN NetIsAvailable(void);
/* True when lwIP/tcpip_thread are ready; independent of physical NIC state. */
BOOLEAN NetIsStackReady(void);
const UINT8 *NetGetMacAddress(void);
void NetGetStatistics(NET_INTERFACE_STATS *statistics);
void NetQueryInterface(SV_NET_INTERFACE_INFO *information);

/* Layer-2 entry points used by the future protocol stack. Frames are copied. */
BOOLEAN NetTransmitFrame(const void *frame, UINT32 length);
typedef void (*NET_RECEIVE_HANDLER)(const void *frame, UINT32 length,
                                    PVOID context);
void NetSetReceiveHandler(NET_RECEIVE_HANDLER handler, PVOID context);

#endif
