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

/* The command processor is linked with /nodefaultlib.  Clang may lower
 * aggregate copies to a call to memcpy even when source code never names it,
 * so provide the tiny byte-copy primitive locally instead of depending on a
 * C runtime DLL.  Volatile accesses prevent the compiler from folding this
 * implementation back into a recursive memcpy call. */
void *memcpy(void *destination, const void *source, unsigned long long count)
{
    volatile BYTE *dst = (volatile BYTE *)destination;
    const volatile BYTE *src = (const volatile BYTE *)source;
    while (count--) *dst++ = *src++;
    return destination;
}

#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INVALID_HANDLE_VALUE ((HANDLE)(ULONGLONG)-1)

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) BOOL GetConsoleMode(HANDLE, DWORD *);
__declspec(dllimport) BOOL SetConsoleInputMode(HANDLE, DWORD);
__declspec(dllimport) BOOL SetConsoleStreamControl(HANDLE, DWORD, DWORD);
typedef struct _CMD_COORD { short X, Y; } CMD_COORD;
typedef struct _CMD_RECT { short Left, Top, Right, Bottom; } CMD_RECT;
typedef struct _CMD_SCREEN_INFO {
    CMD_COORD Size, CursorPosition;
    WORD Attributes;
    CMD_RECT Window;
    CMD_COORD MaximumWindowSize;
} CMD_SCREEN_INFO;
__declspec(dllimport) BOOL GetConsoleScreenBufferInfo(HANDLE, CMD_SCREEN_INFO *);
__declspec(dllimport) BOOL SetConsoleCursorPosition(HANDLE, CMD_COORD);
__declspec(dllimport) BOOL FillConsoleOutputCharacterA(HANDLE, char, DWORD,
                                                       CMD_COORD, DWORD *);
