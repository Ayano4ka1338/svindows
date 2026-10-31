#include "netutil.h"

static size_t append(char *out, size_t position, size_t capacity,
                     const char *text)
{
    while (*text && position + 1 < capacity) out[position++] = *text++;
    out[position] = 0;
    return position;
}

static size_t append_port(char *out, size_t position, unsigned port)
{
    char digits[5]; unsigned count = 0;
    do { digits[count++] = (char)('0' + port % 10u); port /= 10u; }
    while (port && count < sizeof(digits));
    while (count) out[position++] = digits[--count];
    out[position] = 0;
    return position;
}

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    int head_only = 0;
    const char *url = 0;
    for (int i = 1; i < argc; i++) {
        if (sv_streq(argv[i], "-I")) head_only = 1;
        else if (argv[i][0] == '-') {
            sv_write_all(2, "curl: unsupported option\n", 25);
            return 2;
        } else url = argv[i];
    }
    if (!url) {
        sv_write_all(2, "usage: curl [-I] http://host[:port]/path\n", 41);
        return 2;
    }
    const char *host_start = url;
    if (url[0] == 'h' && url[1] == 't' && url[2] == 't' &&
        url[3] == 'p' && url[4] == ':' && url[5] == '/' && url[6] == '/')
        host_start = url + 7;
    else {
        sv_write_all(2, "curl: this build supports http:// URLs only (no TLS)\n", 53);
        return 2;
    }
    char host[256]; size_t host_length = 0;
    const char *cursor = host_start;
    while (*cursor && *cursor != '/' && *cursor != ':' && host_length + 1 < sizeof(host))
        host[host_length++] = *cursor++;
    host[host_length] = 0;
    if (!host_length || (*cursor && *cursor != '/' && *cursor != ':'))
        return 2;
    unsigned port = 80;
    if (*cursor == ':') {
        cursor++; port = 0;
        if (*cursor < '0' || *cursor > '9') return 2;
        while (*cursor >= '0' && *cursor <= '9') {
            port = port * 10u + (unsigned)(*cursor++ - '0');
            if (port > 65535u) return 2;
        }
        if (!port) return 2;
    }
    char request_path[1024];
    if (*cursor == '/') {
        sv_copy(request_path, cursor, sizeof(request_path));
    } else if (*cursor == '?') {
        request_path[0] = '/';
        sv_copy(request_path + 1, cursor, sizeof(request_path) - 1);
    } else {
        request_path[0] = '/';
        request_path[1] = 0;
    }
    if (sv_strlen(request_path) + host_length + 256 >= 1200) {
        sv_write_all(2, "curl: URL is too long\n", 22);
        return 2;
    }
    const char *path = request_path;

    uint8_t address[4];
    if (!sv_resolve_ipv4(host, address)) {
        sv_write_all(2, "curl: could not resolve host\n", 29);
        return 6;
    }
    long fd = sv_syscall3(SYS_SOCKET, AF_INET_VALUE, SOCK_STREAM_VALUE, 6);
    if (fd < 0) {
        sv_write_all(2, "curl: socket creation failed\n", 29);
        return 7;
    }
    struct sv_sockaddr_in target = {0};
    target.family = AF_INET_VALUE;
    target.port[0] = (uint8_t)(port >> 8);
    target.port[1] = (uint8_t)port;
    for (unsigned i = 0; i < 4; i++) target.address[i] = address[i];
    if (sv_syscall3(SYS_CONNECT, fd, (long)&target, sizeof(target)) < 0) {
        sv_write_all(2, "curl: connection failed\n", 24);
        sv_syscall3(SYS_CLOSE, fd, 0, 0);
        return 7;
    }

    char request[1200] = {0}; size_t used = 0;
    used = append(request, used, sizeof(request), head_only ? "HEAD " : "GET ");
    used = append(request, used, sizeof(request), path);
    used = append(request, used, sizeof(request), " HTTP/1.0\r\nHost: ");
    used = append(request, used, sizeof(request), host);
    if (port != 80) {
        used = append(request, used, sizeof(request), ":");
        used = append_port(request, used, port);
    }
    used = append(request, used, sizeof(request), "\r\nUser-Agent: Svindows-curl/0.1\r\nConnection: close\r\n\r\n");

    size_t sent_total = 0;
    while (sent_total < used) {
        long sent = sv_syscall3(SYS_WRITE, fd, (long)(request + sent_total),
                                (long)(used - sent_total));
        if (sent <= 0) {
            sv_write_all(2, "curl: request write failed\n", 27);
            sv_syscall3(SYS_CLOSE, fd, 0, 0);
            return 7;
        }
        sent_total += (size_t)sent;
    }
    uint8_t buffer[4096];
    for (;;) {
        long received = sv_syscall3(SYS_READ, fd, (long)buffer, sizeof(buffer));
        if (received == 0) break;
        if (received < 0) {
            sv_write_all(2, "curl: response read failed\n", 27);
            sv_syscall3(SYS_CLOSE, fd, 0, 0);
            return 56;
        }
        sv_write_all(1, (const char *)buffer, (size_t)received);
    }
    sv_syscall3(SYS_CLOSE, fd, 0, 0);
    return 0;
}
