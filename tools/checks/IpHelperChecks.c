#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define __declspec(x)
#include "../../user/w/iphlpapi/Iphlpapi.c"
static SV_NET_INTERFACE_INFO fixture;
IP_UINT64 SvNtQueryNetworkInterface(SV_NET_INTERFACE_INFO *info, IP_UINT64 size)
{ assert(size == sizeof(fixture)); *info = fixture; return 1; }
int main(void)
{
    assert(offsetof(IP_IF_ROW2, InOctets) == 1208);
    assert(offsetof(IP_ADAPTER, Luid) == 224);
    IP_DWORD size = 123;
    assert(GetAdaptersAddresses(0, 0, 0, 0, &size) == 232 && !size);
    fixture.Present = fixture.Up = 1; fixture.Mtu = 1500;
    fixture.Address = 0x0f02000a; fixture.Netmask = 0x00ffffff;
    fixture.Mac[0] = 0x52; fixture.Mac[1] = 0x54; fixture.Mac[5] = 0x11;
    fixture.RxBytes = 9876; fixture.TxBytes = 321; fixture.RxDrops = 4;
    fixture.RxTraffic.UnicastPackets = 13;
    fixture.RxTraffic.MulticastPackets = 2;
    fixture.LeaseSeconds = 86400;
    size = 0;
    assert(GetAdaptersAddresses(0, 0, 0, 0, &size) == 111 && size > 448);
    unsigned char *memory = malloc(size + 16); assert(memory);
    memset(memory, 0xa5, size + 16);
    IP_ADAPTER *adapter = (IP_ADAPTER *)memory;
    IP_DWORD needed = size - 1;
    assert(GetAdaptersAddresses(0, 0, 0, adapter, &needed) == 111 && memory[0] == 0xa5);
    assert(GetAdaptersAddresses(0, 0, 0, adapter, &size) == 0);
    assert(adapter->Length == 448 && adapter->Index == 1 && adapter->Mtu == 1500);
    assert(adapter->Name >= (char *)memory && adapter->Name + 39 <= (char *)memory + size);
    assert(!strcmp(adapter->Name, "{53564e45-0000-0001-8000-525400000011}"));
    assert(adapter->FriendlyName[0] == 'E' && adapter->Physical[0] == 0x52);
    IP_UNICAST *address = adapter->Unicast;
    assert(address && address->PrefixLength == 24 && address->Lease == 86400);
    assert(((IP_SOCKADDR4 *)address->Address.Address)->Address == fixture.Address);
    for (unsigned i = 0; i < 16; ++i) assert(memory[size + i] == 0xa5);
    IP_IF_ROW2 row = {0}; row.Index = 1;
    assert(!GetIfEntry2(&row) && row.InOctets == 9876 && row.OutOctets == 321);
    assert(row.InUnicast == 13 && row.InNonUnicast == 2 && row.InDiscards == 4);
    IP_WCHAR name[16]; IP_DWORD bytes = 1;
    assert(NhGetInterfaceNameFromDeviceGuid(&row.Guid, name, &bytes, 0, 0) == 122);
    assert(bytes == sizeof(ip_alias));
    assert(!NhGetInterfaceNameFromDeviceGuid(&row.Guid, name, &bytes, 0, 0) && name[0] == 'E');
    row.Index = 2; row.Luid = 0;
    assert(GetIfEntry2(&row) == 1168);
    assert(GetAdaptersAddresses(23, 0, 0, adapter, &size) == 232);
    free(memory);
    puts("PASS IP Helper ABI, native counters, adapter buffer bounds and absent-interface results");
}
