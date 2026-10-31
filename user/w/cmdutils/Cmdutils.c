/* Standalone console commands formerly embedded in cmd.exe. */
typedef void              *HANDLE;
typedef void              *LPVOID;
typedef unsigned char      BYTE;
typedef unsigned short     WCHAR;
typedef unsigned long      DWORD;
typedef unsigned long long ULONGLONG;
typedef int                BOOL;

#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INVALID_HANDLE_VALUE ((HANDLE)(ULONGLONG)-1)

typedef struct _FILETIME { DWORD Low, High; } FILETIME;
typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    DWORD dwReserved0, dwReserved1;
    WCHAR cFileName[260];
    WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW;

__declspec(dllimport) HANDLE GetStdHandle(DWORD);
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
__declspec(dllimport) BOOL DeleteFileA(const char *);
__declspec(dllimport) BOOL MoveFileA(const char *, const char *);
__declspec(dllimport) BOOL CreateDirectoryA(const char *, LPVOID);
__declspec(dllimport) BOOL RemoveDirectoryA(const char *);
__declspec(dllimport) DWORD GetFileAttributesA(const char *);
__declspec(dllimport) DWORD GetCurrentDirectoryW(DWORD, WCHAR *);

typedef struct _SYSTEM_INFO_K {
    DWORD dwOemId;
    DWORD dwPageSize;
    void *lpMinimumApplicationAddress;
    ULONGLONG lpMaximumApplicationAddress;
    ULONGLONG dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WCHAR wProcessorLevel;
    WCHAR wProcessorRevision;
} SYSTEM_INFO_K;
__declspec(dllimport) void GetSystemInfo(SYSTEM_INFO_K *);

static HANDLE g_in, g_out;

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

static const char *command_name(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    return name;
}

static int command_is(const char *name, const char *wanted)
{
    unsigned n = slen(name), w = slen(wanted);
    if (n == w) return same(name, wanted);
    if (n != w + 4 || !same(name + w, ".exe")) return 0;
    for (unsigned i = 0; i < w; i++)
        if (lower(name[i]) != lower(wanted[i])) return 0;
    return 1;
}

