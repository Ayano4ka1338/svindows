#ifndef SVINDOWS_NETUTIL_H
#define SVINDOWS_NETUTIL_H

#include <stdint.h>
#include <stddef.h>

#define SYS_READ       0
#define SYS_OPEN       2
#define SYS_WRITE      1
#define SYS_CLOSE      3
#define SYS_POLL       7
#define SYS_SOCKET     41
#define SYS_CONNECT    42
#define SYS_SENDTO     44
#define SYS_RECVFROM   45
#define SYS_GETPID     39
#define SYS_EXIT_GROUP 231

#define AF_INET_VALUE       2
#define SOCK_STREAM_VALUE   1
#define SOCK_DGRAM_VALUE   2
#define SOCK_RAW_VALUE     3
#define IPPROTO_ICMP_VALUE 1
#define IPPROTO_UDP_VALUE 17
#define POLLIN_VALUE       1

struct sv_sockaddr_in {
    uint16_t family;
    uint8_t port[2];
    uint8_t address[4];
    uint8_t zero[8];
};

struct sv_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

static inline long sv_syscall3(long number, long a, long b, long c)
{
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a),
                     "S"(b), "d"(c) : "rcx", "r11", "memory");
    return result;
}

static inline long sv_syscall6(long number, long a, long b, long c,
                               long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a),
                     "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) :
                     "rcx", "r11", "memory");
    return result;
}

static inline long sv_write(int fd, const void *data, size_t length)
{
    return sv_syscall3(SYS_WRITE, fd, (long)data, (long)length);
}

static inline void sv_write_all(int fd, const char *data, size_t length)
{
    while (length) {
        long written = sv_write(fd, data, length);
        if (written <= 0) return;
        data += (size_t)written;
        length -= (size_t)written;
    }
}

static inline size_t sv_strlen(const char *text)
{
    size_t length = 0;
    while (text[length]) length++;
    return length;
}

static inline int sv_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static inline void sv_copy(char *destination, const char *source, size_t cap)
{
    size_t i = 0;
    if (!cap) return;
    while (i + 1 < cap && source[i]) {
        destination[i] = source[i];
        i++;
    }
    destination[i] = 0;
}

static inline int sv_parse_ipv4(const char *text, uint8_t out[4])
{
    for (unsigned part = 0; part < 4; part++) {
        unsigned value = 0, digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10u + (unsigned)(*text - '0');
            if (value > 255u) return 0;
            text++;
            digits++;
        }
        if (!digits) return 0;
        out[part] = (uint8_t)value;
        if (part != 3) {
            if (*text != '.') return 0;
            text++;
        } else if (*text) return 0;
    }
    return 1;
}

static inline void sv_format_ipv4(const uint8_t ip[4], char out[16])
{
    char reverse[4][3];
    unsigned lengths[4];
    for (unsigned i = 0; i < 4; i++) {
        unsigned value = ip[i], n = 0;
        do { reverse[i][n++] = (char)('0' + value % 10u); value /= 10u; }
        while (value && n < 3);
        lengths[i] = n;
    }
    unsigned p = 0;
    for (unsigned i = 0; i < 4; i++) {
        while (lengths[i]) out[p++] = reverse[i][--lengths[i]];
        if (i != 3) out[p++] = '.';
    }
    out[p] = 0;
}

static inline int sv_skip_dns_name(const uint8_t *packet, size_t size,
                                   size_t *position)
{
    size_t p = *position;
    while (p < size) {
        uint8_t length = packet[p++];
        if (!length) { *position = p; return 1; }
        if ((length & 0xc0u) == 0xc0u) {
            if (p >= size) return 0;
            *position = p + 1;
            return 1;
        }
        if (length & 0xc0u || length > 63u || p + length > size)
            return 0;
        p += length;
    }
    return 0;
}

/* Read the configured DNS resolver. QEMU user networking defaults to
 * 10.0.2.3; etc/resolv.conf in the disk image provides that default and can
 * be replaced for a bridged or physical network. */
static inline void sv_dns_server(uint8_t server[4])
{
    server[0] = 10; server[1] = 0; server[2] = 2; server[3] = 3;
    static const char path[] = "/etc/resolv.conf";
    long fd = sv_syscall3(SYS_OPEN, (long)path, 0, 0);
    if (fd < 0) return;
    char contents[256];
    long count = sv_syscall3(SYS_READ, fd, (long)contents,
                             sizeof(contents) - 1);
    sv_syscall3(SYS_CLOSE, fd, 0, 0);
    if (count <= 0) return;
    contents[count] = 0;
    for (size_t i = 0; i + 10 < (size_t)count; i++) {
        if (contents[i] != 'n' || contents[i + 1] != 'a' ||
            contents[i + 2] != 'm' || contents[i + 3] != 'e' ||
            contents[i + 4] != 's' || contents[i + 5] != 'e' ||
            contents[i + 6] != 'r' || contents[i + 7] != 'v' ||
            contents[i + 8] != 'e' || contents[i + 9] != 'r')
            continue;
        size_t p = i + 10;
        while (p < (size_t)count && (contents[p] == ' ' || contents[p] == '\t')) p++;
        char numeric[16]; size_t n = 0;
        while (p < (size_t)count && n + 1 < sizeof(numeric) &&
               ((contents[p] >= '0' && contents[p] <= '9') || contents[p] == '.'))
            numeric[n++] = contents[p++];
        numeric[n] = 0;
        if (n && sv_parse_ipv4(numeric, server)) return;
    }
}

