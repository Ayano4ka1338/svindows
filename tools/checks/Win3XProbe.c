/* Native Linux-ABI X11 client used only by Win3XVisualSmoke.py. */
#include <stdint.h>

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static void delay(unsigned ms) {
    uint64_t t[2] = {ms / 1000, (ms % 1000) * 1000000ULL};
    sc(35, (long)t, 0, 0);
}
static char log_buffer[512];
static unsigned log_length;
static int buffering;
static void say(const char *s) {
    unsigned n = 0;
    while (s[n])
        n++;
    if (buffering) {
        for (unsigned i = 0; i < n && log_length < sizeof(log_buffer); i++)
            log_buffer[log_length++] = s[i];
    } else
        sc(1, 1, (long)s, n);
}
static void number(unsigned n) {
    char b[12];
    unsigned at = sizeof(b);
    do {
        b[--at] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    if (buffering) {
        for (unsigned i = at; i < sizeof(b) && log_length < sizeof(log_buffer); i++)
            log_buffer[log_length++] = b[i];
    } else
        sc(1, 1, (long)(b + at), sizeof(b) - at);
}
static void die(const char *why) {
    say("[win3x-check] FAIL ");
    say(why);
    say("\n");
    sc(60, 2, 0, 0);
    for (;;) {
    }
}
static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }
static void p16(uint8_t *p, unsigned n) {
    p[0] = n;
    p[1] = n >> 8;
}
static void p32(uint8_t *p, unsigned n) {
    p16(p, n);
    p16(p + 2, n >> 16);
}
static int fd;
static uint8_t reply[8192];
static int io(int write, void *data, unsigned size) {
    uint8_t *p = data;
    while (size) {
        long n = sc(write ? 1 : 0, fd, (long)p, size);
        if (n <= 0)
            return 0;
        p += n;
        size -= n;
    }
    return 1;
}
static int response(void) {
    for (;;) {
        if (!io(0, reply, 32))
            die("X socket closed");
        if (reply[0] == 0)
            return 0;
        if (reply[0] != 1)
            continue; /* MappingNotify is not a reply. */
        unsigned size = u32(reply + 4) * 4;
        if (size > sizeof(reply) - 32)
            die("oversized X reply");
        if (size && !io(0, reply + 32, size))
            die("short X reply");
        return 1;
    }
}
static int request(uint8_t *data, unsigned size) {
    if (!io(1, data, size))
        die("X write");
    return response();
}
static uint32_t root, protocol, delete_atom;
static uint32_t intern(const char *name, unsigned len) {
    uint8_t r[64] = {16, 0, 0, 0};
    p16(r + 2, (8 + len + 3) / 4);
    p16(r + 4, len);
    for (unsigned i = 0; i < len; i++)
        r[8 + i] = name[i];
    if (!request(r, ((8 + len + 3) / 4) * 4))
        die("InternAtom");
    return u32(reply + 8);
}
static int property(uint32_t win, unsigned atom, char *out, unsigned capacity) {
    uint8_t r[24] = {20, 0, 6, 0};
    p32(r + 4, win);
    p32(r + 8, atom);
    p32(r + 20, 64);
    if (!request(r, sizeof(r)) || reply[1] != 8)
        return 0;
    unsigned n = u32(reply + 16);
    if (n >= capacity)
        n = capacity - 1;
    for (unsigned i = 0; i < n; i++)
        out[i] = (char)reply[32 + i];
    out[n] = 0;
    return n > 0;
}
static int prefix(const char *s, const char *p) {
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}
typedef struct {
    uint32_t id;
    unsigned width, height;
} WINDOW;
static WINDOW calc, clock_window;
static void find_windows(void) {
    uint32_t queue[256];
    unsigned begin = 0, end = 1;
    queue[0] = root;
    calc.id = clock_window.id = 0;
    while (begin < end) {
        uint32_t win = queue[begin++];
        char klass[64], title[128];
        if (property(win, 67, klass, sizeof(klass)) && prefix(klass, "svdos") &&
            property(win, 39, title, sizeof(title))) {
            uint8_t r[8] = {14, 0, 2, 0};
            p32(r + 4, win);
            if (request(r, sizeof(r))) {
                WINDOW w = {win, u16(reply + 16), u16(reply + 18)};
                if (w.width > 100 && w.height > 100) {
                    if (prefix(title, "Alarm Clock"))
                        clock_window = w;
                    if (prefix(title, "Calc") || prefix(title, "\xca\xe0\xeb\xfc"))
                        calc = w;
                }
            }
        }
        uint8_t q[8] = {15, 0, 2, 0};
        p32(q + 4, win);
        if (!request(q, sizeof(q)))
            continue;
        unsigned n = u16(reply + 16);
        for (unsigned i = 0; i < n && end < 256; i++)
            queue[end++] = u32(reply + 32 + i * 4);
    }
}
static int root_position(uint32_t win, int *x, int *y) {
    uint8_t r[16] = {40, 0, 4, 0};
    p32(r + 4, win);
    p32(r + 8, root);
    if (!request(r, sizeof(r)))
        return 0;
    *x = (int16_t)u16(reply + 12);
    *y = (int16_t)u16(reply + 14);
    return 1;
}
static uint32_t display_hash(uint32_t win, unsigned width) {
    uint8_t r[20] = {73, 2, 5, 0};
    p32(r + 4, win);
    p16(r + 8, width - 90);
    p16(r + 10, 45);
    p16(r + 12, 80);
    p16(r + 14, 18);
    p32(r + 16, ~0u);
    if (!request(r, sizeof(r)))
        die("GetImage");
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < u32(reply + 4) * 4; i++)
        hash = (hash ^ reply[32 + i]) * 16777619u;
    return hash;
}
static void keyboard_check(void) {
    uint8_t focus[12] = {42, 1, 3, 0};
    p32(focus + 4, calc.id);
    if (!io(1, focus, sizeof(focus)))
        die("focus");
    delay(1000);
    uint32_t before = display_hash(calc.id, calc.width);
    for (unsigned up = 0; up < 2; up++) {
        uint8_t key[44] = {25, 0, 11, 0};
        p32(key + 4, calc.id);
        p32(key + 8, up ? 2 : 1);
        key[12] = up ? 3 : 2;
        key[13] = 14; /* evdev keycode for 5 */
        p32(key + 20, root);
        p32(key + 24, calc.id);
        key[42] = 1;
        if (!io(1, key, sizeof(key)))
            die("key");
    }
    for (unsigned i = 0; i < 100; i++) {
        delay(100);
        if (display_hash(calc.id, calc.width) != before) {
            say("[win3x-check] keyboard delivered\n");
            return;
        }
    }
    die("Calculator did not receive keyboard input");
}
__attribute__((force_align_arg_pointer)) void _start(void) {
    static const struct {
        uint16_t family;
        char path[sizeof("\0/tmp/.X11-unix/X0")];
    } addr = {1, "\0/tmp/.X11-unix/X0"};
    fd = (int)sc(41, 1, 1, 0);
    if (fd < 0)
        die("socket");
    if (sc(42, fd, (long)&addr, 2 + sizeof(addr.path) - 1))
        die("connect");
    uint8_t hello[12] = {'l', 0, 11}, prefix_reply[8];
    if (!io(1, hello, 12) || !io(0, prefix_reply, 8) || prefix_reply[0] != 1)
        die("X setup");
    unsigned size = u16(prefix_reply + 6) * 4;
    if (size > sizeof(reply) || !io(0, reply, size))
        die("X setup length");
    unsigned at = 32 + ((u16(reply + 16) + 3) & ~3u) + (unsigned)reply[21] * 8;
    if (at + 40 > size)
        die("X root");
    root = u32(reply + at);
    protocol = intern("WM_PROTOCOLS", 12);
    delete_atom = intern("WM_DELETE_WINDOW", 16);
    for (unsigned i = 0; i < 2400; i++) {
        find_windows();
        if (calc.id && clock_window.id)
            break;
        delay(100);
    }
    if (!calc.id || !clock_window.id || calc.id == clock_window.id)
        die("two Win16 clients not found");
    delay(3000); /* Let the first independent snapshots finish. */
    uint8_t move[20] = {12, 0, 5, 0};
    p32(move + 4, calc.id);
    p16(move + 8, 3);
    p32(move + 12, 850);
    p32(move + 16, 100);
    if (!io(1, move, sizeof(move)))
        die("move");
    int x = 0, y = 0;
    for (unsigned i = 0; i < 100; i++) {
        delay(100);
        if (root_position(calc.id, &x, &y) && x >= 700)
            break;
    }
    if (x < 700)
        die("host move was not retained");
    delay(2000);
    keyboard_check();
    buffering = 1;
    log_length = 0;
    say("[win3x-check] READY x=");
    number(x);
    say(" y=");
    number(y);
    say(" w=");
    number(calc.width);
    say(" h=");
    number(calc.height);
    say(" calc=");
    number(calc.id);
    say(" clock=");
    number(clock_window.id);
    say("\n");
    buffering = 0;
    sc(1, 1, (long)log_buffer, log_length);
    delay(8000); /* The host captures the moved client before closing Clock. */
    uint32_t saved_calc = calc.id;
    uint8_t close[44] = {25, 0, 11, 0};
    p32(close + 4, clock_window.id);
    close[12] = 33;
    close[13] = 32;
    p32(close + 16, clock_window.id);
    p32(close + 20, protocol);
    p32(close + 24, delete_atom);
    if (!io(1, close, sizeof(close)))
        die("close");
    for (unsigned i = 0; i < 150; i++) {
        delay(100);
        find_windows();
        if (!clock_window.id && calc.id == saved_calc)
            break;
    }
    if (clock_window.id || calc.id != saved_calc)
        die("close did not preserve Calculator");
    say("[win3x-check] PASS independent windows, move and close\n");
    sc(60, 0, 0, 0);
    for (;;) {
    }
}
