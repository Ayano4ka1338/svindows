#ifndef SVINDOWS_LWIP_OPTS_H
#define SVINDOWS_LWIP_OPTS_H

#define NO_SYS                         0
#define SYS_LIGHTWEIGHT_PROT           1
#define LWIP_NETCONN                   1
#define LWIP_SOCKET                    1
#define LWIP_COMPAT_SOCKETS             0
#define LWIP_SOCKET_OFFSET              0
#define LWIP_SOCKET_POLL               1
#define LWIP_IPV4                      1
#define LWIP_IPV6                      0
#define LWIP_ARP                       1
#define LWIP_ICMP                      1
#define LWIP_RAW                       1
#define LWIP_UDP                       1
#define LWIP_TCP                       1
#define LWIP_DHCP                      1
#define LWIP_DNS                       1
#define LWIP_NETIF_API                 1
/* Keep IPv4 loopback available even when the physical NIC is absent.
 * lwIP creates and manages the native 127.0.0.1 loop interface. */
#define LWIP_HAVE_LOOPIF               1
/* Socket operations run in user threads while lwIP's core is serialized by
 * tcpip_thread. Schedule loopback queue draining on that same core thread. */
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 1
#define LWIP_TIMERS                    1
#define LWIP_NETIF_LINK_CALLBACK       1
#define LWIP_NETIF_STATUS_CALLBACK     1
#define LWIP_NETIF_HOSTNAME            1
#define LWIP_SO_RCVTIMEO               1
#define LWIP_SO_SNDTIMEO               1
#define LWIP_SO_RCVBUF                 1
/* User processes have separate address spaces from the kernel tcpip_thread.
 * Make lwIP allocate API message/call records from its pools instead of
 * placing cross-thread records on a caller's process stack. */
#define LWIP_MPU_COMPATIBLE            1
#define LWIP_TCPIP_CORE_LOCKING        0
#define LWIP_NETCONN_SEM_PER_THREAD    0
#define LWIP_STATS                     1
#define LWIP_STATS_DISPLAY             0
#define LWIP_DEBUG                     1
#define DHCP_DEBUG                     LWIP_DBG_ON
#define LWIP_DHCP_AUTOIP_COOP          0
#define LWIP_DHCP_DOES_ACD_CHECK       0
#define LWIP_DHCP_GET_NTP_SRV          0
#define LWIP_DHCP_MAX_NTP_SERVERS      0
#define LWIP_MDNS_RESPONDER            0
#define LWIP_IGMP                      0
#define LWIP_IPV6_MLD                  0

#define MEM_ALIGNMENT                  8
#define MEM_SIZE                       (512 * 1024)
#define MEMP_NUM_PBUF                  128
#define MEMP_NUM_SYS_TIMEOUT            32
#define MEMP_NUM_RAW_PCB               8
#define MEMP_NUM_UDP_PCB               16
#define MEMP_NUM_TCP_PCB               32
#define MEMP_NUM_TCP_PCB_LISTEN        8
#define MEMP_NUM_TCP_SEG               128
#define MEMP_NUM_NETBUF                32
#define MEMP_NUM_NETCONN               32
#define MEMP_NUM_API_MSG                32
#define MEMP_NUM_TCPIP_MSG_API          32
#define MEMP_NUM_TCPIP_MSG_INPKT        64
#define PBUF_POOL_SIZE                 128
#define PBUF_POOL_BUFSIZE              1600
#define TCP_MSS                        1460
#define TCP_WND                        (16 * TCP_MSS)
#define TCP_LISTEN_BACKLOG             1
#define TCP_SND_BUF                    (8 * TCP_MSS)
#define TCP_SND_QUEUELEN               64
#define TCPIP_MBOX_SIZE                64
#define DEFAULT_ACCEPTMBOX_SIZE        16
#define DEFAULT_TCP_RECVMBOX_SIZE      32
#define DEFAULT_UDP_RECVMBOX_SIZE      32
#define DEFAULT_RAW_RECVMBOX_SIZE      16
#define DEFAULT_THREAD_STACKSIZE       0
#define DEFAULT_THREAD_PRIO            8
#define LWIP_DNS_MAX_SERVERS           2
#define LWIP_DNS_SECURE                0
#define LWIP_DHCP_DOES_ARP_CHECK       1
#define LWIP_CHECKSUM_CTRL_PER_NETIF   0
#define LWIP_CHECKSUM_ON_COPY          1
#define LWIP_NETIF_TX_SINGLE_PBUF      0
#define LWIP_TCP_KEEPALIVE             1
#define LWIP_TCPIP_TIMEOUT             1

#endif
