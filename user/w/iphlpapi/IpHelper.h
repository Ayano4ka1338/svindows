#include "../../../kernel/include/abi/NetQuery.h"
typedef unsigned int IP_DWORD;
typedef unsigned short IP_WCHAR;
typedef unsigned long long IP_UINT64;
typedef struct { IP_DWORD Data1; unsigned short Data2, Data3; unsigned char Data4[8]; } IP_GUID;
typedef struct { void *Address; int Length; } IP_SOCKET_ADDRESS;
typedef struct {
    IP_UINT64 Luid;
    IP_DWORD Index;
    IP_GUID Guid;
    IP_WCHAR Alias[257], Description[257];
    IP_DWORD PhysicalLength;
    unsigned char Physical[32], Permanent[32];
    IP_DWORD Mtu, Type, TunnelType, MediaType, PhysicalMedium, Access, Direction;
    unsigned char StatusFlags;
    IP_DWORD OperStatus, AdminStatus, MediaState;
    IP_GUID NetworkGuid;
    IP_DWORD ConnectionType;
    IP_UINT64 TxSpeed, RxSpeed, InOctets, InUnicast, InNonUnicast, InDiscards,
        InErrors, InUnknown, InUnicastOctets, InMulticastOctets, InBroadcastOctets,
        OutOctets, OutUnicast, OutNonUnicast, OutDiscards, OutErrors,
        OutUnicastOctets, OutMulticastOctets, OutBroadcastOctets, QueueLength;
} IP_IF_ROW2;
_Static_assert(sizeof(IP_IF_ROW2) == 1352, "MIB_IF_ROW2 x64");
typedef struct {
    IP_DWORD Length, Index;
    void *Next;
    char *Name;
    void *Unicast, *Anycast, *Multicast, *DnsServer;
    IP_WCHAR *DnsSuffix, *Description, *FriendlyName;
    unsigned char Physical[8];
    IP_DWORD PhysicalLength, Flags, Mtu, Type, Status, Ipv6Index, Zone[16];
    void *Prefix;
    IP_UINT64 TxSpeed, RxSpeed;
    void *WinsServer, *Gateway;
    IP_DWORD Ipv4Metric, Ipv6Metric;
    IP_UINT64 Luid;
    IP_SOCKET_ADDRESS Dhcp4;
    IP_DWORD Compartment;
    IP_GUID NetworkGuid;
    IP_DWORD ConnectionType, TunnelType;
    IP_SOCKET_ADDRESS Dhcp6;
    unsigned char Duid[130];
    IP_DWORD DuidLength, Dhcp6Iaid;
    void *FirstDnsSuffix;
} IP_ADAPTER;
_Static_assert(sizeof(IP_ADAPTER) == 448, "IP_ADAPTER_ADDRESSES x64");
typedef struct {
    IP_DWORD Length, Flags;
    void *Next;
    IP_SOCKET_ADDRESS Address;
    IP_DWORD PrefixOrigin, SuffixOrigin, DadState, Valid, Preferred, Lease;
    unsigned char PrefixLength;
} IP_UNICAST;
_Static_assert(sizeof(IP_UNICAST) == 64, "IP_ADAPTER_UNICAST_ADDRESS x64");
typedef struct { unsigned short Family, Port; IP_DWORD Address; unsigned char Zero[8]; } IP_SOCKADDR4;
