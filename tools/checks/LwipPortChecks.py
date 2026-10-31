#!/usr/bin/env python3
"""Static regression checks for the kernel's lwIP compiler/include port."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
CC = ROOT / "kernel/net/lwip/arch/cc.h"
OPTS = ROOT / "kernel/net/lwip/lwipopts.h"
ADAPTER = ROOT / "kernel/net/Lwip.c"
SYS_ARCH = ROOT / "kernel/net/LwipSysArch.c"
STAGER = ROOT / "tools/StageLwip.py"
CTYPE = ROOT / "kernel/include/ctype.h"
STDLIB = ROOT / "kernel/include/stdlib.h"
STRING_IMPL = ROOT / "kernel/rtl/String.c"

cc = CC.read_text(encoding="utf-8")
opts = OPTS.read_text(encoding="utf-8")
adapter = ADAPTER.read_text(encoding="utf-8")
sys_arch = SYS_ARCH.read_text(encoding="utf-8")
stager = STAGER.read_text(encoding="utf-8")
ctype = CTYPE.read_text(encoding="utf-8")
stdlib = STDLIB.read_text(encoding="utf-8")
string_impl = STRING_IMPL.read_text(encoding="utf-8")

if not re.search(r"^#define\s+LWIP_HAVE_LOOPIF\s+1\s*$", opts, re.M):
    raise SystemExit("FAIL: lwIP native IPv4 loopback interface is disabled")
if not re.search(r"^#define\s+LWIP_NETIF_LOOPBACK_MULTITHREADING\s+1\s*$", opts, re.M):
    raise SystemExit("FAIL: loopback packets are not drained on tcpip_thread")

if not re.search(r'^#include\s+[<\"]sys/time\.h[>\"]', cc, re.M):
    raise SystemExit("FAIL: cc.h must import the target fd_set/timeval declarations before lwIP sockets.h")
if not re.search(r"^#define\s+LWIP_TIMEVAL_PRIVATE\s+0\s*$", cc, re.M):
    raise SystemExit("FAIL: lwIP must not redeclare struct timeval")
if not re.search(r"^#define\s+LWIP_NO_STDINT_H\s+0\s*$", cc, re.M):
    raise SystemExit("FAIL: the port must use lwIP's stdint.h typedefs")
if not re.search(r"^#define\s+LWIP_NO_INTTYPES_H\s+0\s*$", cc, re.M):
    raise SystemExit("FAIL: lwIP must include inttypes.h to define U16_F and related format macros")
if not re.search(r"^#define\s+LWIP_NO_LIMITS_H\s+0\s*$", cc, re.M):
    raise SystemExit("FAIL: lwIP must include limits.h so SSIZE_MAX/INT_MAX are defined")
errno_names = (
    "EPERM", "ENOENT", "EINTR", "EIO", "ENXIO", "E2BIG", "ENOEXEC", "EBADF",
    "ECHILD", "EAGAIN", "ENOMEM", "EACCES", "EFAULT", "EBUSY", "EEXIST", "ENODEV",
    "EINVAL", "ENFILE", "EMFILE", "ENOSPC", "EPIPE", "ERANGE", "ENOSYS", "EOVERFLOW",
    "ENOTSOCK", "EDESTADDRREQ", "EMSGSIZE", "EPROTOTYPE", "ENOPROTOOPT",
    "EPROTONOSUPPORT", "EOPNOTSUPP", "EAFNOSUPPORT", "EADDRINUSE", "EADDRNOTAVAIL",
    "ENETDOWN", "ENETUNREACH", "ENETRESET", "ECONNABORTED", "ECONNRESET",
    "ENOBUFS", "EISCONN", "ENOTCONN", "ESHUTDOWN", "ETIMEDOUT", "ECONNREFUSED",
    "EHOSTUNREACH", "EHOSTDOWN", "EALREADY", "EINPROGRESS", "EWOULDBLOCK", "ENOTEMPTY",
)
for name in errno_names:
    if not re.search(r"^#define\s+" + name + r"\s+", cc, re.M):
        raise SystemExit(f"FAIL: freestanding lwIP errno {name} is not provided by arch/cc.h")
if not re.search(r"^extern\s+int\s+errno\s*;", cc, re.M):
    raise SystemExit("FAIL: freestanding lwIP errno variable is not declared")
if not re.search(r"^int\s+errno\s*;", sys_arch, re.M):
    raise SystemExit("FAIL: freestanding lwIP errno storage is not defined")
if re.search(r"^typedef\s+.*\b(?:s8_t|u8_t|s16_t|u16_t|s32_t|u32_t|s64_t|u64_t|mem_ptr_t)\s*;", cc, re.M):
    raise SystemExit("FAIL: cc.h redeclares types owned by lwIP arch.h")
if not re.search(r"static\s+inline\s+uint32_t\s+sv_lwip_rand\s*\(\s*void\s*\)", cc):
    raise SystemExit("FAIL: lwIP random hook uses a type available before arch.h typedefs")
if not re.search(r"#include\s+[<\"]netif/ethernet\.h[>\"]", adapter):
    raise SystemExit("FAIL: kernel adapter must include netif/ethernet.h")
if re.search(r"#include\s+[<\"]lwip/netif/ethernet\.h[>\"]", adapter):
    raise SystemExit("FAIL: obsolete Ethernet header path remains")
if not re.search(r"#include\s+[<\"]nt/Ntstatus\.h[>\"]", adapter):
    raise SystemExit("FAIL: Lwip.c must include NTSTATUS definitions for STATUS_TIMEOUT")
if "STATUS_TIMEOUT" in adapter and "KeWaitForSingleObjectTimeout" not in adapter:
    raise SystemExit("FAIL: timeout status check is detached from the kernel wait API")
if "STATUS_TIMEOUT" in sys_arch and not re.search(r"#include\s+[<\"]nt/Ntstatus\.h[>\"]", sys_arch):
    raise SystemExit("FAIL: LwipSysArch.c must include NTSTATUS definitions for dispatcher timeout results")
if '"include/netif/ethernet.h"' not in stager:
    raise SystemExit("FAIL: staging validation does not check the Ethernet header")

print("PASS: lwIP native 127.0.0.1 loopback and tcpip_thread delivery enabled")
print("PASS: lwIP target fd_set/timeval declarations and private timeval disabled")
print("PASS: lwIP stdint typedef ownership")
print("PASS: lwIP inttypes format macros enabled")
print("PASS: lwIP limits definitions enabled")
print("PASS: freestanding lwIP errno constants and storage")
print("PASS: lwIP Ethernet public header path")
print("PASS: lwIP random hook declaration order")
print("PASS: kernel timeout status include")
print("PASS: sys_arch timeout status include")
print("PASS: staging validates required headers")

if "__ctype_b_loc" in ctype or "__ctype_b_loc" in stdlib:
    raise SystemExit("FAIL: freestanding headers must not reference glibc ctype internals")
if not re.search(r"static\s+inline\s+int\s+isdigit", ctype):
    raise SystemExit("FAIL: freestanding ctype.h does not provide isdigit")
if not re.search(r"static\s+inline\s+int\s+atoi", stdlib):
    raise SystemExit("FAIL: freestanding stdlib.h does not provide atoi")
if not re.search(r"long\s+strtol\s*\(", string_impl):
    raise SystemExit("FAIL: kernel runtime does not implement strtol")
if re.search(r"^void\s+sys_msleep\s*\(", sys_arch, re.M):
    raise SystemExit("FAIL: sys_msleep must be provided by lwIP core, not the OS port")
if not re.search(r"^void\s+sys_arch_msleep\s*\(", sys_arch, re.M):
    raise SystemExit("FAIL: OS port must implement sys_arch_msleep")
if not re.search(r"char\s*\*\s*strncpy\s*\(", string_impl):
    raise SystemExit("FAIL: kernel runtime does not implement strncpy")
string_header = (ROOT / "kernel/include/string.h").read_text(encoding="utf-8")
if not re.search(r"strncpy\s*\(", string_header):
    raise SystemExit("FAIL: freestanding string.h does not declare strncpy")
print("PASS: freestanding ctype/stdlib headers and strtol runtime")
print("PASS: lwIP sys_arch_msleep ownership")
print("PASS: freestanding strncpy declaration and implementation")
