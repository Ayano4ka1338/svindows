/* Start an Xorg server, wait for its X11 setup reply, then run TWM. */
#include <stdint.h>

/* Keep one client connected while short-lived setup clients exit. Xorg
 * resets the root window when its last client disconnects. */
static int x11_hold_fd = -1;

static long linux_syscall3(long number, long a, long b, long c)
{
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a),
                     "S"(b), "d"(c) : "rcx", "r11", "memory");
    return result;
}

static long linux_syscall0(long number)
{
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number) :
                     "rcx", "r11", "memory");
    return result;
}

static long linux_syscall4(long number, long a, long b, long c, long d)
{
    register long r10 __asm__("r10") = d;
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a),
                     "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return result;
}

static void pause_ms(unsigned milliseconds)
{
    uint64_t duration[2] = {
        milliseconds / 1000,
        (milliseconds % 1000) * 1000000ULL
    };
    linux_syscall3(35, (long)duration, 0, 0); /* nanosleep */
}

static int write_all(int fd, const char *text, unsigned length)
{
    while (length) {
        long written = linux_syscall3(1, fd, (long)text, length);
        if (written <= 0) return -1;
        text += written;
        length -= (unsigned)written;
    }
    return 0;
}

static int read_all(int fd, uint8_t *buffer, unsigned length)
{
    while (length) {
        long received = linux_syscall3(0, fd, (long)buffer, length);
        if (received <= 0) return -1;
        buffer += received;
        length -= (unsigned)received;
    }
    return 0;
}

