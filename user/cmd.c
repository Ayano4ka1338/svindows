/*
 * cmd.c - Svindows command processor.
 *
 * This is a normal console PE, not kernel code.  Commands which change shell
 * state or require shell syntax are built in, just as they are in Windows'
 * cmd.exe. Filesystem commands use the writable FAT32 Win32 facade.
 */
typedef void              *HANDLE;
typedef void              *LPVOID;
typedef unsigned char      BYTE;
typedef unsigned short     WORD;
typedef unsigned long      DWORD;
typedef unsigned long long ULONGLONG;
typedef long               NTSTATUS;
typedef int                BOOL;
typedef unsigned short     WCHAR;

#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INVALID_HANDLE_VALUE ((HANDLE)(ULONGLONG)-1)

typedef struct _FILETIME { DWORD Low, High; } FILETIME;
typedef struct _WIN32_FIND_DATAW {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
    DWORD    dwReserved0, dwReserved1;
    WCHAR    cFileName[260];
    WCHAR    cAlternateFileName[14];
} WIN32_FIND_DATAW;

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) HANDLE CreateFileA(const char *, DWORD, DWORD, LPVOID,
                                         DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL CloseHandle(HANDLE);
__declspec(dllimport) HANDLE FindFirstFileExW(const WCHAR *, int,
                                              WIN32_FIND_DATAW *, int,
                                              LPVOID, DWORD);
__declspec(dllimport) BOOL FindNextFileW(HANDLE, WIN32_FIND_DATAW *);
__declspec(dllimport) BOOL FindClose(HANDLE);
__declspec(dllimport) NTSTATUS NtEnumerateDirectorySimple(
    const char *, DWORD, void *);
__declspec(dllimport) BOOL CreateProcessA(const char *, char *, LPVOID, LPVOID,
                                          BOOL, DWORD, LPVOID, const char *,
                                          LPVOID, LPVOID);
__declspec(dllimport) DWORD WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) BOOL GetExitCodeProcess(HANDLE, DWORD *);
__declspec(dllimport) BOOL CreatePipe(HANDLE *, HANDLE *, LPVOID, DWORD);
__declspec(dllimport) BOOL DeleteFileA(const char *);
__declspec(dllimport) BOOL MoveFileA(const char *, const char *);
__declspec(dllimport) BOOL CreateDirectoryA(const char *, LPVOID);
__declspec(dllimport) BOOL RemoveDirectoryA(const char *);
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

static void write_u64_width(ULONGLONG value, unsigned width)
{
    ULONGLONG copy = value;
    unsigned digits = 1;
    while (copy >= 10) { copy /= 10; digits++; }
    while (width > digits) { write_s(" "); width--; }
    write_u64(value);
}

static int dispatch(char *line);
static int execute_line(char *line);
#include "cmd/shell_features.inc"
#include "cmd/syntax.inc"

static int read_line(char *out, unsigned capacity)
{
    unsigned n = 0;
    for (;;) {
        char c;
        DWORD got = 0;
        if (!ReadFile(g_in, &c, 1, &got, 0) || !got)
            continue;
        if (c == '\r')
            continue;
        if (c == '\n') {
            write_s("\r\n");
            out[n] = 0;
            return 1;
        }
        if (c == '\b') {
            if (n) {
                n--;
                write_s("\b \b");
            }
            continue;
        }
        if (c == 27) {
            while (n) { write_s("\b \b"); n--; }
            continue;
        }
        if (c == '\t') {
            do {
                if (n + 1 < capacity) out[n++] = ' ';
                write_s(" ");
            } while (n % 4);
            continue;
        }
        if ((BYTE)c >= 32 && n + 1 < capacity) {
            out[n++] = c;
            write_n(&c, 1);
        }
    }
}

static void ascii_to_wide(const char *text, WCHAR *wide, unsigned cap)
{
    unsigned i = 0;
    for (; text[i] && i + 1 < cap; i++) wide[i] = (WCHAR)(BYTE)text[i];
    wide[i] = 0;
}

