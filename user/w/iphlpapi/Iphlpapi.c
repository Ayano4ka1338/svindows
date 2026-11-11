#include "IpHelper.h"
__declspec(dllimport) IP_UINT64 SvNtQueryNetworkInterface(SV_NET_INTERFACE_INFO *, IP_UINT64);

static void ip_zero(void *p, IP_UINT64 size)
{ unsigned char *bytes = p; while (size--) *bytes++ = 0; }
static void ip_copy(void *to, const void *from, IP_UINT64 size)
{ unsigned char *d = to; const unsigned char *s = from; while (size--) *d++ = *s++; }
static void ip_wide(IP_WCHAR *out, const IP_WCHAR *text)
{ do { *out++ = *text; } while (*text++); }
static IP_GUID ip_guid(const SV_NET_INTERFACE_INFO *info)
{
    /* Stable per-device identifier derived from the interface MAC. */
    IP_GUID guid = {0x53564e45, 0, 1, {0x80, 0}};
    ip_copy(guid.Data4 + 2, info->Mac, 6);
    return guid;
}
static int ip_guid_equal(const IP_GUID *a, const IP_GUID *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (unsigned i = 0; i < 16; ++i) if (x[i] != y[i]) return 0;
    return 1;
}
static const IP_WCHAR ip_alias[] = L"Ethernet";
static const IP_WCHAR ip_description[] = L"VirtIO Ethernet";
#define IP_LUID 0x0006000001000000ull

__declspec(dllexport) IP_DWORD GetIfEntry2(IP_IF_ROW2 *row)
{
    if (!row || (!row->Luid && !row->Index)) return 87;
    if (row->Luid ? row->Luid != IP_LUID : row->Index != 1) return 1168;
    SV_NET_INTERFACE_INFO info;
    if (!SvNtQueryNetworkInterface(&info, sizeof(info))) return 50;
    if (!info.Present) return 1168; /* ERROR_NOT_FOUND */
    ip_zero(row, sizeof(*row));
    row->Luid = IP_LUID; row->Index = 1; row->Guid = ip_guid(&info);
    ip_wide(row->Alias, ip_alias); ip_wide(row->Description, ip_description);
    row->PhysicalLength = 6;
    ip_copy(row->Physical, info.Mac, 6); ip_copy(row->Permanent, info.Mac, 6);
    row->Mtu = info.Mtu; row->Type = 6;
    row->Access = 2; row->Direction = 0;
    row->StatusFlags = 5 | (info.Up ? 0 : 16);
    row->OperStatus = info.Up ? 1 : 2;
    row->AdminStatus = info.Up ? 1 : 2;
    row->MediaState = info.Up ? 1 : 2;
    row->NetworkGuid = row->Guid; row->ConnectionType = 1;
    /* Legacy VirtIO exposes no negotiated link speed. Zero means unknown. */
    row->InOctets = info.RxBytes; row->OutOctets = info.TxBytes;
    row->InDiscards = info.RxDrops; row->OutDiscards = info.TxDrops;
    row->InUnicast = info.RxTraffic.UnicastPackets;
    row->InNonUnicast = info.RxTraffic.MulticastPackets + info.RxTraffic.BroadcastPackets;
    row->InUnicastOctets = info.RxTraffic.UnicastBytes;
    row->InMulticastOctets = info.RxTraffic.MulticastBytes;
    row->InBroadcastOctets = info.RxTraffic.BroadcastBytes;
    row->OutUnicast = info.TxTraffic.UnicastPackets;
    row->OutNonUnicast = info.TxTraffic.MulticastPackets + info.TxTraffic.BroadcastPackets;
    row->OutUnicastOctets = info.TxTraffic.UnicastBytes;
    row->OutMulticastOctets = info.TxTraffic.MulticastBytes;
    row->OutBroadcastOctets = info.TxTraffic.BroadcastBytes;
    return 0;
}