static int x11_server_ready(void)
{
    if (x11_hold_fd >= 0) return 1;
    static const struct {
        uint16_t family;
        char path[sizeof("\0/tmp/.X11-unix/X0")];
    } address = {1, "\0/tmp/.X11-unix/X0"};
    static const uint8_t setup_request[12] = {
        'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    int fd = (int)linux_syscall3(41, 1, 1, 0); /* AF_UNIX, SOCK_STREAM */
    if (fd < 0) return 0;
    if (linux_syscall3(42, fd, (long)&address,
                       2 + sizeof("\0/tmp/.X11-unix/X0") - 1) != 0) {
        linux_syscall3(3, fd, 0, 0);
        return 0;
    }

    uint8_t prefix[8] = {0};
    uint8_t body[8192];
    if (write_all(fd, (const char *)setup_request,
                  sizeof(setup_request)) < 0 ||
        read_all(fd, prefix, sizeof(prefix)) < 0 || prefix[0] != 1) {
        linux_syscall3(3, fd, 0, 0);
        return 0;
    }
    unsigned body_size = ((unsigned)prefix[6] |
                          (unsigned)prefix[7] << 8) * 4;
    if (body_size > sizeof(body) || read_all(fd, body, body_size) < 0) {
        linux_syscall3(3, fd, 0, 0);
        return 0;
    }
    x11_hold_fd = fd;
    return 1;
}

static long start_xorg(void)
{
    long pid = linux_syscall0(57); /* fork */
    if (pid != 0) return pid;

    static const char path[] = "/usr/bin/xorg";
    static const char display[] = ":0";
    static const char terminal[] = "vt2";
    static const char no_listen[] = "-nolisten";
    static const char tcp[] = "tcp";
    static const char unix_transport[] = "unix";
    static const char no_lock[] = "-nolock";
    static const char no_access_control[] = "-ac";
    static const char log_verbose[] = "-logverbose";
    static const char log_level[] = "0";
    static const char *const arguments[] = {
        path, display, terminal, no_listen, tcp, no_listen, unix_transport,
        no_lock, no_access_control, log_verbose, log_level, 0
    };
    linux_syscall3(59, (long)path, (long)arguments, 0); /* execve */
    linux_syscall3(60, 127, 0, 0); /* exit */
    __builtin_unreachable();
}

/* Setting the root background is cosmetic. Do not let a client which
 * stalls in Xlib/server I/O prevent the actual terminal and window manager
 * from starting. Reap xsetroot when it exits promptly, but cap the wait. */
static int set_root_pattern(void)
{
    long pid = linux_syscall0(57); /* fork */
    if (pid < 0) return -1;
    if (pid == 0) {
        if (x11_hold_fd >= 0) linux_syscall3(3, x11_hold_fd, 0, 0);
        static const char path[] = "/usr/bin/xsetroot";
        static const char display_option[] = "-display";
        static const char display[] = ":0";
        static const char pattern[] = "-mod";
        static const char step[] = "2";
        static const char foreground[] = "-fg";
        static const char ink[] = "#a0a0a0";
        static const char background[] = "-bg";
        static const char paper[] = "#c0c0c0";
        static const char failed[] = "startx: failed to execute xsetroot\n";
        static const char *const arguments[] = {
            path, display_option, display, pattern, step, step,
            foreground, ink, background, paper, 0
        };
        linux_syscall3(59, (long)path, (long)arguments, 0);
        write_all(2, failed, sizeof(failed) - 1);
        linux_syscall3(60, 127, 0, 0);
        __builtin_unreachable();
    }

    for (unsigned attempt = 0; attempt < 50; attempt++) {
        unsigned status = 0;
        long waited = linux_syscall4(61, pid, (long)&status, 1, 0);
        if (waited == pid) return status == 0 ? 0 : -1;
        if (waited < 0) return -1;
        pause_ms(10);
    }
    return 1; /* still running: let the rest of the desktop start */
}

/* Run xterm with cmd.exe inside: execve("/usr/bin/cmd") loads the PE image
 * through the kernel's Windows-personality switch, and the pty slave the
 * child inherits becomes its console via the char-device file bridge. */
static long start_xterm(void)
{
    long pid = linux_syscall0(57); /* fork */
    if (pid != 0) return pid;

    if (x11_hold_fd >= 0) linux_syscall3(3, x11_hold_fd, 0, 0);

    static const char path[] = "/usr/bin/xterm";
    /* Leave room for Win32 clients that were mapped during Xorg startup. */
    static const char geometry_option[] = "-geometry";
    static const char geometry[] = "80x24-0+0";
    static const char exec_option[] = "-e";
    static const char shell[] = "/usr/bin/cmd";
    static const char *const arguments[] = {
        path, geometry_option, geometry, exec_option, shell, 0
    };
    static const char display[] = "DISPLAY=:0";
    static const char home[] = "HOME=/";
    static const char search_path[] = "PATH=/usr/bin:/bin";
    static const char shell_env[] = "SHELL=/usr/bin/cmd";
    static const char *const environment[] = {
        display, home, search_path, shell_env, 0
    };
    linux_syscall3(59, (long)path, (long)arguments,
                   (long)environment); /* execve */
    linux_syscall3(60, 127, 0, 0);
    __builtin_unreachable();
}

static void start_twm(void)
{
    static const char path[] = "/usr/bin/twm";
    static const char option[] = "-f";
    static const char config[] = "/usr/share/X11/twm/system.twmrc";
    static const char *const arguments[] = {path, option, config, 0};
    static const char display[] = "DISPLAY=:0";
    static const char home[] = "HOME=/";
    static const char search_path[] = "PATH=/usr/bin:/bin";
    static const char *const environment[] = {
        display, home, search_path, 0
    };
    static const char error[] = "startx: failed to execute twm\n";
    linux_syscall3(59, (long)path, (long)arguments,
                   (long)environment); /* execve */
    write_all(2, error, sizeof(error) - 1);
    linux_syscall3(60, 1, 0, 0);
    __builtin_unreachable();
}

__attribute__((force_align_arg_pointer)) void _start(void)
{
    static const char starting[] = "startx: starting Xorg on :0\n";
    static const char waiting[] = "startx: waiting for Xorg\n";
    static const char unavailable[] = "startx: Xorg did not become ready\n";
    static const char ready[] = "startx: Xorg accepted X11 setup on :0\n";
    static const char pattern_failed[] = "startx: could not start root pattern helper\n";
    static const char pattern_slow[] = "startx: xsetroot is still running; continuing\n";
    static const char launching_xterm[] = "startx: launching xterm\n";
    static const char launching_twm[] = "startx: launching twm\n";
    static const char xterm_failed[] = "startx: could not start xterm\n";

    if (!x11_server_ready()) {
        write_all(1, starting, sizeof(starting) - 1);
        if (start_xorg() < 0) {
            write_all(2, unavailable, sizeof(unavailable) - 1);
            linux_syscall3(60, 1, 0, 0);
            __builtin_unreachable();
        }
        write_all(1, waiting, sizeof(waiting) - 1);
        unsigned attempt;
        for (attempt = 0; attempt < 2000; attempt++) {
            pause_ms(10);
            if (x11_server_ready()) break;
        }
        if (attempt == 2000) {
            write_all(2, unavailable, sizeof(unavailable) - 1);
            linux_syscall3(60, 1, 0, 0);
            __builtin_unreachable();
        }
    }
    write_all(1, ready, sizeof(ready) - 1);
    int pattern_result = set_root_pattern();
    if (pattern_result < 0)
        write_all(2, pattern_failed, sizeof(pattern_failed) - 1);
    else if (pattern_result > 0)
        write_all(2, pattern_slow, sizeof(pattern_slow) - 1);

    write_all(1, launching_xterm, sizeof(launching_xterm) - 1);
    if (start_xterm() < 0)
        write_all(2, xterm_failed, sizeof(xterm_failed) - 1);
    write_all(1, launching_twm, sizeof(launching_twm) - 1);
    start_twm();
}
