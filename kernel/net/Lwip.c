/* Real IPv4 stack integration.  The VirtIO worker only delivers complete
 * Ethernet frames; all ARP/IP/TCP/UDP/DHCP/DNS processing runs in lwIP's
 * tcpip_thread through tcpip_input(). */
#include <svindows/Net.h>
#include <svindows/Ke.h>
#include <svindows/Ex.h>
#include <svindows/Rtl.h>
#include <nt/Ntstatus.h>
#include <lwip/err.h>
#include <lwip/init.h>
#include <lwip/tcpip.h>
#include <lwip/netif.h>
#include <netif/ethernet.h>
#include <lwip/etharp.h>
#include <lwip/dhcp.h>
#include <lwip/ip4_addr.h>

static struct netif g_netif;
static KEVENT g_stack_ready_event;
static BOOLEAN g_lwip_initialized;
static BOOLEAN g_interface_added;

static void SvNetifStatusChanged(struct netif *netif)
{
    const ip4_addr_t *address = netif_ip4_addr(netif);
    const ip4_addr_t *netmask = netif_ip4_netmask(netif);
    const ip4_addr_t *gateway = netif_ip4_gw(netif);
    if (ip4_addr_isany_val(*address)) {
        KeLog("[net] DHCP address unavailable\n");
        return;
    }
    KeLog("[net] DHCP lease address=%u.%u.%u.%u netmask=%u.%u.%u.%u gateway=%u.%u.%u.%u\n",
          (unsigned)ip4_addr1(address), (unsigned)ip4_addr2(address),
          (unsigned)ip4_addr3(address), (unsigned)ip4_addr4(address),
          (unsigned)ip4_addr1(netmask), (unsigned)ip4_addr2(netmask),
          (unsigned)ip4_addr3(netmask), (unsigned)ip4_addr4(netmask),
          (unsigned)ip4_addr1(gateway), (unsigned)ip4_addr2(gateway),
          (unsigned)ip4_addr3(gateway), (unsigned)ip4_addr4(gateway));
}

static UINT16 SvReadBe16(const UINT8 *value)
{
    return (UINT16)(((UINT16)value[0] << 8) | value[1]);
}

/* Keep a small, console-copyable trace for diagnosing TCP handshakes without
 * requiring a packet capture. Only the first 48 TCP frames are logged. */
static void SvTraceTcpFrame(const char *direction, const UINT8 *frame,
                            UINT32 length)
{
    static volatile UINT32 trace_count;
    if (!frame || length < 14u + 20u || SvReadBe16(frame + 12) != 0x0800u)
        return;
    const UINT8 *ip = frame + 14u;
    UINT32 ihl = (UINT32)(ip[0] & 0x0fu) * 4u;
    if ((ip[0] >> 4) != 4u || ihl < 20u || length < 14u + ihl + 20u ||
        ip[9] != 6u)
        return;
    const UINT8 *tcp = ip + ihl;
    UINT32 tcp_header = (UINT32)(tcp[12] >> 4) * 4u;
    if (tcp_header < 20u || length < 14u + ihl + tcp_header)
        return;
    UINT32 index = __sync_fetch_and_add(&trace_count, 1u);
    if (index >= 48u) return;
    KeLog("[net-tcp] %s %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u flags=0x%02x frame=%u\n",
          direction,
          (unsigned)ip[12], (unsigned)ip[13], (unsigned)ip[14], (unsigned)ip[15],
          (unsigned)SvReadBe16(tcp),
          (unsigned)ip[16], (unsigned)ip[17], (unsigned)ip[18], (unsigned)ip[19],
          (unsigned)SvReadBe16(tcp + 2), (unsigned)tcp[13], (unsigned)length);
}

/* DNS/UDP console trace: bounded and restricted to port 53, so a user can
 * diagnose resolver traffic without producing a packet-capture file. */