typedef struct {
    IP_ADAPTER Adapter;
    IP_UNICAST Unicast;
    IP_SOCKADDR4 Address;
    IP_WCHAR Alias[16], Description[32], Empty[1];
    char Name[39];
} IP_ADAPTER_BUFFER;

__declspec(dllexport) IP_DWORD GetAdaptersAddresses(IP_DWORD family, IP_DWORD flags,
    void *reserved, IP_ADAPTER *out, IP_DWORD *size)
{
    if (!size || reserved || (family && family != 2 && family != 23)) return 87;
    if (flags & ~0x2fu) return 50; /* prefixes, gateways and all-interface mode unavailable */
    SV_NET_INTERFACE_INFO info;
    if (!SvNtQueryNetworkInterface(&info, sizeof(info))) return 50;
    if (!info.Present || family == 23) { *size = 0; return 232; }
    IP_DWORD required = sizeof(IP_ADAPTER_BUFFER);
    if (!out || *size < required) { *size = required; return 111; }
    IP_ADAPTER_BUFFER *buffer = (IP_ADAPTER_BUFFER *)out;
    ip_zero(buffer, sizeof(*buffer));
    IP_ADAPTER *adapter = &buffer->Adapter;
    adapter->Length = sizeof(*adapter); adapter->Index = 1;
    adapter->Name = buffer->Name;
    /* GUID string uses the same deterministic device identity as GetIfEntry2. */
    const char prefix[] = "{53564e45-0000-0001-8000-";
    unsigned n = 0; while (prefix[n]) { buffer->Name[n] = prefix[n]; ++n; }
    const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 6; ++i) {
        buffer->Name[n++] = hex[info.Mac[i] >> 4]; buffer->Name[n++] = hex[info.Mac[i] & 15];
    }
    buffer->Name[n] = '}';
    ip_wide(buffer->Alias, ip_alias); ip_wide(buffer->Description, ip_description);
    adapter->FriendlyName = flags & 0x20 ? 0 : buffer->Alias;
    adapter->Description = buffer->Description; adapter->DnsSuffix = buffer->Empty;
    ip_copy(adapter->Physical, info.Mac, 6); adapter->PhysicalLength = 6;
    adapter->Flags = 4 | 0x80; /* DHCPv4 and IPv4 supported. */
    adapter->Mtu = info.Mtu; adapter->Type = 6; adapter->Status = info.Up ? 1 : 2;
    adapter->Luid = IP_LUID; adapter->NetworkGuid = ip_guid(&info);
    adapter->ConnectionType = 1;
    if (!(flags & 1) && info.Address) {
        IP_UNICAST *address = &buffer->Unicast;
        adapter->Unicast = address;
        address->Length = sizeof(*address);
        address->Address.Address = &buffer->Address;
        address->Address.Length = sizeof(buffer->Address);
        address->PrefixOrigin = address->SuffixOrigin = 3; /* DHCP */
        address->DadState = 4; /* Preferred */
        address->Valid = address->Preferred = address->Lease = info.LeaseSeconds;
        const unsigned char *mask = (const unsigned char *)&info.Netmask;
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned bit = 0; bit < 8; ++bit)
                address->PrefixLength += (mask[i] >> bit) & 1;
        buffer->Address.Family = 2; buffer->Address.Address = info.Address;
    }
    *size = required;
    return 0;
}

__declspec(dllexport) IP_DWORD NhGetInterfaceNameFromDeviceGuid(const IP_GUID *guid,
    IP_WCHAR *name, IP_DWORD *bytes, IP_DWORD reserved1, IP_DWORD reserved2)
{
    if (!guid || !bytes || reserved1 || reserved2) return 87;
    SV_NET_INTERFACE_INFO info;
    if (!SvNtQueryNetworkInterface(&info, sizeof(info))) return 50;
    IP_GUID actual = ip_guid(&info);
    if (!info.Present || !ip_guid_equal(guid, &actual)) return 1168;
    IP_DWORD required = sizeof(ip_alias);
    if (!name || *bytes < required) { *bytes = required; return 122; }
    ip_copy(name, ip_alias, required); *bytes = required;
    return 0;
}
