# Svindows networking

## Implemented layers

1. `kernel/net/Pci.c` scans PCI configuration space for the transitional
   VirtIO network device (`1af4:1000`).
2. `kernel/net/VirtioNet.c` negotiates the legacy MAC feature, allocates
   below-4-GiB DMA rings and split RX/TX virtqueues, and runs a passive
   worker that polls used rings and delivers complete Ethernet frames. The
   driver keeps the device-reported ring size but posts at most 64 descriptors
   per queue; two independent 2-KiB frame buffers share each 4-KiB DMA page.
   Initialization reports whether the ring, metadata, or frame-buffer DMA
   allocation failed instead of collapsing all causes into one message.
3. `kernel/net/Lwip.c` registers an lwIP `netif`. Receive frames are wrapped in
   pbufs and submitted with `tcpip_input`; lwIP's TCP/IP thread owns ARP, IPv4,
   ICMP, UDP, TCP, DHCP and DNS processing. Transmit frames go through the
   netif `linkoutput` callback and `NetTransmitFrame`.
4. `kernel/net/LwipSysArch.c` implements lwIP semaphores, mailboxes, mutexes,
   threads, timing and critical sections on top of the Svindows dispatcher.

lwIP is fetched at build time from the pinned upstream 2.2.1 commit recorded in
`tools/lwip.lock.json`. `tools/StageLwip.py` validates the release header and
places the source under `build/lwip/src`. A working network connection on the
build host is required for the first build unless the staged source is already
present.

`make run` and `make run-gui` boot the Limine ISO, which carries the writable
FAT32 system volume as `boot/initrd.fat`; the kernel uses it as a memory-backed
block device. They attach QEMU's user-mode network backend to a transitional
`virtio-net-pci` device. On boot, the kernel starts a DHCP client
and logs the lease address, netmask and gateway. `tools/QemuSmoke.py` verifies
the real guest path by pinging the QEMU gateway, resolving and pinging
`example.com`, and sending HTTP and certificate-verified HTTPS requests through `curl`
command. These checks run inside the guest, over lwIP and VirtIO, not on the
build host.

## Current limitations

- The current driver supports the transitional legacy PCI VirtIO-net interface;
  modern-only VirtIO and physical NIC drivers are not included. RX/TX currently
  use a polling worker rather than device interrupts.
- The Linux x86-64 syscall layer now bridges `AF_INET` sockets to lwIP's BSD
  socket API after both the VirtIO device and lwIP netif are ready. An IP
  socket request while networking is unavailable returns `ENETDOWN` rather
  than calling uninitialized lwIP pools and surfacing a misleading `ENOBUFS`.
  It handles `SOCK_STREAM`, `SOCK_DGRAM` and `SOCK_RAW`, bind,
  connect, listen/accept4, read/write, sendto/recvfrom, sendmsg/recvmsg,
  getsockname/getpeername, common socket options, nonblocking mode and poll.
  Linux and lwIP `SOL_SOCKET` option numbers are translated at the boundary.
- Native libc-free `PING.ELF` and packaged Alpine `curl` are installed into
  `C:\USR\BIN` and `/usr/bin`. `ping` accepts IPv4 literals or DNS names;
  `curl` supports HTTP and HTTPS with OpenSSL and the staged CA bundle. DNS
  reads `/etc/resolv.conf`; the image's default resolver is QEMU
  user-networking's `10.0.2.3` and can be replaced for another network backend.
- This implementation currently provides IPv4 on one transitional VirtIO-net
  interface. IPv6, multiple interfaces, routing configuration, firewall
  policy, and tools such as `wget`/`nc` are not
  implemented yet.
- QEMU's default CPU model hides RDRAND. The bundled QEMU configuration uses
  `-cpu max` to provide the hardware random source OpenSSL needs; on physical
  CPUs without RDRAND, secure TLS randomness is unavailable.
- The included end-to-end test uses DHCP, DNS, TCP and HTTP over QEMU user
  networking. It must be run in QEMU to verify actual packet exchange; a
  successful compile does not establish connectivity.

## QEMU network configuration

The QEMU network backend is configurable without editing the Makefile:

```sh
make run-gui
make run-gui QEMU_MEMORY=2048M
make run-gui QEMU_MACHINE=pc
make run-gui QEMU_NETDEV='user,id=svnet0,ipv6=off,hostfwd=tcp::8080-:80'
make run-gui QEMU_NETDEV='user,id=svnet0,ipv6=off' QEMU_EXTRA_ARGS='-net dump,file=build/net.pcap'
```

`QEMU_NETDEV` and `QEMU_NET_DEVICE` must use the same `QEMU_NET_ID`. The
`run-net` boots the system with the same QEMU user-mode NAT network. Run the
guest commands below to check connectivity. `tools/QemuSmoke.py` runs the
automated guest-side DHCP, ICMP, DNS, TCP and HTTP check.

## User-facing tools and Flatpak prerequisites

The image includes native `ping` and Alpine `curl` clients that use the Linux
syscall ABI and the kernel's `AF_INET` socket bridge. `ping` supports IPv4
ICMP echo requests and DNS names. `curl` supports HTTP and HTTPS with
certificate verification using the bundled CA file. From the Svindows shell,
try:

```sh
ping 10.0.2.2
ping -c 4 example.com
curl -I http://example.com/
curl http://example.com/
curl -I https://example.com/
```