static void SvTraceDnsFrame(const char *direction, const UINT8 *frame,
                            UINT32 length)
{
    static volatile UINT32 trace_count;
    if (!frame || length < 14u + 20u + 8u ||
        SvReadBe16(frame + 12) != 0x0800u)
        return;
    const UINT8 *ip = frame + 14u;
    UINT32 ihl = (UINT32)(ip[0] & 0x0fu) * 4u;
    if ((ip[0] >> 4) != 4u || ihl < 20u ||
        length < 14u + ihl + 8u || ip[9] != 17u)
        return;
    const UINT8 *udp = ip + ihl;
    UINT16 source_port = SvReadBe16(udp);
    UINT16 target_port = SvReadBe16(udp + 2);
    UINT16 udp_length = SvReadBe16(udp + 4);
    UINT16 ip_length = SvReadBe16(ip + 2);
    if (source_port != 53u && target_port != 53u) return;
    if (ip_length < ihl + 8u || ip_length > length - 14u ||
        udp_length < 8u || udp_length > ip_length - ihl ||
        length < 14u + ihl + udp_length) return;
    UINT16 dns_id = 0, dns_flags = 0, questions = 0, answers = 0;
    if (udp_length >= 20u) {
        dns_id = SvReadBe16(udp + 8);
        dns_flags = SvReadBe16(udp + 10);
        questions = SvReadBe16(udp + 12);
        answers = SvReadBe16(udp + 14);
    }
    UINT32 index = __sync_fetch_and_add(&trace_count, 1u);
    if (index >= 32u) return;
    KeLog("[net-dns] %s %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u id=0x%04x qr=%u rcode=%u qd=%u an=%u bytes=%u\n",
          direction,
          (unsigned)ip[12], (unsigned)ip[13], (unsigned)ip[14], (unsigned)ip[15],
          (unsigned)source_port,
          (unsigned)ip[16], (unsigned)ip[17], (unsigned)ip[18], (unsigned)ip[19],
          (unsigned)target_port, (unsigned)dns_id,
          (unsigned)((dns_flags >> 15) & 1u),
          (unsigned)(dns_flags & 0x0fu), (unsigned)questions,
          (unsigned)answers, (unsigned)udp_length);
}

static void SvTraceDhcpFrame(const char *direction, const UINT8 *frame,
                             UINT32 length)
{
    static volatile UINT32 trace_count;
    if (!frame || length < 14u + 20u + 8u ||
        SvReadBe16(frame + 12) != 0x0800u)
        return;
    const UINT8 *ip = frame + 14u;
    UINT32 ihl = (UINT32)(ip[0] & 0x0fu) * 4u;
    if ((ip[0] >> 4) != 4u || ihl < 20u ||
        length < 14u + ihl + 8u || ip[9] != 17u)
        return;
    const UINT8 *udp = ip + ihl;
    UINT16 source_port = SvReadBe16(udp);
    UINT16 target_port = SvReadBe16(udp + 2);
    if (!((source_port == 67u || source_port == 68u) &&
          (target_port == 67u || target_port == 68u)))
        return;
    UINT32 index = __sync_fetch_and_add(&trace_count, 1u);
    if (index >= 16u) return;
    const UINT8 *bootp = udp + 8u;
    if (length < (UINT32)(bootp - frame) + 240u) return;
    UINT8 message_type = 0;
    UINT32 options = 240u;
    while ((UINT32)(bootp - frame) + options + 2u <= length) {
        UINT8 option = bootp[options++];
        if (option == 0) continue;
        if (option == 255) break;
        UINT8 option_length = bootp[options++];
        if ((UINT32)(bootp - frame) + options + option_length > length) break;
        if (option == 53u && option_length == 1u)
            message_type = bootp[options];
        options += option_length;
    }
    KeLog("[net-dhcp] %s %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u op=%u type=%u xid=%02x%02x%02x%02x yiaddr=%u.%u.%u.%u frame=%u\n",
          direction,
          (unsigned)ip[12], (unsigned)ip[13], (unsigned)ip[14], (unsigned)ip[15],
          (unsigned)source_port,
          (unsigned)ip[16], (unsigned)ip[17], (unsigned)ip[18], (unsigned)ip[19],
          (unsigned)target_port, (unsigned)bootp[0], (unsigned)message_type,
          (unsigned)bootp[4], (unsigned)bootp[5], (unsigned)bootp[6],
          (unsigned)bootp[7], (unsigned)bootp[16], (unsigned)bootp[17],
          (unsigned)bootp[18], (unsigned)bootp[19], (unsigned)length);
}

static void SvTraceArpFrame(const char *direction, const UINT8 *frame,
                            UINT32 length)
{
    static volatile UINT32 trace_count;
    if (!frame || length < 42u || SvReadBe16(frame + 12) != 0x0806u)
        return;
    UINT32 index = __sync_fetch_and_add(&trace_count, 1u);
    if (index >= 16u) return;
    KeLog("[net-arp] %s op=%u sender=%u.%u.%u.%u target=%u.%u.%u.%u\n",
          direction, (unsigned)SvReadBe16(frame + 20),
          (unsigned)frame[28], (unsigned)frame[29],
          (unsigned)frame[30], (unsigned)frame[31],
          (unsigned)frame[38], (unsigned)frame[39],
          (unsigned)frame[40], (unsigned)frame[41]);
}