static void print_wide(const WCHAR *text)
{
    char buf[260];
    unsigned i = 0;
    while (text[i] && i < sizeof(buf)) {
        buf[i] = text[i] <= 0x7f ? (char)text[i] : '?';
        i++;
    }
    write_n(buf, i);
}

typedef struct _CMD_FIND_DATA {
    char Name[260];
    DWORD Size;
    DWORD Attributes;
    WORD WriteDate;
    WORD WriteTime;
} CMD_FIND_DATA;

static int dir_wildcard_match(const char *pattern, const char *name)
{
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*name) {
                if (dir_wildcard_match(pattern, name)) return 1;
                name++;
            }
            return dir_wildcard_match(pattern, name);
        }
        if (!*name || (*pattern != '?' &&
            lower(*pattern) != lower(*name)))
            return 0;
        pattern++;
        name++;
    }
    return *name == 0;
}

static void command_dir(char *arg)
{
    DWORD files = 0;
    DWORD directories = 0;
    ULONGLONG bytes = 0;

    arg = skip_spaces(arg);
    if (!*arg) arg = "*";
    if (same(arg, "/?")) {
        write_s("Displays files in the current directory.\r\n\r\nDIR [pattern]\r\n");
        return;
    }
    char path[260];
    if (!normalize_path(arg, path)) {
        write_s("The system cannot find the path specified.\r\n");
        return;
    }

    char directory[260];
    char pattern[260];
    unsigned separator = 0;
    for (unsigned i = 0; path[i]; i++)
        if (path[i] == '\\' || path[i] == '/') separator = i;
    if (separator == 2 && path[1] == ':') separator++;
    unsigned d = 0;
    for (; d < separator && d + 1 < sizeof(directory); d++)
        directory[d] = path[d];
    directory[d] = 0;
    if (!directory[0]) {
        directory[0] = 'C'; directory[1] = ':';
        directory[2] = '\\'; directory[3] = 0;
    }
    unsigned p = 0, source = separator;
    while (path[source] == '\\' || path[source] == '/') source++;
    while (path[source] && p + 1 < sizeof(pattern))
        pattern[p++] = path[source++];
    pattern[p] = 0;
    if (!pattern[0]) { pattern[0] = '*'; pattern[1] = 0; }

    write_s(" Volume in drive C is SVINDOWS\r\n Directory of ");
    write_s(g_current_directory); write_s("\r\n\r\n");
    CMD_FIND_DATA data;
    DWORD index = 0;
    while (NtEnumerateDirectorySimple(directory, index++, &data) >= 0) {
        if (!dir_wildcard_match(pattern, data.Name)) continue;
        if (data.Attributes & 0x10) {
            write_s("             <DIR>  ");
            directories++;
        } else {
            write_u64_width(data.Size, 18);
            write_s("  ");
            files++;
            bytes += data.Size;
        }
        write_s(data.Name);
        write_s("\r\n");
    }
    if (!files && !directories) write_s("File Not Found\r\n");
    write_s("               ");
    write_u64(files);
    write_s(" File(s)  ");
    write_u64(bytes);
    write_s(" bytes\r\n");
    write_s("               ");
    write_u64(directories);
    write_s(" Dir(s)\r\n");
}

static void command_type(char *arg)
{
    arg = skip_spaces(arg);
    if (!*arg || same(arg, "/?")) {
        write_s("Displays the contents of a text file.\r\n\r\nTYPE filename\r\n");
        return;
    }
    char path[260];
    if (!normalize_path(arg, path)) {
        write_s("The system cannot find the path specified.\r\n");
        return;
    }
    HANDLE file = CreateFileA(path, 0x80000000u, 1, 0, 3, 0, 0);
    if (file == INVALID_HANDLE_VALUE) {
        write_s("The system cannot find the file specified.\r\n");
        return;
    }
    char buf[512];
    DWORD got;
    while (ReadFile(file, buf, sizeof(buf), &got, 0) && got) {
        /* Keep console writes modest.  Besides matching the way ordinary
         * stdio flushes text, this avoids making TYPE depend on a backend's
         * maximum contiguous console-write size. */
        DWORD offset = 0;
        while (offset < got) {
            DWORD chunk = got - offset;
            if (chunk > 64) chunk = 64;
            write_n(buf + offset, chunk);
            offset += chunk;
        }
    }
    CloseHandle(file);
    write_s("\r\n");
}