Run `make network-check` for the source/ABI checks. Run
`python3 tools/QemuSmoke.py` to verify DHCP, ICMP, DNS, TCP, HTTP and
certificate-verified HTTPS from the guest in QEMU.

This provides the first usable IPv4 networking path for Linux applications,
but it is not yet full Kyronix parity. IPv6, complete Linux socket corner-case
compatibility, ancillary-data control messages, advanced multicast/options,
and additional network tools remain incomplete. Flatpak downloads will require
broader HTTP behavior such as redirect and range handling, alongside a
compatible userspace runtime, mount/user namespaces, cgroups, seccomp,
bubblewrap and portals. None of those Flatpak sandbox/runtime pieces are
claimed as implemented by this network bring-up.


## lwIP API messages across process address spaces

Svindows runs lwIP's `tcpip_thread` in the System process while socket API
calls originate in isolated user processes. `LWIP_MPU_COMPATIBLE=1` is
therefore enabled in `kernel/net/lwip/lwipopts.h`: lwIP allocates API message
and call records from its memory pools rather than relying on caller-stack
storage that is not mapped in the System process. The corresponding network
build revision is bumped so the lwIP objects are rebuilt.

## TCP diagnostics and Linux socket-option ABI (r14)

The legacy VirtIO-net driver now follows the feature-negotiation sequence used
by Kyronix: it writes the selected guest feature bitmap, sets `FEATURES_OK`,
checks that the device retained that status bit, and only then installs the
virtqueues. `DRIVER_OK` is also read back before the RX/TX worker is started.

The Linux AF_INET bridge translates Linux TCP socket-option numbers to lwIP's
BSD socket-option numbers. In particular Linux `TCP_KEEPIDLE=4`,
`TCP_KEEPINTVL=5`, and `TCP_KEEPCNT=6` map to lwIP's corresponding constants
(3, 4, and 5 in lwIP 2.2.1). The old pass-through caused curl's harmless but
misleading `ENOPROTOOPT` warning.

For diagnosis without packet-capture files, the physical network interface
prints up to 48 TCP frames as `[net-tcp] TX/RX`, including IPv4 endpoints,
ports, TCP flags, and Ethernet-frame length. A missing TX SYN points to the
lwIP output/VirtIO TX path; TX SYN without RX SYN-ACK points to the virtual
NIC, host network, or RX path; an RX SYN-ACK that is followed by no ACK points
to lwIP TCP input or its `tcpip_thread` scheduling.

This trace is intentionally bounded and can be removed after TCP handshake
validation.



## Linux/lwIP poll ABI translation (r15)

The r14 TCP trace showed a complete three-way handshake (SYN, SYN-ACK, ACK),
followed by the client closing the connection without sending an HTTP request.
The cause was in `LinuxInetPoll`: it passed Linux `POLLOUT` (`0x0004`) directly
to lwIP's `lwip_poll`, where `POLLOUT` is `0x0002` and `0x0004` means `POLLERR`.
The returned lwIP `revents` bits were also passed through unchanged, although
Linux and lwIP assign different values to `POLLOUT`, `POLLERR`, and `POLLHUP`.

r15 now requests lwIP's own `POLLIN|POLLOUT` masks and explicitly translates
each returned event to the Linux poll ABI. This lets curl observe a completed
nonblocking connect as writable and proceed to send its HTTP request. The
source-level AF_INET checks now lock in both the request and result mappings.
For the next console-only check, the kernel also prints up to 24 positive lwIP
poll results as `[linux-net] lwip poll`, showing both the raw lwIP mask and the
translated Linux mask. A successful connected TCP socket should report Linux
`POLLOUT=0x0004` (normally `linux=0x0004`, or with readable data `linux=0x0005`).

The lwIP 2.2.1 poll constants are defined in `src/include/lwip/sockets.h`;
Linux poll constants follow the Linux UAPI. See the upstream lwIP socket header
and the Linux poll ABI documentation when changing either side.


## Console diagnostics for DNS and UDP (r22)

The Linux AF_INET bridge logs a bounded number of UDP `sendto`/`recvfrom`
operations and empty lwIP `poll` results as `[linux-net]`. These messages help
distinguish a resolver query that never leaves the guest from a DNS response
that arrives but is not reported as readable. The default QEMU user-networking
resolver remains `10.0.2.3`; `curl` by hostname depends on that DNS path before
it can exercise TCP or HTTP. The r22 source checks cover the new trace sites,
but DNS by hostname still requires an end-to-end QEMU test.


The r22 DNS trace prints UDP destinations and received-source addresses in
dotted-decimal form (`[linux-net] sendto dst` / `recvfrom src`) so a console
log can show whether c-ares queries `10.0.2.3` and whether a reply comes back.
This is diagnostic instrumentation; DNS-by-hostname still needs an end-to-end
QEMU run on the target build.


## Hostname resolver fallback (r23)

The default `resolv.conf` retains QEMU user networking's `10.0.2.3` resolver
and adds `1.1.1.1` as a fallback, with bounded resolver timeout/retry options.
The Ethernet trace emits `[net-dns] TX/RX` for UDP port 53, including the DNS
transaction ID, response bit, response code, question count and answer count.
This is diagnostic coverage: a runtime DNS lookup still needs to be confirmed
inside QEMU.