static inline int sv_dns_resolve(const char *hostname, uint8_t address[4])
{
    uint8_t query[512] = {0};
    query[0] = 0x53; query[1] = 0x56;
    query[2] = 0x01; query[5] = 0x01; /* RD, one question */
    size_t used = 12;
    const char *label = hostname;
    while (*label) {
        const char *end = label;
        size_t label_length = 0;
        while (*end && *end != '.') { end++; label_length++; }
        if (!label_length || label_length > 63 || used + label_length + 6 >= sizeof(query))
            return 0;
        query[used++] = (uint8_t)label_length;
        for (size_t i = 0; i < label_length; i++) {
            char c = label[i];
            query[used++] = (uint8_t)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        label = *end ? end + 1 : end;
    }
    query[used++] = 0;
    query[used++] = 0; query[used++] = 1; /* A */
    query[used++] = 0; query[used++] = 1; /* IN */

    long fd = sv_syscall3(SYS_SOCKET, AF_INET_VALUE, SOCK_DGRAM_VALUE,
                          IPPROTO_UDP_VALUE);
    if (fd < 0) return 0;
    struct sv_sockaddr_in dns = {0};
    dns.family = AF_INET_VALUE;
    dns.port[0] = 0; dns.port[1] = 53;
    sv_dns_server(dns.address);
    if (sv_syscall3(SYS_CONNECT, fd, (long)&dns, sizeof(dns)) < 0 ||
        sv_syscall6(SYS_SENDTO, fd, (long)query, (long)used, 0, 0, 0) < 0) {
        sv_syscall3(SYS_CLOSE, fd, 0, 0);
        return 0;
    }
    struct sv_pollfd pollfd = {(int32_t)fd, POLLIN_VALUE, 0};
    if (sv_syscall3(SYS_POLL, (long)&pollfd, 1, 2500) <= 0 ||
        !(pollfd.revents & POLLIN_VALUE)) {
        sv_syscall3(SYS_CLOSE, fd, 0, 0);
        return 0;
    }
    uint8_t response[512];
    long received = sv_syscall6(SYS_RECVFROM, fd, (long)response,
                                sizeof(response), 0, 0, 0);
    sv_syscall3(SYS_CLOSE, fd, 0, 0);
    if (received < 12 || response[0] != query[0] ||
        response[1] != query[1] || (response[2] & 0x80u) == 0 ||
        (response[3] & 0x0fu) != 0)
        return 0;

    size_t position = 12;
    if (!sv_skip_dns_name(response, (size_t)received, &position) ||
        position + 4 > (size_t)received)
        return 0;
    position += 4;
    unsigned answers = ((unsigned)response[6] << 8) | response[7];
    for (unsigned i = 0; i < answers; i++) {
        if (!sv_skip_dns_name(response, (size_t)received, &position) ||
            position + 10 > (size_t)received)
            return 0;
        unsigned type = ((unsigned)response[position] << 8) | response[position + 1];
        unsigned klass = ((unsigned)response[position + 2] << 8) | response[position + 3];
        unsigned rdlength = ((unsigned)response[position + 8] << 8) | response[position + 9];
        position += 10;
        if (position + rdlength > (size_t)received) return 0;
        if (type == 1 && klass == 1 && rdlength == 4) {
            address[0] = response[position];
            address[1] = response[position + 1];
            address[2] = response[position + 2];
            address[3] = response[position + 3];
            return 1;
        }
        position += rdlength;
    }
    return 0;
}

static inline int sv_resolve_ipv4(const char *host, uint8_t address[4])
{
    if (sv_parse_ipv4(host, address)) return 1;
    return sv_dns_resolve(host, address);
}

static inline void sv_exit(int status)
{
    sv_syscall3(SYS_EXIT_GROUP, status, 0, 0);
    for (;;) __asm__ volatile("hlt");
}

extern int main(int argc, char **argv, char **envp);

__attribute__((naked, noreturn, used)) void _start(void)
{
    __asm__ volatile(
        "xor %rbp, %rbp\n"
        "mov (%rsp), %rdi\n"
        "lea 8(%rsp), %rsi\n"
        "lea 16(%rsp,%rdi,8), %rdx\n"
        "and $-16, %rsp\n"
        "call main\n"
        "mov %eax, %edi\n"
        "mov $231, %eax\n"
        "syscall\n"
        "1: jmp 1b\n");
}

#endif