static err_t SvNetifLinkOutput(struct netif *netif, struct pbuf *packet)
{
    (void)netif;
    if (!packet || packet->tot_len == 0 || packet->tot_len > NET_MAX_FRAME)
        return ERR_BUF;
    UINT8 frame[NET_MAX_FRAME];
    if (pbuf_copy_partial(packet, frame, packet->tot_len, 0) != packet->tot_len)
        return ERR_BUF;
    SvTraceTcpFrame("TX", frame, packet->tot_len);
    SvTraceDnsFrame("TX", frame, packet->tot_len);
    SvTraceDhcpFrame("TX", frame, packet->tot_len);
    SvTraceArpFrame("TX", frame, packet->tot_len);
    if (!NetTransmitFrame(frame, packet->tot_len)) {
        KeLog("[net] linkoutput: VirtIO TX queue rejected frame (%u bytes)\n",
              (unsigned)packet->tot_len);
        return ERR_MEM;
    }
    return ERR_OK;
}

static err_t SvNetifInit(struct netif *netif)
{
    const UINT8 *mac = NetGetMacAddress();
    if (!mac) return ERR_IF;
    netif->name[0] = 'e';
    netif->name[1] = '0';
    netif->hwaddr_len = NET_MAC_LENGTH;
    memcpy(netif->hwaddr, mac, NET_MAC_LENGTH);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = SvNetifLinkOutput;
    return ERR_OK;
}

static void SvTcpipInitialized(void *argument)
{
    (void)argument;
    ip4_addr_t address, netmask, gateway;
    IP4_ADDR(&address, 0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gateway, 0, 0, 0, 0);
    /* lwIP's own netif_init() has already installed its 127.0.0.1 loopif
       because LWIP_HAVE_LOOPIF is enabled.  The TCP/IP core must remain
       usable even when PCI discovery or the physical NIC fails. */
    if (NetIsAvailable()) {
        if (!netif_add(&g_netif, &address, &netmask, &gateway, NULL,
                       SvNetifInit, tcpip_input)) {
            KeLog("[net] lwIP could not register the VirtIO interface\n");
        } else {
            g_interface_added = TRUE;
            netif_set_default(&g_netif);
            netif_set_up(&g_netif);
            netif_set_link_up(&g_netif);
            netif_set_status_callback(&g_netif, SvNetifStatusChanged);
            err_t result = dhcp_start(&g_netif);
            if (result != ERR_OK)
                KeLog("[net] DHCP start failed: %d\n", result);
        }
    }
    /* This event means the lwIP core and tcpip_thread are initialized;
       physical NIC availability is tracked separately by NetIsAvailable(). */
    KeSetEvent(&g_stack_ready_event);
}

static void SvReceiveEthernetFrame(const void *frame, UINT32 length,
                                   PVOID context)
{
    (void)context;
    if (!g_interface_added || !frame || length < 14u || length > NET_MAX_FRAME)
        return;
    SvTraceTcpFrame("RX", (const UINT8 *)frame, length);
    SvTraceDnsFrame("RX", (const UINT8 *)frame, length);
    SvTraceDhcpFrame("RX", (const UINT8 *)frame, length);
    SvTraceArpFrame("RX", (const UINT8 *)frame, length);
    struct pbuf *packet = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_POOL);
    if (!packet) return;
    if (pbuf_take(packet, frame, (u16_t)length) != ERR_OK) {
        pbuf_free(packet);
        return;
    }
    if (g_netif.input(packet, &g_netif) != ERR_OK)
        pbuf_free(packet);
}

BOOLEAN LwipInitialize(void)
{
    if (g_lwip_initialized) return TRUE;
    KeInitializeEvent(&g_stack_ready_event, TRUE, FALSE);
    if (NetIsAvailable())
        NetSetReceiveHandler(SvReceiveEthernetFrame, NULL);
    tcpip_init(SvTcpipInitialized, NULL);
    if (KeWaitForSingleObjectTimeout(&g_stack_ready_event.Header, 1000) == STATUS_TIMEOUT) {
        KeLog("[net] lwIP TCP/IP thread initialization timed out\n");
        NetSetReceiveHandler(NULL, NULL);
        return FALSE;
    }
    /* The native loopback interface is part of the lwIP core.  A missing
       physical interface must not disable AF_INET sockets or 127.0.0.1. */
    g_lwip_initialized = TRUE;
    return TRUE;
}
