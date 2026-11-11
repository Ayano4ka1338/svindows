#ifndef SV_NET_QUERY_H
#define SV_NET_QUERY_H
/* Fixed-width snapshot shared with native Win32 IP Helper clients. */
typedef struct {
    unsigned long long UnicastPackets, MulticastPackets, BroadcastPackets;
    unsigned long long UnicastBytes, MulticastBytes, BroadcastBytes;
} SV_NET_TRAFFIC_INFO;
typedef struct {
    unsigned int Size, Present, Up, Address, Netmask, Gateway, Mtu;
    unsigned char Mac[6], Padding[2];
    unsigned int LeaseSeconds;
    unsigned long long RxBytes, TxBytes, RxPackets, TxPackets, RxDrops, TxDrops;
    SV_NET_TRAFFIC_INFO RxTraffic, TxTraffic;
} SV_NET_INTERFACE_INFO;
_Static_assert(sizeof(SV_NET_INTERFACE_INFO) == 184, "network snapshot ABI");
#endif
