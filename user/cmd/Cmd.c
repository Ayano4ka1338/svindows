/*
 * Cmd.c - Svindows command processor.
 *
 * This is a normal console PE, not kernel code.  Commands which change shell
 * state or require shell syntax remain built in. Stateless commands are
 * separate console PE images implemented in Cmdutils.c.
 */
typedef void              *HANDLE;
typedef void              *LPVOID;
typedef unsigned char      BYTE;
typedef unsigned short     WORD;
typedef unsigned long      DWORD;
typedef unsigned long long ULONGLONG;
typedef long               NTSTATUS;
typedef int                BOOL;

#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INVALID_HANDLE_VALUE ((HANDLE)(ULONGLONG)-1)

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) HANDLE CreateFileA(const char *, DWORD, DWORD, LPVOID,
                                         DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL CloseHandle(HANDLE);
__declspec(dllimport) BOOL CreateProcessA(const char *, char *, LPVOID, LPVOID,
                                          BOOL, DWORD, LPVOID, const char *,
                                          LPVOID, LPVOID);
__declspec(dllimport) DWORD WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) BOOL GetExitCodeProcess(HANDLE, DWORD *);
__declspec(dllimport) BOOL CreatePipe(HANDLE *, HANDLE *, LPVOID, DWORD);
__declspec(dllimport) DWORD GetFileAttributesA(const char *);
__declspec(dllimport) DWORD SetFilePointer(HANDLE, long, long *, DWORD);

typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess, hThread;
    DWORD dwProcessId, dwThreadId;
} PROCESS_INFORMATION;

typedef struct _STARTUPINFOA {
    DWORD cb;
    char *reserved;
    char *desktop;
    char *title;
    DWORD x, y, x_size, y_size, x_chars, y_chars, fill, flags;
    WORD show_window, reserved2_size;
    BYTE *reserved2;
    HANDLE standard_input, standard_output, standard_error;
} STARTUPINFOA;

#define STARTF_USESTDHANDLES 0x00000100u

static HANDLE g_in, g_out, g_err;
static int g_echo = 1;
static DWORD g_errorlevel;
static int g_async_launch;
static PROCESS_INFORMATION g_launched_process;

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static void write_n(const char *s, unsigned n)
{
    DWORD done;
    WriteFile(g_out, s, n, &done, 0);
}

static void write_s(const char *s) { write_n(s, slen(s)); }

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static int same(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return *a == 0 && *b == 0;
}