static void write_u64(ULONGLONG value)
{
    char buf[32];
    unsigned pos = sizeof(buf);
    do { buf[--pos] = (char)('0' + value % 10); value /= 10; } while (value);
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

static int wide_to_utf8(const WCHAR *text, char *out, unsigned cap)
{
    unsigned in = 0, n = 0;
    while (text[in]) {
        unsigned cp = text[in++];
        if (cp >= 0xd800 && cp <= 0xdbff &&
            text[in] >= 0xdc00 && text[in] <= 0xdfff)
            cp = 0x10000 + ((cp - 0xd800) << 10) + (text[in++] - 0xdc00);
        unsigned need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (n + need >= cap) return 0;
        if (need == 1) out[n++] = (char)cp;
        else if (need == 2) {
            out[n++] = (char)(0xc0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 0x3f));
        } else if (need == 3) {
            out[n++] = (char)(0xe0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[n++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[n++] = (char)(0xf0 | (cp >> 18));
            out[n++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[n++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    out[n] = 0;
    return 1;
}

static int utf8_to_wide(const char *text, WCHAR *wide, unsigned cap)
{
    unsigned out = 0;
    while (*text) {
        const BYTE *s = (const BYTE *)text;
        unsigned cp, count;
        if (s[0] < 0x80) { cp = s[0]; count = 1; }
        else if ((s[0] & 0xe0) == 0xc0) { cp = s[0] & 0x1f; count = 2; }
        else if ((s[0] & 0xf0) == 0xe0) { cp = s[0] & 0x0f; count = 3; }
        else if ((s[0] & 0xf8) == 0xf0) { cp = s[0] & 7; count = 4; }
        else return 0;
        for (unsigned i = 1; i < count; i++) {
            if (!s[i] || (s[i] & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (s[i] & 0x3f);
        }
        text += count;
        if (cp < 0x10000) {
            if (out + 1 >= cap) return 0;
            wide[out++] = (WCHAR)cp;
        } else {
            if (out + 2 >= cap) return 0;
            cp -= 0x10000;
            wide[out++] = (WCHAR)(0xd800 + (cp >> 10));
            wide[out++] = (WCHAR)(0xdc00 + (cp & 0x3ff));
        }
    }
    wide[out] = 0;
    return 1;
}

static int current_directory(char out[260])
{
    WCHAR wide[260];
    DWORD n = GetCurrentDirectoryW(260, wide);
    return n && n < 260 && wide_to_utf8(wide, out, 260);
}

static int normalize_path(const char *input, char output[260])
{
    if (!input || !*input) return 0;
    char joined[520], cwd[260];
    unsigned j = 0, p = 0;
    int absolute = input[0] == '\\' || input[0] == '/' || input[1] == ':';
    if (input[1] == ':' && lower(input[0]) != 'c') return 0;
    if (!absolute) {
        if (!current_directory(cwd)) return 0;
        while (cwd[p] && j + 1 < sizeof(joined)) joined[j++] = cwd[p++];
        if (j && joined[j - 1] != '\\') joined[j++] = '\\';
    }
    p = input[1] == ':' ? 2 : 0;
    while (input[p] == '\\' || input[p] == '/') p++;
    if (absolute) { joined[j++] = 'C'; joined[j++] = ':'; joined[j++] = '\\'; }
    while (input[p] && j + 1 < sizeof(joined)) joined[j++] = input[p++];
    joined[j] = 0;

    unsigned n = 0;
    output[n++] = 'C'; output[n++] = ':'; output[n++] = '\\';
    p = joined[1] == ':' ? 2 : 0;
    while (joined[p] == '\\' || joined[p] == '/') p++;
    while (joined[p]) {
        char part[64]; unsigned count = 0;
        while (joined[p] && joined[p] != '\\' && joined[p] != '/') {
            if (count + 1 >= sizeof(part)) return 0;
            part[count++] = joined[p++];
        }
        part[count] = 0;
        while (joined[p] == '\\' || joined[p] == '/') p++;
        if (!count || (count == 1 && part[0] == '.')) continue;
        if (count == 2 && part[0] == '.' && part[1] == '.') {
            if (n > 3) {
                if (output[n - 1] == '\\') n--;
                while (n > 3 && output[n - 1] != '\\') n--;
            }
            continue;
        }
        if (n > 3 && output[n - 1] != '\\') output[n++] = '\\';
        for (unsigned i = 0; i < count && n + 1 < 260; i++)
            output[n++] = part[i];
    }
    output[n] = 0;
    return 1;
}

static void print_wide(const WCHAR *text)
{
    char utf8[1040];
    if (wide_to_utf8(text, utf8, sizeof(utf8))) write_s(utf8);
}

static int run_dir(int argc, char **argv)
{
    const char *requested = argc > 1 ? argv[1] : "*";
    if (same(requested, "/?")) {
        write_s("Displays files in a directory.\r\n\r\nDIR [pattern]\r\n");
        return 0;
    }
    char path[260], search[260], directory[260];
    if (!normalize_path(requested, path)) {
        write_s("The system cannot find the path specified.\r\n");
        return 1;
    }
    DWORD attributes = GetFileAttributesA(path);
    if (attributes != (DWORD)-1 && (attributes & 0x10)) {
        unsigned i = 0;
        while (path[i] && i + 3 < sizeof(search)) {
            search[i] = directory[i] = path[i]; i++;
        }
        directory[i] = 0;
        if (i && search[i - 1] != '\\') search[i++] = '\\';
        search[i++] = '*'; search[i] = 0;
    } else {
        unsigned separator = 0;
        for (unsigned i = 0; path[i]; i++)
            if (path[i] == '\\' || path[i] == '/') separator = i;
        unsigned d = 0;
        for (; d <= separator && path[d] && d + 1 < sizeof(directory); d++)
            directory[d] = path[d];
        directory[d] = 0;
        unsigned i = 0;
        while (path[i] && i + 1 < sizeof(search)) { search[i] = path[i]; i++; }
        search[i] = 0;
    }

    write_s(" Volume in drive C is SVINDOWS\r\n Directory of ");
    write_s(directory); write_s("\r\n\r\n");
    WCHAR wide_search[260];
    WIN32_FIND_DATAW data;
    HANDLE find = utf8_to_wide(search, wide_search, 260)
                    ? FindFirstFileExW(wide_search, 0, &data, 0, 0, 0)
                    : INVALID_HANDLE_VALUE;
    DWORD files = 0, directories = 0;
    ULONGLONG bytes = 0;
    while (find != INVALID_HANDLE_VALUE) {
        if (data.dwFileAttributes & 0x10) {
            write_s("             <DIR>  "); directories++;
        } else {
            ULONGLONG size = ((ULONGLONG)data.nFileSizeHigh << 32) |
                             data.nFileSizeLow;
            write_u64_width(size, 18); write_s("  ");
            files++; bytes += size;
        }
        print_wide(data.cFileName); write_s("\r\n");
        if (!FindNextFileW(find, &data)) break;
    }
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
    if (!files && !directories) write_s("File Not Found\r\n");
    write_s("               "); write_u64(files);
    write_s(" File(s)  "); write_u64(bytes); write_s(" bytes\r\n");
    write_s("               "); write_u64(directories); write_s(" Dir(s)\r\n");
    return files || directories ? 0 : 1;
}

static int run_type(int argc, char **argv)
{
    if (argc < 2 || same(argv[1], "/?")) {
        write_s("Displays a text file.\r\n\r\nTYPE filename\r\n");
        return argc < 2;
    }
    char path[260];
    if (!normalize_path(argv[1], path)) return 1;
    HANDLE file = CreateFileA(path, 0x80000000u, 1, 0, 3, 0, 0);
    if (file == INVALID_HANDLE_VALUE) {
        write_s("The system cannot find the file specified.\r\n");
        return 1;
    }
    char buffer[512]; DWORD got, done;
    BOOL ok = 1;
    while (ReadFile(file, buffer, sizeof(buffer), &got, 0) && got) {
        DWORD offset = 0;
        while (offset < got) {
            DWORD chunk = got - offset;
            if (chunk > 64) chunk = 64;
            if (!WriteFile(g_out, buffer + offset, chunk, &done, 0)) {
                ok = 0; break;
            }
            offset += done;
        }
        if (!ok) break;
    }
    CloseHandle(file);
    return ok ? 0 : 1;
}

static int run_copy(int argc, char **argv)
{
    if (argc < 3) {
        write_s("The syntax of the command is incorrect.\r\n"); return 1;
    }
    char source[260], destination[260];
    if (!normalize_path(argv[1], source) || !normalize_path(argv[2], destination))
        return 1;
    HANDLE in = CreateFileA(source, 0x80000000u, 1, 0, 3, 0, 0);
    HANDLE out = CreateFileA(destination, 0x40000000u, 0, 0, 2, 0, 0);
    if (in == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE) {
        if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        write_s("The system cannot find the file specified.\r\n"); return 1;
    }
    char buffer[1024]; DWORD got, done; BOOL ok = 1;
    while (ReadFile(in, buffer, sizeof(buffer), &got, 0) && got)
        if (!WriteFile(out, buffer, got, &done, 0) || done != got) {
            ok = 0; break;
        }
    CloseHandle(in); CloseHandle(out);
    write_s(ok ? "        1 file(s) copied.\r\n" : "Copy failed.\r\n");
    return ok ? 0 : 1;
}

static int run_mutation(const char *command, int argc, char **argv)
{
    if (argc < 2 || ((command_is(command, "ren") ||
                      command_is(command, "rename")) && argc < 3)) {
        write_s("The syntax of the command is incorrect.\r\n"); return 1;
    }
    char first[260], second[260];
    BOOL ok = normalize_path(argv[1], first);
    if (argc > 2) ok = ok && normalize_path(argv[2], second);
    if (ok && (command_is(command, "del") || command_is(command, "erase")))
        ok = DeleteFileA(first);
    else if (ok && (command_is(command, "md") || command_is(command, "mkdir")))
        ok = CreateDirectoryA(first, 0);
    else if (ok && (command_is(command, "rd") || command_is(command, "rmdir")))
        ok = RemoveDirectoryA(first);
    else if (ok) ok = MoveFileA(first, second);
    if (!ok) write_s("The filesystem operation failed.\r\n");
    return ok ? 0 : 1;
}

static void uname_field(const char *value, int *printed)
{
    if (*printed) write_s(" ");
    write_s(value);
    *printed = 1;
}

static int run_uname(int argc, char **argv)
{
    int sysname = 0, nodename = 0, release = 0, version = 0;
    int machine = 0, operating_system = 0, all = 0;
    if (argc <= 1) {
        write_s("Svindows\r\n");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (same(arg, "--help")) {
            write_s("Usage: uname [OPTION]...\r\n"
                    "  -a, --all           print all system information\r\n"
                    "  -s, --kernel-name   print the kernel name\r\n"
                    "  -n, --nodename      print the network node hostname\r\n"
                    "  -r, --kernel-release print the kernel release\r\n"
                    "  -v, --kernel-version print the kernel version\r\n"
                    "  -m, --machine       print the machine hardware name\r\n"
                    "  -o, --operating-system print the operating system\r\n");
            return 0;
        }
        if (same(arg, "--version")) {
            write_s("uname (Svindows core utilities) 1.0\r\n");
            return 0;
        }
        if (same(arg, "--all")) { all = 1; continue; }
        if (same(arg, "--kernel-name")) { sysname = 1; continue; }
        if (same(arg, "--nodename")) { nodename = 1; continue; }
        if (same(arg, "--kernel-release")) { release = 1; continue; }
        if (same(arg, "--kernel-version")) { version = 1; continue; }
        if (same(arg, "--machine")) { machine = 1; continue; }
        if (same(arg, "--operating-system")) { operating_system = 1; continue; }
        if (arg[0] != '-' || !arg[1]) {
            write_s("uname: invalid option or operand: "); write_s(arg); write_s("\r\n");
            return 1;
        }
        for (unsigned j = 1; arg[j]; j++) {
            switch (arg[j]) {
                case 'a': all = 1; break;
                case 's': sysname = 1; break;
                case 'n': nodename = 1; break;
                case 'r': release = 1; break;
                case 'v': version = 1; break;
                case 'm': machine = 1; break;
                case 'o': operating_system = 1; break;
                default:
                    write_s("uname: invalid option -- '");
                    write_n(&arg[j], 1); write_s("'\r\n"); return 1;
            }
        }
    }
    if (all) sysname = nodename = release = version = machine = operating_system = 1;
    if (!(sysname || nodename || release || version || machine || operating_system))
        sysname = 1;
    int printed = 0;
    if (sysname) uname_field("Svindows", &printed);
    if (nodename) uname_field("svindows", &printed);
    if (release) uname_field("0.6.0", &printed);
    if (version) uname_field("Svindows 0.6.0", &printed);
    if (machine) uname_field("x86_64", &printed);
    if (operating_system) uname_field("Svindows", &printed);
    write_s("\r\n");
    return 0;
}

static int run_help(void)
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
        "START   Starts a program without waiting for it to exit.\r\n"
        "TERMINAL Opens another independent X terminal window.\r\n"
        "SHELL   Supports pipes, redirection, ;, &, && and ||.\r\n"
        "TAB     Completes a matching name in the current directory.\r\n"
        "CTRL+C  Cancels the current command-line input.\r\n"
        "REM     Records a comment in a command line.\r\n"
        "TYPE    Displays the contents of a text file.\r\n"
        "UNAME   Displays system and kernel information.\r\n"
        "FASTFETCH Runs the Svindows system information utility.\r\n"
        "MD/RD   Creates or removes a directory.\r\n"
        "REN     Renames a file or directory.\r\n"
        "VER     Displays the operating system version.\r\n"
        "VOL     Displays the disk volume label.\r\n");
    return 0;
}

int main(int argc, char **argv)
{
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    const char *command = command_name(argc ? argv[0] : "");
    if (command_is(command, "dir")) return run_dir(argc, argv);
    if (command_is(command, "type")) return run_type(argc, argv);
    if (command_is(command, "copy")) return run_copy(argc, argv);
    if (command_is(command, "help")) return run_help();
    if (command_is(command, "uname")) return run_uname(argc, argv);
    if (command_is(command, "del") || command_is(command, "erase") ||
        command_is(command, "md") || command_is(command, "mkdir") ||
        command_is(command, "rd") || command_is(command, "rmdir") ||
        command_is(command, "ren") || command_is(command, "rename"))
        return run_mutation(command, argc, argv);
    if (command_is(command, "cls")) { write_s("\f"); return 0; }
    if (command_is(command, "ver")) {
        write_s("Svindows [Version 0.6.0]\r\n"); return 0;
    }
    if (command_is(command, "vol")) {
        write_s("Volume in drive C is SVINDOWS\r\n"); return 0;
    }
    if (command_is(command, "hostname")) { write_s("SVINDOWS64\r\n"); return 0; }
    if (command_is(command, "pause")) {
        char c; DWORD got;
        write_s("Press any key to continue . . .");
        ReadFile(g_in, &c, 1, &got, 0); write_s("\r\n"); return 0;
    }
    write_s("Unsupported command image.\r\n");
    return 1;
}