__declspec(dllimport) BOOL SetConsoleForegroundProcess(HANDLE, HANDLE);
__declspec(dllimport) BOOL RefreshConsole(HANDLE);
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
static int g_native_output_console;
static DWORD g_original_input_mode;
static CMD_COORD g_edit_origin;
static CMD_SCREEN_INFO g_edit_screen;

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

    if (g_native_output_console &&
        GetConsoleScreenBufferInfo(g_out, &g_edit_screen)) {
        unsigned width = (unsigned short)g_edit_screen.Size.X;
        unsigned height = (unsigned short)g_edit_screen.Size.Y;
        if (width && height) {
            (void)SetConsoleStreamControl(g_out, 2, 0);
            (void)SetConsoleCursorPosition(g_out, g_edit_origin);
            write_n(buf, n);
            unsigned stale = *rendered > n ? *rendered - n : 0;
            unsigned clear_at = (unsigned short)g_edit_origin.Y * width +
                                (unsigned short)g_edit_origin.X + n;
            if (clear_at < width * height && stale) {
                if (stale > width * height - clear_at)
                    stale = width * height - clear_at;
                CMD_COORD clear_origin = {
                    (short)(clear_at % width), (short)(clear_at / width)
                };
                DWORD cleared = 0;
                (void)FillConsoleOutputCharacterA(g_out, ' ', stale,
                                                   clear_origin, &cleared);
            }
            (void)SetConsoleStreamControl(g_out, 3, 0);
            (void)SetConsoleStreamControl(g_out, 4, n - pos);
            unsigned caret = (unsigned short)g_edit_origin.Y * width +
                            (unsigned short)g_edit_origin.X + pos;
            if (caret >= width * height) caret = width * height - 1;
            CMD_COORD caret_position = {
                (short)(caret % width), (short)(caret / width)
            };
            (void)SetConsoleCursorPosition(g_out, caret_position);
            *rendered = n;
            return;
        }
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


#define CMD_TAB_MATCH_MAX 64

typedef struct _CMD_TAB_MATCH {
    char Name[260];
    int Directory;
} CMD_TAB_MATCH;

static CMD_TAB_MATCH g_tab_matches[CMD_TAB_MATCH_MAX];
static unsigned g_tab_match_count;
static char g_tab_snapshot[CMD_LINE_MAX];
static unsigned g_tab_snapshot_pos;
static int g_tab_snapshot_valid;

static int tab_prefix_matches(const char *name, const char *prefix)
{
    while (*prefix) {
        if (!*name || lower(*name) != lower(*prefix)) return 0;
        name++;
        prefix++;
    }
    return 1;
}

/* Keep the command processor freestanding: /nodefaultlib means CRT helpers
 * such as strchr are not available unless we provide them ourselves. */
static int tab_contains_char(const char *text, char character)
{
    while (*text) {
        if (*text == character) return 1;
        text++;
    }
    return 0;
}

static int tab_is_executable_name(const char *name)
{
    return ends_ci(name, ".exe") || ends_ci(name, ".com") ||
           ends_ci(name, ".bat") || ends_ci(name, ".elf") ||
           !tab_contains_char(name, '.');
}

static void tab_add_match(const char *name, int directory,
                          const char *prefix, int command_position)
{
    char candidate[260];
    unsigned length = slen(name);
    if (!length || length >= sizeof(candidate)) return;
    for (unsigned i = 0; i <= length; i++) candidate[i] = name[i];

    /* PATHEXT makes foo.exe executable as `foo`; expose the shell spelling
     * unless the user has already started typing an extension. */
    if (command_position && !directory && !tab_contains_char(prefix, '.') &&
        tab_is_executable_name(candidate)) {
        unsigned n = slen(candidate);
        while (n && candidate[n - 1] != '.') n--;
        if (n) candidate[n - 1] = 0;
    }
    if (!tab_prefix_matches(candidate, prefix)) return;
    for (unsigned i = 0; i < g_tab_match_count; i++)
        if (same(g_tab_matches[i].Name, candidate)) return;
    if (g_tab_match_count >= CMD_TAB_MATCH_MAX) return;
    unsigned n = slen(candidate);
    for (unsigned i = 0; i <= n; i++) g_tab_matches[g_tab_match_count].Name[i] = candidate[i];
    g_tab_matches[g_tab_match_count].Directory = directory;
    g_tab_match_count++;
}

static void tab_search_directory(const char *directory, const char *prefix,
                                 int command_position)
{
    char absolute[260], pattern[520];
    if (!normalize_path(directory, absolute)) return;
    unsigned used = 0;
    while (absolute[used] && used + 2 < sizeof(pattern)) {
        pattern[used] = absolute[used];
        used++;
    }
    if (!used || (pattern[used - 1] != '\\' && pattern[used - 1] != '/'))
        pattern[used++] = '\\';
    for (unsigned i = 0; prefix[i] && used + 2 < sizeof(pattern); i++)
        pattern[used++] = prefix[i];
    pattern[used++] = '*';
    pattern[used] = 0;

    WORD wide[520];
    for (unsigned i = 0; i <= used; i++) wide[i] = (BYTE)pattern[i];
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(wide, &found);
    if (!search || search == INVALID_HANDLE_VALUE) return;
    do {
        char name[260];
        unsigned n = 0;
        while (found.name[n] && n + 1 < sizeof(name)) {
            name[n] = found.name[n] <= 0x7f ? (char)found.name[n] : '?';
            n++;
        }
        name[n] = 0;
        if (!command_position || (found.attributes & 0x10u) == 0) {
            if (!command_position || tab_is_executable_name(name))
                tab_add_match(name, (found.attributes & 0x10u) != 0,
                              prefix, command_position);
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

static void tab_collect_matches(const char *token, int command_position)
{
    g_tab_match_count = 0;
    static const char *const commands[] = {
        "cd", "cls", "copy", "del", "dir", "echo", "exit",
        "fastfetch", "help", "history", "hostname", "md", "rd",
        "ren", "set", "start", "terminal", "type", "uname", "ver",
        "vol", "xterm"
    };
    int has_path = tab_contains_char(token, '\\') ||
                   tab_contains_char(token, '/') ||
                   tab_contains_char(token, ':');
    if (command_position && !has_path) {
        for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
            tab_add_match(commands[i], 0, token, 1);

        /* The command resolver checks the current directory before PATH. */
        tab_search_directory(g_current_directory, token, 1);

        const char *path = env_get("PATH");
        unsigned start = 0;
        while (path[start]) {
            char directory[260];
            unsigned n = 0;
            while (path[start] && path[start] != ';' && n + 1 < sizeof(directory))
                directory[n++] = path[start++];
            directory[n] = 0;
            while (path[start] == ';') start++;
            if (n) tab_search_directory(directory, token, 1);
        }
        return;
    }

    char directory[260];
    char leaf[260];
    unsigned length = slen(token), split = length;
    for (unsigned i = 0; i < length; i++)
        if (token[i] == '\\' || token[i] == '/') split = i;
    unsigned leaf_start = split < length ? split + 1 : 0;
    char token_prefix[260];
    unsigned token_prefix_length = 0;
    unsigned leaf_length = length - leaf_start;
    if (leaf_length >= sizeof(leaf)) leaf_length = sizeof(leaf) - 1;
    for (unsigned i = 0; i < leaf_length; i++) leaf[i] = token[leaf_start + i];
    leaf[leaf_length] = 0;

    if (split == length) {
        if (length >= 2 && token[1] == ':') {
            /* `C:` means the drive root for completion, not CWD + `C:`. */
            directory[0] = token[0];
            directory[1] = ':';
            directory[2] = 0;
            token_prefix[0] = token[0];
            token_prefix[1] = ':';
            if (length == 2) {
                token_prefix[2] = '\\';
                token_prefix[3] = 0;
                token_prefix_length = 3;
                leaf[0] = 0;
            } else {
                token_prefix[2] = 0;
                token_prefix_length = 2;
                for (unsigned i = 2; i < length; i++) leaf[i - 2] = token[i];
                leaf[length - 2] = 0;
            }
        } else {
            copy_line(directory, g_current_directory, sizeof(directory));
        }
    } else {
        unsigned directory_length = split ? split : 1;
        if (directory_length >= sizeof(directory)) return;
        for (unsigned i = 0; i < directory_length; i++) directory[i] = token[i];
        directory[directory_length] = 0;
        token_prefix_length = leaf_start;
        for (unsigned i = 0; i < token_prefix_length; i++)
            token_prefix[i] = token[i];
        token_prefix[token_prefix_length] = 0;
    }
    tab_search_directory(directory, leaf, 0);
    if (token_prefix_length) {
        for (unsigned i = 0; i < g_tab_match_count; i++) {
            char full_name[260];
            unsigned name_length = slen(g_tab_matches[i].Name);
            if (token_prefix_length + name_length >= sizeof(full_name)) continue;
            for (unsigned j = 0; j < token_prefix_length; j++)
                full_name[j] = token_prefix[j];
            for (unsigned j = 0; j <= name_length; j++)
                full_name[token_prefix_length + j] = g_tab_matches[i].Name[j];
            for (unsigned j = 0; j <= token_prefix_length + name_length; j++)
                g_tab_matches[i].Name[j] = full_name[j];
        }
    }
}

static int tab_name_after(const char *left, const char *right)
{
    while (*left && *right && lower(*left) == lower(*right)) {
        left++;
        right++;
    }
    return lower(*left) > lower(*right);
}

static void tab_sort_matches(void)
{
    for (unsigned i = 1; i < g_tab_match_count; i++) {
        CMD_TAB_MATCH item = g_tab_matches[i];
        unsigned j = i;
        while (j && tab_name_after(g_tab_matches[j - 1].Name, item.Name)) {
            g_tab_matches[j] = g_tab_matches[j - 1];
            j--;
        }
        g_tab_matches[j] = item;
    }
}

static unsigned tab_common_prefix(void)
{
    if (!g_tab_match_count) return 0;
    unsigned common = slen(g_tab_matches[0].Name);
    for (unsigned i = 1; i < g_tab_match_count && common; i++) {
        unsigned j = 0;
        while (j < common && g_tab_matches[i].Name[j] &&
               lower(g_tab_matches[0].Name[j]) == lower(g_tab_matches[i].Name[j]))
            j++;
        common = j;
    }
    return common;
}

static int tab_is_command_position(const char *line, unsigned token_begin)
{
    int expect_command = 1;
    int quoted = 0;
    for (unsigned i = 0; i < token_begin; i++) {
        char c = line[i];
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (quoted) continue;
        if (c == ';' || c == '&' || c == '|') {
            expect_command = 1;
        } else if (c != ' ' && c != '\t') {
            expect_command = 0;
        }
    }
    return expect_command;
}

static void tab_show_matches(const char *prompt, const char *line,
                             unsigned length, unsigned position,
                             unsigned *rendered)
{
    write_s("\r\n");
    for (unsigned i = 0; i < g_tab_match_count; i++) {
        write_s(g_tab_matches[i].Name);
        if (g_tab_matches[i].Directory) write_s("\\");
        write_s("\r\n");
    }
    *rendered = 0;
    write_s(prompt);
    if (g_ansi_terminal) {
        write_s("\x1b[s");
    } else if (g_native_output_console &&
               GetConsoleScreenBufferInfo(g_out, &g_edit_screen)) {
        g_edit_origin = g_edit_screen.CursorPosition;
        (void)SetConsoleStreamControl(g_out, 1, 0);
    }
    redraw_line(prompt, line, length, position, rendered);
}

static int read_line(const char *prompt, char *out, unsigned capacity)
{
    if (capacity < 2) return 0;
    unsigned n = 0, pos = 0, rendered = 0;
    int hist_pos = -1;
    g_tab_snapshot_valid = 0;
    static char draft[CMD_LINE_MAX];
    out[0] = 0;
    draft[0] = 0;
    /* The native console normally buffers input until Enter. Temporarily
     * switch only its input side to one-byte mode while this editor is active;
     * restore the caller's mode before dispatching the command. */
    int restore_input_mode = g_native_input_console &&
        SetConsoleInputMode(g_in, 0x0001u);
    write_s(prompt);
    if (g_native_output_console &&
        GetConsoleScreenBufferInfo(g_out, &g_edit_screen)) {
        g_edit_origin = g_edit_screen.CursorPosition;
        (void)SetConsoleStreamControl(g_out, 1, 0);
    }
    if (g_ansi_terminal) write_s("\x1b[s");

    for (;;) {
        char c;
        if (!read_byte(&c)) continue;

        /* Terminals differ: PS/2 reports BS, while xterm commonly sends DEL.
         * Normalize both before they enter the shell's line editor. */
        if ((unsigned char)c == 0x7f) c = '\b';
        if (c != '\t') g_tab_snapshot_valid = 0;

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
            int in_quotes = 0;
            for (unsigned i = 0; i < pos; i++)
                if (out[i] == '"') in_quotes = !in_quotes;
            while (begin) {
                char previous = out[begin - 1];
                if (previous == '"') {
                    in_quotes = !in_quotes;
                    begin--;
                    continue;
                }
                if (!in_quotes && (previous == ' ' || previous == '\t')) break;
                begin--;
            }
            unsigned token_begin = begin;
            if (out[token_begin] == '"') token_begin++;
            char token[260];
            unsigned token_length = pos - token_begin;
            if (token_length >= sizeof(token)) token_length = sizeof(token) - 1;
            for (unsigned i = 0; i < token_length; i++) token[i] = out[token_begin + i];
            token[token_length] = 0;
            int command_position = tab_is_command_position(out, begin);

            int repeated = g_tab_snapshot_valid && g_tab_snapshot_pos == pos &&
                           same(g_tab_snapshot, out);
            tab_collect_matches(token, command_position);
            tab_sort_matches();
            if (g_tab_match_count) {
                unsigned common = tab_common_prefix();
                if (g_tab_match_count > 1 && common == token_length && repeated) {
                    tab_show_matches(prompt, out, n, pos, &rendered);
                    g_tab_snapshot_valid = 0;
                    continue;
                }

                const CMD_TAB_MATCH *match = &g_tab_matches[0];
                unsigned replace_length = token_length;
                unsigned target_length = g_tab_match_count == 1 ?
                    slen(match->Name) : common;
                if (target_length > replace_length) {
                    for (unsigned i = 0; i < replace_length; i++)
                        line_delete_before(out, &n, &pos);
                    for (unsigned i = 0; i < target_length; i++)
                        line_insert(out, &n, &pos, capacity, match->Name[i]);
                }

                if (g_tab_match_count == 1) {
                    if (match->Directory) {
                        if (pos < capacity - 1 &&
                            (pos == 0 || (out[pos - 1] != '\\' && out[pos - 1] != '/')))
                            line_insert(out, &n, &pos, capacity, '\\');
                    } else if (command_position || pos == n || out[pos] != ' ') {
                        if (pos < capacity - 1)
                            line_insert(out, &n, &pos, capacity, ' ');
                    }
                    g_tab_snapshot_valid = 0;
                } else {
                    copy_line(g_tab_snapshot, out, sizeof(g_tab_snapshot));
                    g_tab_snapshot_pos = pos;
                    g_tab_snapshot_valid = 1;
                }
            } else {
                g_tab_snapshot_valid = 0;
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
        /* Native consoles and xterm PTY bridges both track the active child.
         * The PTY path is per-terminal so Ctrl+C cannot hit TWM/startx through
         * the global native-console foreground slot. */
        (void)SetConsoleForegroundProcess(g_in, process.hProcess);
        WaitForSingleObject(process.hProcess, (DWORD)-1);
        (void)SetConsoleForegroundProcess(g_in, 0);
        if (g_native_input_console) {
            /* A DOS/Windows guest can leave the console input side in a
             * different state after taking over the display. Restore the
             * shell's original mode before the next line-editor read. */
            SetConsoleInputMode(g_in, g_original_input_mode & 0x0007u);
        }
        /* The DOS video backend preserves the native text buffer while VGA
         * owns the framebuffer. Repaint it before returning to the prompt. */
        RefreshConsole(g_out);
        GetExitCodeProcess(process.hProcess, &g_errorlevel);
        if (process.hThread) CloseHandle(process.hThread);
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
    else if (same(command, "type")) command_type(arg);
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
    g_native_output_console = GetConsoleMode(g_out, &output_mode);
    g_original_input_mode = input_mode;
    g_ansi_terminal = !g_native_input_console && !g_native_output_console;

    write_s("Svindows Command Processor [Version 0.6.0]\r\n"
            "Type HELP for a list of commands.\r\n\r\n");
    for (;;) {
        prompt[0] = 0;
        /* ECHO controls command echo only; DOS keeps showing its prompt after
         * `@echo off` in a batch file. */
        unsigned i = 0;
        while (g_current_directory[i] && i + 2 < sizeof(prompt)) {
            prompt[i] = g_current_directory[i];
            i++;
        }
        prompt[i++] = '>';
        prompt[i] = 0;
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