static void command_cd(char *arg)
{
    arg = skip_spaces(arg);
    trim_right(arg);
    if (!*arg) {
        write_s(g_current_directory); write_s("\r\n");
        return;
    }
    char path[260];
    if (normalize_path(arg, path) &&
        (GetFileAttributesA(path) & 0x10) != 0 &&
        GetFileAttributesA(path) != (DWORD)-1) {
        unsigned i = 0;
        while ((g_current_directory[i] = path[i])) i++;
        return;
    }
    write_s("The system cannot find the path specified.\r\n");
}

static void command_help(void)
{
    write_s(
        "CD      Displays or changes the current directory.\r\n"
        "CLS     Clears the screen.\r\n"
        "DIR     Displays files in the current directory.\r\n"
        "COPY    Copies a file.\r\n"
        "DEL     Deletes a file.\r\n"
        "ECHO    Displays messages, or turns command echoing on/off.\r\n"
        "EXIT    Exits the command interpreter.\r\n"
        "HELP    Displays this help.\r\n"
        "HISTORY Displays the command history.\r\n"
        "HOSTNAME Displays the computer name.\r\n"
        "PAUSE   Waits for a key press.\r\n"
        "SET     Displays or changes environment variables.\r\n"
        "REM     Records a comment in a command line.\r\n"
        "TYPE    Displays the contents of a text file.\r\n"
        "MD/RD   Creates or removes a directory.\r\n"
        "REN     Renames a file or directory.\r\n"
        "VER     Displays the operating system version.\r\n"
        "VOL     Displays the disk volume label.\r\n");
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
    if (!CreateProcessA(image, command_line, 0, 0, 1, 0, 0, 0,
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

    if (same(command, "dir")) command_dir(arg);
    else if (same(command, "type")) command_type(arg);
    else if (same(command, "cd") || same(command, "chdir")) command_cd(arg);
    else if (same(command, "cls")) write_s("\f");
    else if (same(command, "ver")) write_s("Svindows [Version 0.6.0]\r\n");
    else if (same(command, "vol")) write_s("Volume in drive C is SVINDOWS\r\n");
    else if (same(command, "hostname")) write_s("SVINDOWS64\r\n");
    else if (same(command, "help")) command_help();
    else if (same(command, "history") || same(command, "doskey")) command_history();
    else if (same(command, "set")) command_set(arg);
    else if (same(command, "if")) command_if(arg);
    else if (same(command, "rem") || command[0] == ':') { }
    else if (same(command, "echo")) {
        if (!*arg) write_s(g_echo ? "ECHO is on.\r\n" : "ECHO is off.\r\n");
        else if (same(arg, "on")) g_echo = 1;
        else if (same(arg, "off")) g_echo = 0;
        else { write_s(arg); write_s("\r\n"); }
    } else if (same(command, "pause")) {
        char c; DWORD got;
        write_s("Press any key to continue . . .");
        ReadFile(g_in, &c, 1, &got, 0);
        write_s("\r\n");
    } else if (same(command, "exit")) return 0;
    else if (same(command, "copy")) command_copy(arg);
    else if (same(command, "del") || same(command, "erase") ||
             same(command, "md") || same(command, "mkdir") ||
             same(command, "rd") || same(command, "rmdir") ||
             same(command, "ren") || same(command, "rename")) {
        command_path_mutation(command, arg);
    } else {
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
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_err = g_out;
    write_s("Svindows Command Processor [Version 0.6.0]\r\n"
            "Type HELP for a list of commands.\r\n\r\n");
    for (;;) {
        if (g_echo) { write_s(g_current_directory); write_s(">"); }
        if (!read_line(line, sizeof(line))) break;
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
