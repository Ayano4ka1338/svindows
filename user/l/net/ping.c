#include "netutil.h"

static uint16_t checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < length; i += 2)
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
    if (length & 1u) sum += (uint32_t)data[length - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    unsigned count = 4;
    const char *host = 0;
    for (int i = 1; i < argc; i++) {
        if (sv_streq(argv[i], "-c") && i + 1 < argc) {
            count = 0;
            const char *number = argv[++i];
            while (*number >= '0' && *number <= '9') {
                unsigned digit = (unsigned)(*number - '0');
                if (count > 10u || (count == 10u && digit != 0u)) {
                    count = 101u;
                    break;
                }
                count = count * 10u + digit;
                number++;
            }
            if (*number || !count || count > 100) {
                sv_write_all(2, "ping: invalid count\n", 20);
                return 2;
            }
        } else if (!host) host = argv[i];
        else {
            sv_write_all(2, "usage: ping [-c count] host\n", 28);
            return 2;
        }
    }
    if (!host) {
        sv_write_all(2, "usage: ping [-c count] host\n", 28);
        return 2;
    }

    uint8_t destination[4];
    if (!sv_resolve_ipv4(host, destination)) {
        sv_write_all(2, "ping: cannot resolve host\n", 26);
        return 1;
    }
    char ip_text[16];
    sv_format_ipv4(destination, ip_text);
    sv_write_all(1, "PING ", 5);
    sv_write_all(1, host, sv_strlen(host));
    sv_write_all(1, " (", 2);
    sv_write_all(1, ip_text, sv_strlen(ip_text));
    sv_write_all(1, ") 56 data bytes\n", 16);

    long fd = sv_syscall3(SYS_SOCKET, AF_INET_VALUE, SOCK_RAW_VALUE,
                          IPPROTO_ICMP_VALUE);
    if (fd < 0) {
        unsigned error = (unsigned)(-fd);
        char digits[5];
        unsigned n = 0;
        do { digits[n++] = (char)('0' + error % 10u); error /= 10u; }
        while (error && n < sizeof(digits));
        sv_write_all(2, "ping: raw ICMP socket unavailable (errno=", 41);
        while (n) sv_write_all(2, &digits[--n], 1);
        sv_write_all(2, ")\n", 2);
        return 1;
    }
    struct sv_sockaddr_in target = {0};
    target.family = AF_INET_VALUE;
    for (unsigned i = 0; i < 4; i++) target.address[i] = destination[i];
    unsigned received_count = 0;
    unsigned transmitted_count = 0;
    unsigned identifier = (unsigned)sv_syscall3(SYS_GETPID, 0, 0, 0) & 0xffffu;
    for (unsigned sequence = 1; sequence <= count; sequence++) {
        uint8_t packet[64] = {0};
        packet[0] = 8; /* ICMP echo request */
        packet[4] = (uint8_t)(identifier >> 8);
        packet[5] = (uint8_t)identifier;
        packet[6] = (uint8_t)(sequence >> 8);
        packet[7] = (uint8_t)sequence;
        for (unsigned i = 8; i < sizeof(packet); i++)
            packet[i] = (uint8_t)(i + sequence);
        uint16_t sum = checksum(packet, sizeof(packet));
        packet[2] = (uint8_t)(sum >> 8);
        packet[3] = (uint8_t)sum;
        long sent = sv_syscall6(SYS_SENDTO, fd, (long)packet,
            sizeof(packet), 0, (long)&target, sizeof(target));
        if (sent < 0) {
            sv_write_all(2, "ping: send failed\n", 18);
            break;
        }
        transmitted_count++;

        struct sv_pollfd pollfd = {(int32_t)fd, POLLIN_VALUE, 0};
        long ready = sv_syscall3(SYS_POLL, (long)&pollfd, 1, 1000);
        if (ready <= 0 || !(pollfd.revents & POLLIN_VALUE)) {
            sv_write_all(1, "Request timed out.\n", 19);
            continue;
        }
        uint8_t reply[256];
        long length = sv_syscall6(SYS_RECVFROM, fd, (long)reply,
                                   sizeof(reply), 0, 0, 0);
        if (length < 8) {
            sv_write_all(1, "Short ICMP reply.\n", 18);
            continue;
        }
        size_t offset = 0;
        if ((reply[0] >> 4) == 4) {
            offset = (size_t)(reply[0] & 15u) * 4u;
            if (offset + 8 > (size_t)length) continue;
        }
        if (reply[offset] != 0 || reply[offset + 1] != 0 ||
            reply[offset + 4] != (uint8_t)(identifier >> 8) ||
            reply[offset + 5] != (uint8_t)identifier ||
            reply[offset + 6] != (uint8_t)(sequence >> 8) ||
            reply[offset + 7] != (uint8_t)sequence)
            continue;
        received_count++;
        sv_write_all(1, "64 bytes from ", 14);
        sv_write_all(1, ip_text, sv_strlen(ip_text));
        sv_write_all(1, ": icmp_seq=", 11);
        char digits[4]; unsigned n = 0, value = sequence;
        do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value);
        while (n) sv_write_all(1, &digits[--n], 1);
        sv_write_all(1, " ttl=?\n", 7);
    }
    sv_syscall3(SYS_CLOSE, fd, 0, 0);
    sv_write_all(1, "\n--- ", 5);
    sv_write_all(1, host, sv_strlen(host));
    sv_write_all(1, " ping statistics ---\n", 21);
    /* Minimal decimal output, keeping this utility libc-independent. */
    char a[4], b[4]; unsigned an = 0, bn = 0;
    unsigned x = transmitted_count, y = received_count;
    do { a[an++] = (char)('0' + x % 10u); x /= 10u; } while (x);
    do { b[bn++] = (char)('0' + y % 10u); y /= 10u; } while (y);
    while (an) sv_write_all(1, &a[--an], 1);
    sv_write_all(1, " packets transmitted, ", 22);
    while (bn) sv_write_all(1, &b[--bn], 1);
    sv_write_all(1, " received\n", 10);
    return received_count ? 0 : 1;
}
