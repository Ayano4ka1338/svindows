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
__declspec(dllimport) BOOL GetConsoleMode(HANDLE, DWORD *);
__declspec(dllimport) BOOL SetConsoleInputMode(HANDLE, DWORD);
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
__declspec(dllimport) char *GetCommandLineA(void);
__declspec(dllimport) DWORD GetCurrentDirectoryW(DWORD, WORD *);
__declspec(dllimport) BOOL SetCurrentDirectoryW(const WORD *);
__declspec(dllimport) WORD *GetEnvironmentStringsW(void);
__declspec(dllimport) BOOL FreeEnvironmentStringsW(WORD *);
__declspec(dllimport) HANDLE FindFirstFileW(const WORD *, void *);
__declspec(dllimport) BOOL FindNextFileW(HANDLE, void *);
__declspec(dllimport) BOOL FindClose(HANDLE);

typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess, hThread;
    DWORD dwProcessId, dwThreadId;
} PROCESS_INFORMATION;

typedef struct _WIN32_FIND_DATAW {
    DWORD attributes;
    ULONGLONG times[3];
    DWORD size_high, size_low, reserved0, reserved1;
    WORD name[260], alternate[14];
} WIN32_FIND_DATAW;

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
static int g_background_launch;
static PROCESS_INFORMATION g_launched_process;
static int g_ansi_terminal;
static int g_native_input_console;
static DWORD g_original_input_mode;

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