static char *skip_spaces(char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static void trim_right(char *s)
{
    unsigned n = slen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static void write_u64(ULONGLONG value)
{
    char buf[32];
    unsigned pos = sizeof(buf);
    do {
        buf[--pos] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    write_n(buf + pos, (unsigned)sizeof(buf) - pos);
}

static int dispatch(char *line);
static int execute_line(char *line);
#include "ShellFeatures.inc"
#include "Syntax.inc"

static void copy_str(char *dst, const char *src)
{
    unsigned i = 0;
    while (src[i] && i + 1 < CMD_LINE_MAX) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Read one byte, blocking until one is available. */
static int read_byte(char *out)
{
    DWORD got = 0;
    while (!ReadFile(g_in, out, 1, &got, 0) || !got)
        ;
    return 1;
}

/* Repaint the edit line in place: carriage-return to the prompt, rewrite the
 * buffer, blank any stale tail, then move the caret back to `pos`. */
static void redraw_line(const char *prompt, const char *buf, unsigned n,
                        unsigned pos, unsigned *rendered)
{
    write_s("\r");
    write_s(prompt);
    write_n(buf, n);
    for (unsigned i = n; i < *rendered; i++)
        write_s(" ");
    *rendered = n;
    for (unsigned i = n; i > pos; i--)
        write_s("\b");
}

static void line_insert(char *out, unsigned *n, unsigned *pos, char c)
{
    for (unsigned i = *n; i > *pos; i--)
        out[i] = out[i - 1];
    out[*pos] = c;
    (*n)++;
    (*pos)++;
}

static int read_line(const char *prompt, char *out, unsigned capacity)
{
    unsigned n = 0, pos = 0, rendered = 0;
    int hist_pos = -1; /* -1 while editing a fresh line */
    static char draft[CMD_LINE_MAX];
    (void)capacity;
    out[0] = 0;
    draft[0] = 0;
    write_s(prompt);

    for (;;) {
        char c;
        if (!read_byte(&c))
            continue;

        if (c == 27) { /* ESC: an ANSI CSI sequence from arrows/Home/End/Del */
            char b;
            if (!read_byte(&b))
                continue;
            if (b != '[') {
                c = b; /* not a CSI sequence; reprocess the byte */
            } else {
                if (!read_byte(&b))
                    continue;
                if (b == 'A') { /* Up: older history */
                    if (g_history_count && hist_pos != 0) {
                        if (hist_pos < 0) {
                            out[n] = 0;
                            copy_str(draft, out);
                            hist_pos = (int)g_history_count;
                        }
                        hist_pos--;
                        copy_str(out, g_history[hist_pos]);
                        n = slen(out); pos = n;
                        redraw_line(prompt, out, n, pos, &rendered);
                    }
                } else if (b == 'B') { /* Down: newer history, then draft */
                    if (hist_pos >= 0) {
                        hist_pos++;
                        if (hist_pos >= (int)g_history_count) {
                            hist_pos = -1;
                            copy_str(out, draft);
                        } else {
                            copy_str(out, g_history[hist_pos]);
                        }
                        n = slen(out); pos = n;
                        redraw_line(prompt, out, n, pos, &rendered);
                    }
                } else if (b == 'C') { /* Right */
                    if (pos < n) { pos++; redraw_line(prompt, out, n, pos, &rendered); }
                } else if (b == 'D') { /* Left */
                    if (pos) { pos--; redraw_line(prompt, out, n, pos, &rendered); }
                } else if (b == 'H') { /* Home */
                    pos = 0; redraw_line(prompt, out, n, pos, &rendered);
                } else if (b == 'F') { /* End */
                    pos = n; redraw_line(prompt, out, n, pos, &rendered);
                } else if (b == '3') { /* Delete: ESC [ 3 ~ */
                    char tilde;
                    if (read_byte(&tilde) && tilde == '~' && pos < n) {
                        for (unsigned i = pos; i < n; i++)
                            out[i] = out[i + 1];
                        n--;
                        redraw_line(prompt, out, n, pos, &rendered);
                    }
                }
                continue;
            }
        }

        if (c == '\r')
            continue;
        if (c == '\n') {
            out[n] = 0;
            write_s("\r\n");
            return 1;
        }
        if (c == '\b') {
            if (pos) {
                for (unsigned i = pos - 1; i < n; i++)
                    out[i] = out[i + 1];
                pos--; n--;
                redraw_line(prompt, out, n, pos, &rendered);
            }
            continue;
        }
        if (c == '\t') {
            do {
                if (n + 1 < capacity) line_insert(out, &n, &pos, ' ');
            } while (n % 4);
            redraw_line(prompt, out, n, pos, &rendered);
            continue;
        }
        if ((BYTE)c >= 32 && n + 1 < capacity) {
            line_insert(out, &n, &pos, c);
            redraw_line(prompt, out, n, pos, &rendered);
        }
    }
}

static void command_cd(char *arg)
{
    char name[260];
    parse_word(arg, name);
    if (!*name) {
        write_s(g_current_directory); write_s("\r\n");
        return;
    }
    char path[260];
    if (normalize_path(name, path) &&
        (GetFileAttributesA(path) & 0x10) != 0 &&
        GetFileAttributesA(path) != (DWORD)-1) {
        unsigned i = 0;
        while ((g_current_directory[i] = path[i])) i++;
        return;
    }
    write_s("The system cannot find the path specified.\r\n");
}

static BOOL launch_external(const char *image, char *command_line,
                            HANDLE input, HANDLE output, HANDLE error,
                            PROCESS_INFORMATION *process_out)
{
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    BYTE *s = (BYTE *)&startup;
    BYTE *p = (BYTE *)&process;
    for (unsigned i = 0; i < sizeof(startup); i++) s[i] = 0;
    for (unsigned i = 0; i < sizeof(process); i++) p[i] = 0;
    startup.cb = sizeof(startup);
    startup.flags = STARTF_USESTDHANDLES;
    startup.standard_input = input;
    startup.standard_output = output;
    startup.standard_error = error;
    if (!CreateProcessA(image, command_line, 0, 0, 1, 0, 0,
                        g_current_directory,
                        &startup, &process))
        return 0;
    if (process_out) *process_out = process;
    else {
        WaitForSingleObject(process.hProcess, (DWORD)-1);
        GetExitCodeProcess(process.hProcess, &g_errorlevel);
        CloseHandle(process.hProcess);
    }
    return 1;
}

static int dispatch(char *line)
{
    char *p = skip_spaces(line);
    trim_right(p);
    if (!*p) return 1;

    char original[CMD_LINE_MAX];
    unsigned original_n = 0;
    while (p[original_n] && original_n + 1 < sizeof(original)) {
        original[original_n] = p[original_n];
        original_n++;
    }
    original[original_n] = 0;

    char command[260];
    char *arg = parse_word(p, command);

    if (same(command, "cd") || same(command, "chdir")) command_cd(arg);
    else if (same(command, "history") || same(command, "doskey")) command_history();
    else if (same(command, "set")) command_set(arg);
    else if (same(command, "if")) command_if(arg);
    else if (same(command, "rem") || command[0] == ':') { }
    else if (same(command, "echo")) {
        if (!*arg) write_s(g_echo ? "ECHO is on.\r\n" : "ECHO is off.\r\n");
        else if (same(arg, "on")) g_echo = 1;
        else if (same(arg, "off")) g_echo = 0;
        else { write_s(arg); write_s("\r\n"); }
    } else if (same(command, "exit")) return 0;
    else {
        char image[260];
        if (!resolve_program(command, image)) {
            g_errorlevel = 9009;
            write_s("'"); write_s(command);
            write_s("' is not recognized as an internal or external command.\r\n");
        } else if (ends_ci(image, ".bat")) {
            execute_batch(image);
        } else if (!launch_external(image, original, g_in, g_out, g_err,
                                    g_async_launch ? &g_launched_process : 0)) {
            g_errorlevel = 9009;
            write_s("The application could not be started.\r\n");
        }
    }
    return 1;
}

int main(void)
{
    char line[CMD_LINE_MAX];
    char prompt[280];
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_err = g_out;
    write_s("Svindows Command Processor [Version 0.6.0]\r\n"
            "Type HELP for a list of commands.\r\n\r\n");
    for (;;) {
        prompt[0] = 0;
        if (g_echo) {
            unsigned i = 0;
            while (g_current_directory[i] && i + 2 < sizeof(prompt)) {
                prompt[i] = g_current_directory[i];
                i++;
            }
            prompt[i++] = '>';
            prompt[i] = 0;
        }
        if (!read_line(prompt, line, sizeof(line))) break;
        if (*line) {
            unsigned slot;
            if (g_history_count < CMD_HISTORY_COUNT) {
                slot = g_history_count++;
            } else {
                for (unsigned i = 1; i < CMD_HISTORY_COUNT; i++) {
                    unsigned j = 0;
                    while ((g_history[i - 1][j] = g_history[i][j])) j++;
                }
                slot = CMD_HISTORY_COUNT - 1;
            }
            unsigned j = 0;
            while (line[j] && j + 1 < CMD_LINE_MAX) {
                g_history[slot][j] = line[j]; j++;
            }
            g_history[slot][j] = 0;
        }
        char expanded[CMD_LINE_MAX];
        expand_variables(line, expanded);
        if (!execute_line(expanded)) break;
    }
    return 0;
}
