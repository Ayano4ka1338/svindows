#!/usr/bin/env python3
"""Source-level checks for the Linux AF_INET-to-lwIP bridge and network tools."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def require(path: str, fragments: tuple[str, ...]) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    missing = [item for item in fragments if item not in text]
    if missing:
        raise SystemExit(f"FAIL {path}: missing {missing}")
    print(f"PASS {path}")

require("kernel/linux/SocketInet.c", (
    "lwip_socket(AF_INET", "NetIsStackReady()", "LINUX_ENETDOWN",
    "lwip_bind", "lwip_connect", "lwip_accept",
    "lwip_sendto", "lwip_recvfrom", "lwip_poll", "inet_socket_option",
    "inet_stage_send_buffer", "NonPagedPool", "if (write && length && !staged)",
    "lwIP socket create failed", "base_type", "protocol", "errno",
    "#define LWIP_O_NONBLOCK 0x01", "enabled ? LWIP_O_NONBLOCK : 0",
    "destination->sin_len = (u8_t)sizeof(*destination)",
    "if (errno > 0 && errno <= 4095)",
))
require("kernel/linux/SocketMsg.c", (
    "LinuxInetSendMessage", "LinuxInetReceiveMessage", "LINUX_MSG_CMSG_CLOEXEC",
))
require("kernel/linux/Syscall.c", (
    'case 41: /* socket */', 'case 44: /* sendto */',
    'case 45: /* recvfrom */', 'case 46: /* sendmsg */',
))
require("user/l/net/ping.c", ("SOCK_RAW_VALUE", "IPPROTO_ICMP_VALUE", "checksum(",
                                "transmitted_count", "count = 101u", "errno="))
require("user/l/net/curl.c", ("http://", "SOCK_STREAM_VALUE", "HTTP/1.0"))
require("user/l/net/netutil.h", ("/etc/resolv.conf", "sv_dns_resolve", "SYS_POLL"))
require("Makefile", (
    "QEMU_MACHINE         ?= pc", "disable-modern=on,bus=pci.0,addr=0x04",
    "NET_BUILD_REV := 2026-10-31-dpmi-regframe-dns-fallback-r23", "NET_REBUILD_OBJECTS",
    "$(PING): user/l/net/ping.c", "$(ALPINE_CURL): tools/StageAlpineCurl.py",
    "::USR/BIN/PING.ELF", "::USR/BIN/CURL.ELF", "etc/resolv.conf",
))
require("kernel/linux/SocketInet.c", (
    "linux_level == LINUX_IPPROTO_TCP", "case 4: return TCP_KEEPIDLE",
    "case 5: return TCP_KEEPINTVL", "case 6: return TCP_KEEPCNT",
    "#define LWIP_POLLIN  0x0001u", "#define LWIP_POLLOUT 0x0002u",
    "descriptor.events = (short)(LWIP_POLLIN | LWIP_POLLOUT)",
    "if (events & LWIP_POLLOUT) ready |= LINUX_POLLOUT",
    "if (events & LWIP_POLLERR) ready |= LINUX_POLLERR",
    "[linux-net] lwip poll fd=%d revents=0x%04x linux=0x%04x",
    "[linux-net] sendto begin", "[linux-net] %s end",
    "inet_log_ipv4(\"sendto dst\"", "inet_log_ipv4(\"recvfrom src\"",
    "[linux-net] lwip poll fd=%d result=%d revents=0x%04x errno=%d",
))
require("kernel/net/Lwip.c", (
    "SvTraceTcpFrame(\"TX\"", "SvTraceTcpFrame(\"RX\"",
    "[net-tcp] %s", "VirtIO TX queue rejected frame",
))
print("PASS Svindows AF_INET networking source checks")

require("kernel/net/Pci.c", (
    "modern-only VirtIO-net", "PCI devices found, but no VirtIO PCI functions",
    "legacy VirtIO-net selected", "KiIrqSave", "KiIrqRestore",
))
require("kernel/net/VirtioNet.c", (
    "BAR0 is not an I/O BAR", "no contiguous DMA32 memory for the virtqueue ring",
    "DMA32 page allocation for RX/TX buffers failed", "VIRTIO_QUEUE_ACTIVE_DESCRIPTORS 16u",
    "VIRTIO_NET_BUFFER_SIZE 2048u", "PCI refused I/O-space or bus-master enable",
    "VirtIO legacy device did not acknowledge reset",
    "VIRTIO_MAX_QUEUE_SIZE 32768u", "VIRTIO_STATUS_FEATURES_OK  8u",
    "VirtIO device rejected FEATURES_OK", "negotiated_status | VIRTIO_STATUS_DRIVER_OK",
))
ke_main = (ROOT / "kernel/ke/KeMain.c").read_text(encoding="utf-8")
if "Network diagnostic" in ke_main or "NetGetInitStatus" in ke_main:
    raise SystemExit("FAIL kernel/ke/KeMain.c: network status must stay out of INIT output")
print("PASS kernel/ke/KeMain.c: network diagnostics stay out of INIT output")

require("kernel/net/Network.c", (
    "if (!NetIsAvailable() && !VirtioNetInitialize())",
    "if (!LwipInitialize())", "g_stack_ready = TRUE",
))
require("kernel/net/Lwip.c", (
    "LWIP_HAVE_LOOPIF", "tcpip_init(SvTcpipInitialized, NULL)",
    "if (NetIsAvailable())", "g_lwip_initialized = TRUE",
))

require("etc/resolv.conf", ("nameserver 10.0.2.3", "nameserver 1.1.1.1", "options timeout:2 attempts:2"))