static void copy_line(char *dst, const char *src, unsigned capacity)
{
    unsigned i = 0;
    if (!capacity) return;
    while (src[i] && i + 1 < capacity) { dst[i] = src[i]; i++; }
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

/* PTYs understand ANSI cursor save/restore. Repainting from a saved origin
 * avoids the old CR + spaces + BS algorithm, which lost the cursor when a
 * line wrapped and could leave stale characters after repeated deletion. */
static void redraw_line(const char *prompt, const char *buf, unsigned n,
                        unsigned pos, unsigned *rendered)
{
    if (g_ansi_terminal) {
        write_s("\x1b[u");
        if (n) write_n(buf, n);
        write_s("\x1b[J");
        write_s("\x1b[u");
        if (pos) write_n(buf, pos);
        *rendered = n;
        return;
    }

    write_s("\r");
    write_s(prompt);
    write_n(buf, n);
    unsigned drawn = n;
    if (*rendered > n) {
        for (unsigned i = n; i < *rendered; i++)
            write_s(" ");
        drawn = *rendered;
    }
    *rendered = n;
    /* Include the clearing spaces in the cursor rewind.  Rewinding only n
     * characters leaves the caret one or more cells too far right after a
     * tail deletion. */
    for (unsigned i = drawn; i > pos; i--)
        write_s("\b");
}

static void line_insert(char *out, unsigned *n, unsigned *pos,
                        unsigned capacity, char c)
{
    if (*n + 1 >= capacity) return;
    for (unsigned i = *n; i > *pos; i--)
        out[i] = out[i - 1];
    out[*pos] = c;
    (*n)++;
    (*pos)++;
    out[*n] = 0;
}

static void line_delete_before(char *out, unsigned *n, unsigned *pos)
{
    if (!*pos) return;
    for (unsigned i = *pos - 1; i < *n; i++)
        out[i] = out[i + 1];
    (*pos)--;
    (*n)--;
}

static void line_delete_at(char *out, unsigned *n, unsigned pos)
{
    if (pos >= *n) return;
    for (unsigned i = pos; i < *n; i++)
        out[i] = out[i + 1];
    (*n)--;
}

static void line_kill_word(char *out, unsigned *n, unsigned *pos)
{
    while (*pos && (out[*pos - 1] == ' ' || out[*pos - 1] == '\t'))
        line_delete_before(out, n, pos);
    while (*pos && out[*pos - 1] != ' ' && out[*pos - 1] != '\t')
        line_delete_before(out, n, pos);
}

static int read_line(const char *prompt, char *out, unsigned capacity)
{
    if (capacity < 2) return 0;
    unsigned n = 0, pos = 0, rendered = 0;
    int hist_pos = -1;
    static char draft[CMD_LINE_MAX];
    out[0] = 0;
    draft[0] = 0;
    /* The native console normally buffers input until Enter. Temporarily
     * switch only its input side to one-byte mode while this editor is active;
     * restore the caller's mode before dispatching the command. */
    int restore_input_mode = g_native_input_console &&
        SetConsoleInputMode(g_in, 0x0001u);
    write_s(prompt);
    if (g_ansi_terminal) write_s("\x1b[s");

    for (;;) {
        char c;
        if (!read_byte(&c)) continue;

        /* Terminals differ: PS/2 reports BS, while xterm commonly sends DEL.
         * Normalize both before they enter the shell's line editor. */
        if ((unsigned char)c == 0x7f) c = '\b';

        if ((unsigned char)c == 27) {
            char sequence[12];
            unsigned used = 0;
            char first;
            if (!read_byte(&first)) continue;
            if (first == '[' || first == 'O') {
                char final = 0;
                while (used + 1 < sizeof(sequence)) {
                    char next;
                    if (!read_byte(&next)) break;
                    sequence[used++] = next;
                    if ((unsigned char)next >= 0x40 &&
                        (unsigned char)next <= 0x7e) {
                        final = next;
                        break;
                    }
                }
                sequence[used] = 0;
                if (first == 'O') {
                    if (final == 'H') pos = 0;
                    else if (final == 'F') pos = n;
                    else continue;
                } else {
                    unsigned parameter = 0;
                    for (unsigned i = 0; i < used; i++) {
                        if (sequence[i] < '0' || sequence[i] > '9') break;
                        parameter = parameter * 10u +
                                    (unsigned)(sequence[i] - '0');
                    }
                    switch (final) {
                    case 'A': /* history up */
                        if (g_history_count && hist_pos != 0) {
                            if (hist_pos < 0) {
                                out[n] = 0;
                                copy_line(draft, out, sizeof(draft));
                                hist_pos = (int)g_history_count;
                            }
                            hist_pos--;
                            copy_line(out, g_history[hist_pos], capacity);
                            n = slen(out); pos = n;
                            redraw_line(prompt, out, n, pos, &rendered);
                        }
                        continue;
                    case 'B': /* history down */
                        if (hist_pos >= 0) {
                            hist_pos++;
                            if (hist_pos >= (int)g_history_count) {
                                hist_pos = -1;
                                copy_line(out, draft, capacity);
                            } else {
                                copy_line(out, g_history[hist_pos], capacity);
                            }
                            n = slen(out); pos = n;
                            redraw_line(prompt, out, n, pos, &rendered);
                        }
                        continue;
                    case 'C': if (pos < n) pos++; break;
                    case 'D': if (pos) pos--; break;
                    case 'H': pos = 0; break;
                    case 'F': pos = n; break;
                    case '~':
                        if (parameter == 1 || parameter == 7) pos = 0;
                        else if (parameter == 4 || parameter == 8) pos = n;
                        else if (parameter == 3) line_delete_at(out, &n, pos);
                        else continue;
                        break;
                    default: continue;
                    }
                }
                redraw_line(prompt, out, n, pos, &rendered);
                continue;
            }
            /* SS3/CSI unsupported prefixes are consumed as escape input, not
             * inserted into the command text as stray printable characters. */
            continue;
        }

        if ((unsigned char)c == 3) { /* Ctrl+C at the prompt */
            n = pos = 0; out[0] = 0;
            write_s("^C\r\n");
            if (restore_input_mode)
                SetConsoleInputMode(g_in, g_original_input_mode & 0x0007u);
            return 2;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            out[n] = 0;
            redraw_line(prompt, out, n, n, &rendered);
            write_s("\r\n");
            if (restore_input_mode)
                SetConsoleInputMode(g_in, g_original_input_mode & 0x0007u);
            return 1;
        }
        if (c == '\b') {
            line_delete_before(out, &n, &pos);
            redraw_line(prompt, out, n, pos, &rendered);
            continue;
        }
        if ((unsigned char)c == 1) { /* Ctrl+A */
            pos = 0;
            redraw_line(prompt, out, n, pos, &rendered);
            continue;
        }
        if ((unsigned char)c == 5) { /* Ctrl+E */
            pos = n;
            redraw_line(prompt, out, n, pos, &rendered);
            continue;
        }
        if ((unsigned char)c == 21) { /* Ctrl+U: clear the input line */
            n = pos = 0;
            out[0] = 0;
            hist_pos = -1;
            redraw_line(prompt, out, n, pos, &rendered);
            continue;
        }
        if ((unsigned char)c == 23) { /* Ctrl+W: erase the previous word */
            line_kill_word(out, &n, &pos);
            redraw_line(prompt, out, n, pos, &rendered);
            continue;
        }
        if (c == '\t') {
            unsigned begin = pos;
            while (begin && out[begin - 1] != ' ' && out[begin - 1] != '\t') begin--;
            char pattern[520]; unsigned w = 0;
            for (unsigned i = 0; g_current_directory[i] && w + 2 < 520; i++)
                pattern[w++] = g_current_directory[i];
            if (w && pattern[w - 1] != '\\') pattern[w++] = '\\';
            for (unsigned i = begin; i < pos && w + 2 < 520; i++) pattern[w++] = out[i];
            pattern[w++] = '*'; pattern[w] = 0;
            WORD wide[520]; for (unsigned i = 0; i <= w; i++) wide[i] = (BYTE)pattern[i];
            char completion[260]; unsigned k = 0;
            static const char *const commands[] = {
                "cd", "cls", "copy", "del", "dir", "echo", "exit",
                "fastfetch", "help", "history", "hostname", "md", "rd",
                "ren", "set", "start", "terminal", "type", "uname", "ver",
                "vol", "xterm"
            };
            if (begin == 0) {
                unsigned prefix = pos - begin;
                for (unsigned ci = 0; ci < sizeof(commands) / sizeof(commands[0]); ci++) {
                    unsigned j = 0;
                    while (j < prefix && commands[ci][j] &&
                           lower(commands[ci][j]) == lower(out[begin + j])) j++;
                    if (j == prefix && commands[ci][j]) {
                        while (commands[ci][k] && k + 1 < sizeof(completion)) {
                            completion[k] = commands[ci][k]; k++;
                        }
                        break;
                    }
                }
            }
            if (!k) {
                WIN32_FIND_DATAW found; HANDLE search = FindFirstFileW(wide, &found);
                if (search && search != INVALID_HANDLE_VALUE) {
                    while (found.name[k] && k + 1 < sizeof(completion)) {
                        completion[k] = found.name[k] <= 0x7f ? (char)found.name[k] : '?'; k++;
                    }
                    if (found.attributes & 0x10u) completion[k++] = '\\';
                    FindClose(search);
                }
            }
            completion[k] = 0;
            if (k >= pos - begin && k) {
                unsigned old = pos - begin;
                for (unsigned j = 0; j < old; j++) line_delete_before(out, &n, &pos);
                for (unsigned j = 0; j < k; j++) line_insert(out, &n, &pos, capacity, completion[j]);
                if (begin == 0 && completion[k - 1] != '\\' && pos + 1 < capacity)
                    line_insert(out, &n, &pos, capacity, ' ');
            }
            redraw_line(prompt, out, n, pos, &rendered);
            continue;
        }
        if ((unsigned char)c >= 32 && (unsigned char)c != 127) {
            line_insert(out, &n, &pos, capacity, c);
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
    if (normalize_path(name, path)) {
        WORD wide[260];
        unsigned i = 0;
        while (path[i] && i < 259) {
            wide[i] = (BYTE)path[i];
            i++;
        }
        wide[i] = 0;
        if (path[i] || !SetCurrentDirectoryW(wide)) {
            write_s("The system cannot find the path specified.\r\n");
            return;
        }
        WORD current[260];
        DWORD count = GetCurrentDirectoryW(260, current);
        if (count && count < 260)
            for (DWORD j = 0; j <= count; j++)
                g_current_directory[j] = current[j] <= 0x7f ? (char)current[j] : '?';
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
    char environment[2048];
    unsigned used = 0;
    for (unsigned e = 0; e < g_environment_count; e++) {
        unsigned name_length = slen(g_environment[e].Name);
        unsigned value_length = slen(g_environment[e].Value);
        if (used + name_length + value_length + 3 > sizeof(environment))
            return 0;
        for (unsigned i = 0; i < name_length; i++)
            environment[used++] = g_environment[e].Name[i];
        environment[used++] = '=';
        for (unsigned i = 0; i < value_length; i++)
            environment[used++] = g_environment[e].Value[i];
        environment[used++] = 0;
    }
    environment[used++] = 0;
    if (ends_ci(image, "xterm.elf")) {
        static const char xterm_command[] = "xterm -e /usr/bin/cmd";
        copy_line(command_line, xterm_command, CMD_LINE_MAX);
    }
    if (!CreateProcessA(image, command_line, 0, 0, 1, 0, environment,
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

static int command_start(char *arguments)
{
    arguments = skip_spaces(arguments);
    /* Accept the conventional empty title argument: start "" xterm. */
    if (arguments[0] == '"' && arguments[1] == '"' &&
        (arguments[2] == ' ' || arguments[2] == '\t'))
        arguments = skip_spaces(arguments + 2);
    if (!*arguments) {
        write_s("Usage: START <program> [arguments]\r\n");
        return 1;
    }

    char program[260];
    (void)parse_word(arguments, program);
    char image[260];
    if (!*program || !resolve_program(program, image)) {
        write_s("START: program not found.\r\n");
        g_errorlevel = 9009;
        return 1;
    }

    char command_line[CMD_LINE_MAX];
    copy_line(command_line, arguments, sizeof(command_line));
    PROCESS_INFORMATION child;
    if (!launch_external(image, command_line, g_in, g_out, g_err, &child)) {
        write_s("START: unable to launch program.\r\n");
        g_errorlevel = 1;
        return 1;
    }

    /* Closing process handles does not terminate the child. It only detaches
     * the shell from its lifetime so the new terminal remains independent. */
    if (child.hThread) CloseHandle(child.hThread);
    if (child.hProcess) CloseHandle(child.hProcess);
    g_errorlevel = 0;
    return 1;
}

static int command_terminal(void)
{
    char arguments[] = "xterm -e /usr/bin/cmd";
    return command_start(arguments);
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
    else if (same(command, "start")) command_start(arg);
    else if (same(command, "terminal") || same(command, "newterm"))
        command_terminal();
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

static char *command_switch_tail(void)
{
    char *command = GetCommandLineA();
    if (!command) return 0;
    int quoted = 0;
    while (*command) {
        if (*command == '"') quoted = !quoted;
        else if (!quoted && (*command == ' ' || *command == '\t')) break;
        command++;
    }
    while (*command == ' ' || *command == '\t') command++;
    if (lower(command[0]) != '/' || lower(command[1]) != 'c' ||
        (command[2] && command[2] != ' ' && command[2] != '\t'))
        return 0;
    command += 2;
    while (*command == ' ' || *command == '\t') command++;
    return command;
}

static void sync_current_directory(void)
{
    WORD wide[260];
    DWORD count = GetCurrentDirectoryW(260, wide);
    if (!count || count >= 260) return;
    for (DWORD i = 0; i <= count; i++)
        g_current_directory[i] = wide[i] <= 0x7f ? (char)wide[i] : '?';
}

static void sync_inherited_environment(void)
{
    WORD *block = GetEnvironmentStringsW();
    if (!block) return;
    for (const WORD *entry = block; *entry; ) {
        char name[32], value[260];
        unsigned n = 0, v = 0;
        while (entry[n] && entry[n] != '=' && n + 1 < sizeof(name)) {
            name[n] = entry[n] <= 0x7f ? (char)entry[n] : '?';
            n++;
        }
        if (entry[n] == '=') {
            name[n] = 0;
            const WORD *text = entry + n + 1;
            while (text[v] && v + 1 < sizeof(value)) {
                value[v] = text[v] <= 0x7f ? (char)text[v] : '?';
                v++;
            }
            if (!text[v]) {
                value[v] = 0;
                /* Linux PATH uses colons and / paths; cmd keeps its DOS
                 * executable search path in that case. */
                if (!(same(name, "PATH") && value[0] == '/'))
                    env_set(name, value);
            }
        }
        while (*entry) entry++;
        entry++;
    }
    FreeEnvironmentStringsW(block);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    char line[CMD_LINE_MAX];
    char prompt[280];
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_err = g_out;
    sync_current_directory();
    sync_inherited_environment();
    char *one_shot = command_switch_tail();
    if (one_shot) {
        char expanded[CMD_LINE_MAX];
        expand_variables(one_shot, expanded);
        execute_line(expanded);
        return (int)g_errorlevel;
    }
    DWORD input_mode = 0, output_mode = 0;
    g_native_input_console = GetConsoleMode(g_in, &input_mode);
    int native_output = GetConsoleMode(g_out, &output_mode);
    g_original_input_mode = input_mode;
    g_ansi_terminal = !g_native_input_console && !native_output;

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
        int line_status = read_line(prompt, line, sizeof(line));
        if (!line_status) break;
        if (line_status == 2) continue;
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
