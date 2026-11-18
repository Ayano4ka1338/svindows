/* Additional, standalone BusyBox-style applets. Existing command applets stay
 * in Cmdutils.c; this file only implements commands that had no equivalent. */
typedef struct _SVBOX_SYSTEMTIME {
    unsigned short Year, Month, DayOfWeek, Day;
    unsigned short Hour, Minute, Second, Milliseconds;
} SVBOX_SYSTEMTIME;

__declspec(dllimport) void GetLocalTime(SVBOX_SYSTEMTIME *time);
__declspec(dllimport) void Sleep(DWORD milliseconds);

static int svbox_is(const char *name, const char *wanted)
{
    return command_is(name, wanted);
}

static int svbox_uint(const char *text, unsigned *value)
{
    unsigned n = 0;
    if (!*text) return 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || n > 100000000u) return 0;
        n = n * 10u + (unsigned)(*text - '0');
    }
    *value = n;
    return 1;
}

static int svbox_sint(const char *text, long long *value)
{
    int negative = 0;
    unsigned long long n = 0;
    if (*text == '-' || *text == '+') negative = *text++ == '-';
    if (!*text) return 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return 0;
        unsigned digit = (unsigned)(*text - '0');
        if (n > (0x7fffffffffffffffULL - digit) / 10u) return 0;
        n = n * 10u + digit;
    }
    *value = negative ? -(long long)n : (long long)n;
    return 1;
}

static void svbox_copy(char *dst, const char *src, unsigned capacity)
{
    unsigned i = 0;
    if (!capacity) return;
    while (src[i] && i + 1 < capacity) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

static void svbox_zero2(unsigned value)
{
    if (value < 10) write_s("0");
    write_u64(value);
}

static int svbox_equal_n(const char *a, const char *b, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

static int svbox_basename(int argc, char **argv, int dir)
{
    if (argc < 2) {
        write_s(dir ? "Usage: dirname PATH\r\n" : "Usage: basename PATH [SUFFIX]\r\n");
        return 1;
    }
    const char *path = argv[1];
    unsigned n = slen(path);
    if (dir && n == 3 && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\\')) {
        write_n(path, 3); write_s("\r\n"); return 0;
    }
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) --n;
    unsigned start = n;
    while (start && path[start - 1] != '/' && path[start - 1] != '\\') --start;
    if (!dir) {
        unsigned end = n;
        if (argc > 2) {
            unsigned suffix = slen(argv[2]);
            if (suffix <= end - start && suffix &&
                svbox_equal_n(path + end - suffix, argv[2], suffix)) end -= suffix;
        }
        if (end == start) write_s("/\r\n");
        else { write_n(path + start, end - start); write_s("\r\n"); }
        return 0;
    }
    unsigned slash = n;
    while (slash && path[slash - 1] != '/' && path[slash - 1] != '\\') --slash;
    if (!slash) { write_s(".\r\n"); return 0; }
    unsigned end = slash - 1;
    while (end && (path[end - 1] == '/' || path[end - 1] == '\\')) --end;
    if (!end) { write_n(path, 1); write_s("\r\n"); }
    else { write_n(path, end); write_s("\r\n"); }
    return 0;
}

static int svbox_date(int argc, char **argv)
{
    SVBOX_SYSTEMTIME t;
    GetLocalTime(&t);
    if (argc > 1 && argv[1][0] == '+') {
        const char *f = argv[1] + 1;
        while (*f) {
            if (*f++ != '%') { write_n(f - 1, 1); continue; }
            char number[5]; unsigned value = 0, width = 2;
            switch (*f++) {
            case '%': write_s("%"); continue;
            case 'Y': value = t.Year; width = 4; break;
            case 'm': value = t.Month; break;
            case 'd': value = t.Day; break;
            case 'H': value = t.Hour; break;
            case 'M': value = t.Minute; break;
            case 'S': value = t.Second; break;
            default: write_s("date: unsupported format\r\n"); return 1;
            }
            for (unsigned i = 0; i < width; ++i) {
                number[width - i - 1] = (char)('0' + value % 10u);
                value /= 10u;
            }
            write_n(number, width);
        }
        write_s("\r\n");
    } else {
        write_u64(t.Year); write_s("-");
        svbox_zero2(t.Month); write_s("-");
        svbox_zero2(t.Day); write_s(" ");
        svbox_zero2(t.Hour); write_s(":");
        svbox_zero2(t.Minute); write_s(":");
        svbox_zero2(t.Second); write_s("\r\n");
    }
    return 0;
}

static int svbox_sleep(int argc, char **argv)
{
    unsigned seconds;
    if (argc != 2 || !svbox_uint(argv[1], &seconds) || seconds > 4294967u) {
        write_s("Usage: sleep SECONDS\r\n");
        return 1;
    }
    Sleep(seconds * 1000u);
    return 0;
}

static int svbox_seq(int argc, char **argv)
{
    long long first = 1, step = 1, last;
    if (argc < 2 || argc > 4) {
        write_s("Usage: seq [FIRST [INCREMENT]] LAST\r\n"); return 1;
    }
    if (argc == 2) {
        if (!svbox_sint(argv[1], &last)) { write_s("seq: invalid number\r\n"); return 1; }
    }
    else {
        if (!svbox_sint(argv[1], &first)) { write_s("seq: invalid number\r\n"); return 1; }
        if (argc == 3) {
            if (!svbox_sint(argv[2], &last)) { write_s("seq: invalid number\r\n"); return 1; }
        }
        else {
            if (!svbox_sint(argv[2], &step) || !step) { write_s("seq: invalid increment\r\n"); return 1; }
            if (!svbox_sint(argv[3], &last)) { write_s("seq: invalid number\r\n"); return 1; }
        }
    }
    if ((step > 0 && first > last) || (step < 0 && first < last)) return 0;
    for (long long n = first; step > 0 ? n <= last : n >= last; n += step) {
        if (n < 0) { write_s("-"); write_u64((unsigned long long)(-(n + 1)) + 1); }
        else write_u64((unsigned long long)n);
        write_s("\r\n");
        if ((step > 0 && n > 0x7fffffffffffffffLL - step) ||
            (step < 0 && n < (-0x7fffffffffffffffLL - 1) - step)) break;
    }
    return 0;
}

static int svbox_white(unsigned char c)
{ return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

static int svbox_wc_stream(HANDLE input, const char *name,
                           unsigned long long *lines,
                           unsigned long long *words,
                           unsigned long long *bytes)
{
    char buffer[512]; DWORD got;
    int in_word = 0;
    while (ReadFile(input, buffer, sizeof(buffer), &got, 0) && got) {
        *bytes += got;
        for (DWORD i = 0; i < got; ++i) {
            unsigned char c = (unsigned char)buffer[i];
            if (c == '\n') ++*lines;
            if (svbox_white(c)) in_word = 0;
            else if (!in_word) { ++*words; in_word = 1; }
        }
    }
    (void)name;
    return 0;
}

static int svbox_wc(int argc, char **argv)
{
    int count_lines = 0, count_words = 0, count_bytes = 0, first = 1;
    unsigned long long tl = 0, tw = 0, tb = 0;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (same(argv[first], "--help")) {
            write_s("Usage: wc [-lwmc] [FILE...]\r\n"); return 0;
        }
        for (unsigned j = 1; argv[first][j]; ++j) {
            switch (argv[first][j]) {
            case 'l': count_lines = 1; break;
            case 'w': count_words = 1; break;
            case 'c': case 'm': count_bytes = 1; break;
            default: write_s("wc: invalid option\r\n"); return 1;
            }
        }
    }
    if (!count_lines && !count_words && !count_bytes)
        count_lines = count_words = count_bytes = 1;
    int files = argc - first;
    if (!files) files = 1;
    for (int f = 0; f < files; ++f) {
        const char *name = argc == first ? "-" : argv[first + f];
        HANDLE input = g_in;
        if (!same(name, "-")) {
            char path[260];
            if (!normalize_path(name, path) ||
                (input = CreateFileA(path, 0x80000000u, 1, 0, 3, 0, 0)) == INVALID_HANDLE_VALUE) {
                write_s("wc: cannot open "); write_s(name); write_s("\r\n"); return 1;
            }
        }
        unsigned long long l = 0, w = 0, b = 0;
        svbox_wc_stream(input, name, &l, &w, &b);
        if (!same(name, "-")) CloseHandle(input);
        if (count_lines) { write_u64_width(l, 8); write_s(" "); }
        if (count_words) { write_u64_width(w, 8); write_s(" "); }
        if (count_bytes) { write_u64_width(b, 8); write_s(" "); }
        if (!same(name, "-")) { write_s(name); }
        write_s("\r\n"); tl += l; tw += w; tb += b;
    }
    if (argc - first > 1) {
        if (count_lines) { write_u64_width(tl, 8); write_s(" "); }
        if (count_words) { write_u64_width(tw, 8); write_s(" "); }
        if (count_bytes) { write_u64_width(tb, 8); write_s(" "); }
        write_s("total\r\n");
    }
    return 0;
}

#define SVBOX_LINE_MAX 2048
static unsigned svbox_tail_lengths[1024];
static char svbox_uniq_previous[SVBOX_LINE_MAX];
static char svbox_uniq_line[SVBOX_LINE_MAX];
static int svbox_read_line(HANDLE input, char line[SVBOX_LINE_MAX], unsigned *length)
{
    DWORD got = 0; char c; *length = 0;
    while (ReadFile(input, &c, 1, &got, 0) && got) {
        if (c == '\n') break;
        if (*length + 1 < SVBOX_LINE_MAX) line[(*length)++] = c;
    }
    if (!got && !*length) return 0;
    if (*length && line[*length - 1] == '\r') --*length;
    line[*length] = 0;
    return 1;
}

static HANDLE svbox_open_input(const char *name)
{
    char path[260];
    if (!name || same(name, "-")) return g_in;
    if (!normalize_path(name, path)) return INVALID_HANDLE_VALUE;
    return CreateFileA(path, 0x80000000u, 1, 0, 3, 0, 0);
}

static void svbox_close_input(HANDLE input, const char *name)
{ if (input != g_in && !same(name, "-")) CloseHandle(input); }

static int svbox_head_tail(int argc, char **argv, int tail)
{
    unsigned limit = 10; int at = 1;
    if (at < argc && argv[at][0] == '-' && argv[at][1] == 'n') {
        const char *n = argv[at] + 2;
        if (!*n) { if (++at >= argc) { write_s("head/tail: missing line count\r\n"); return 1; } n = argv[at]; }
        if (!svbox_uint(n, &limit) || limit > 10000) { write_s("head/tail: invalid line count\r\n"); return 1; }
        ++at;
    }
    int files = argc - at; if (!files) files = 1;
    for (int f = 0; f < files; ++f) {
        const char *name = argc == at ? "-" : argv[at + f];
        HANDLE input = svbox_open_input(name);
        if (input == INVALID_HANDLE_VALUE) { write_s("head/tail: cannot open "); write_s(name); write_s("\r\n"); return 1; }
        if (files > 1) { if (f) write_s("\r\n"); write_s("==> "); write_s(name); write_s(" <==\r\n"); }
        if (!tail) {
            char line[SVBOX_LINE_MAX]; unsigned n, seen = 0;
            while (seen < limit && svbox_read_line(input, line, &n)) {
                write_n(line, n); write_s("\r\n"); ++seen;
            }
        } else {
            static char ring[1024][SVBOX_LINE_MAX];
            if (limit > 1024) { svbox_close_input(input, name); write_s("tail: line count limited to 1024\r\n"); return 1; }
            unsigned capacity = limit ? limit : 1, used = 0, next = 0, n;
            char line[SVBOX_LINE_MAX];
            while (svbox_read_line(input, line, &n)) {
                if (!limit) continue;
                unsigned copy = n < SVBOX_LINE_MAX - 1 ? n : SVBOX_LINE_MAX - 1;
                for (unsigned j = 0; j < copy; ++j) ring[next][j] = line[j];
                ring[next][copy] = 0;
                svbox_tail_lengths[next] = copy; next = (next + 1) % capacity;
                if (used < capacity) ++used;
            }
            unsigned begin = used == capacity ? next : 0;
            for (unsigned i = 0; i < used; ++i) {
                unsigned k = (begin + i) % capacity;
                write_n(ring[k], svbox_tail_lengths[k]); write_s("\r\n");
            }
        }
        svbox_close_input(input, name);
    }
    return 0;
}

static int svbox_contains(const char *line, unsigned length,
                          const char *needle, int ignore_case)
{
    unsigned n = slen(needle);
    if (!n) return 1;
    for (unsigned i = 0; i + n <= length; ++i) {
        unsigned j = 0;
        while (j < n && (ignore_case ? lower(line[i + j]) == lower(needle[j]) : line[i + j] == needle[j])) ++j;
        if (j == n) return 1;
    }
    return 0;
}

static int svbox_grep(int argc, char **argv)
{
    int ignore_case = 0, invert = 0, line_number = 0, count_only = 0;
    int at = 1;
    while (at < argc && argv[at][0] == '-' && argv[at][1]) {
        if (same(argv[at], "--help")) { write_s("Usage: grep [-inv] PATTERN [FILE...]\r\n"); return 0; }
        for (unsigned j = 1; argv[at][j]; ++j) {
            if (argv[at][j] == 'i') ignore_case = 1;
            else if (argv[at][j] == 'n') line_number = 1;
            else if (argv[at][j] == 'v') invert = 1;
            else if (argv[at][j] == 'c') count_only = 1;
            else { write_s("grep: invalid option\r\n"); return 2; }
        }
        ++at;
    }
    if (at >= argc) { write_s("Usage: grep PATTERN [FILE...]\r\n"); return 2; }
    const char *pattern = argv[at++]; int files = argc - at; if (!files) files = 1;
    unsigned long long total = 0;
    for (int f = 0; f < files; ++f) {
        const char *name = argc == at ? "-" : argv[at + f];
        HANDLE input = svbox_open_input(name);
        if (input == INVALID_HANDLE_VALUE) { write_s("grep: cannot open "); write_s(name); write_s("\r\n"); return 2; }
        char line[SVBOX_LINE_MAX]; unsigned n, lineno = 0; unsigned long long matches = 0;
        while (svbox_read_line(input, line, &n)) {
            ++lineno; int found = svbox_contains(line, n, pattern, ignore_case);
            if (invert) found = !found;
            if (!found) continue;
            ++matches;
            if (!count_only) {
                if (files > 1) { write_s(name); write_s(":"); }
                if (line_number) { write_u64(lineno); write_s(":"); }
                write_n(line, n); write_s("\r\n");
            }
        }
        if (count_only) { if (files > 1) { write_s(name); write_s(":"); } write_u64(matches); write_s("\r\n"); }
        total += matches; svbox_close_input(input, name);
    }
    return total ? 0 : 1;
}

static int svbox_cut(int argc, char **argv)
{
    char delimiter = '\t'; const char *fields = 0; int at = 1;
    while (at < argc && argv[at][0] == '-') {
        if ((argv[at][1] == 'd' || argv[at][1] == 'f') && argv[at][2]) {
            if (argv[at][1] == 'd') delimiter = argv[at][2]; else fields = argv[at] + 2;
        } else if (argv[at][1] == 'd' || argv[at][1] == 'f') {
            char option = argv[at++][1];
            if (at >= argc) { write_s("cut: option requires an argument\r\n"); return 1; }
            if (option == 'd') delimiter = argv[at][0]; else fields = argv[at];
        } else { write_s("Usage: cut -d DELIM -f LIST [FILE...]\r\n"); return 1; }
        ++at;
    }
    if (!fields) { write_s("Usage: cut -d DELIM -f LIST [FILE...]\r\n"); return 1; }
    int files = argc - at; if (!files) files = 1;
    for (int f = 0; f < files; ++f) {
        const char *name = argc == at ? "-" : argv[at + f];
        HANDLE input = svbox_open_input(name);
        if (input == INVALID_HANDLE_VALUE) { write_s("cut: cannot open "); write_s(name); write_s("\r\n"); return 1; }
        char line[SVBOX_LINE_MAX]; unsigned n;
        while (svbox_read_line(input, line, &n)) {
            unsigned field = 1, start = 0;
            int printed = 0;
            for (unsigned i = 0; i <= n; ++i) {
                if (i != n && line[i] != delimiter) continue;
                int selected = 0; const char *p = fields;
                while (*p) {
                    unsigned wanted;
                    if (!svbox_uint(p, &wanted)) break;
                    if (wanted == field) selected = 1;
                    while (*p && *p != ',') ++p;
                    if (*p == ',') ++p;
                }
                if (selected) { if (printed) write_n(&delimiter, 1); write_n(line + start, i - start); printed = 1; }
                start = i + 1; ++field;
            }
            write_s("\r\n");
        }
        svbox_close_input(input, name);
    }
    return 0;
}

#define SVBOX_SORT_LINES 512
static int svbox_sort(int argc, char **argv)
{
    int reverse = 0, fold = 0, at = 1;
    while (at < argc && argv[at][0] == '-') {
        for (unsigned j = 1; argv[at][j]; ++j) {
            if (argv[at][j] == 'r') reverse = 1;
            else if (argv[at][j] == 'f') fold = 1;
            else { write_s("Usage: sort [-rf] [FILE]\r\n"); return 1; }
        }
        ++at;
    }
    if (argc - at > 1) { write_s("sort: one input file at a time\r\n"); return 1; }
    const char *name = at < argc ? argv[at] : "-";
    HANDLE input = svbox_open_input(name);
    if (input == INVALID_HANDLE_VALUE) { write_s("sort: cannot open "); write_s(name); write_s("\r\n"); return 1; }
    static char lines[SVBOX_SORT_LINES][SVBOX_LINE_MAX];
    unsigned count = 0, n;
    while (count < SVBOX_SORT_LINES && svbox_read_line(input, lines[count], &n)) {
        if (n >= SVBOX_LINE_MAX) n = SVBOX_LINE_MAX - 1;
        lines[count][n] = 0; ++count;
    }
    if (count == SVBOX_SORT_LINES) { char extra[SVBOX_LINE_MAX]; if (svbox_read_line(input, extra, &n)) { svbox_close_input(input, name); write_s("sort: input exceeds 512 lines\r\n"); return 1; } }
    svbox_close_input(input, name);
    for (unsigned i = 1; i < count; ++i) {
        char key[SVBOX_LINE_MAX]; svbox_copy(key, lines[i], sizeof(key));
        unsigned j = i;
        while (j) {
            int cmp = fold ? 0 : 0; unsigned k = 0;
            while (key[k] && lines[j - 1][k]) {
                char a = fold ? lower(key[k]) : key[k];
                char b = fold ? lower(lines[j - 1][k]) : lines[j - 1][k];
                if (a != b) { cmp = (unsigned char)a < (unsigned char)b ? -1 : 1; break; }
                ++k;
            }
            if (!cmp && key[k] != lines[j - 1][k]) cmp = key[k] ? 1 : -1;
            if (reverse) cmp = -cmp;
            if (cmp >= 0) break;
            svbox_copy(lines[j], lines[j - 1], SVBOX_LINE_MAX); --j;
        }
        svbox_copy(lines[j], key, SVBOX_LINE_MAX);
    }
    for (unsigned i = 0; i < count; ++i) { write_s(lines[i]); write_s("\r\n"); }
    return 0;
}

static int svbox_uniq(int argc, char **argv)
{
    int count = 0, duplicates = 0, unique = 0, at = 1;
    while (at < argc && argv[at][0] == '-') {
        for (unsigned j = 1; argv[at][j]; ++j) {
            if (argv[at][j] == 'c') count = 1;
            else if (argv[at][j] == 'd') duplicates = 1;
            else if (argv[at][j] == 'u') unique = 1;
            else { write_s("Usage: uniq [-cdu] [FILE]\r\n"); return 1; }
        }
        ++at;
    }
    if (argc - at > 1) { write_s("uniq: one input file at a time\r\n"); return 1; }
    const char *name = at < argc ? argv[at] : "-";
    HANDLE input = svbox_open_input(name);
    if (input == INVALID_HANDLE_VALUE) { write_s("uniq: cannot open "); write_s(name); write_s("\r\n"); return 1; }
    char *previous = svbox_uniq_previous, *line = svbox_uniq_line;
    unsigned pn = 0, n, repetitions = 0;
    int have = 0;
    while (svbox_read_line(input, line, &n)) {
        if (have && n == pn && svbox_equal_n(line, previous, n)) { ++repetitions; continue; }
        if (have && (!duplicates || repetitions > 1) && (!unique || repetitions == 1)) {
            if (count) { write_u64_width(repetitions, 7); write_s(" "); }
            write_n(previous, pn); write_s("\r\n");
        }
        svbox_copy(previous, line, SVBOX_LINE_MAX); pn = n; repetitions = 1; have = 1;
    }
    if (have && (!duplicates || repetitions > 1) && (!unique || repetitions == 1)) {
        if (count) { write_u64_width(repetitions, 7); write_s(" "); }
        write_n(previous, pn); write_s("\r\n");
    }
    svbox_close_input(input, name);
    return 0;
}

static int svbox_chmod(int argc, char **argv)
{
    if (argc < 3 || same(argv[1], "--help")) {
        write_s("Usage: chmod MODE FILE...\r\n"
                "FAT stores readonly state, not Unix owner/group/execute permissions.\r\n");
        return argc >= 3 ? 0 : 1;
    }
    int write_enabled = 0, change_write = 1, valid = 1;
    const char *mode = argv[1];
    if (*mode == '+' || *mode == '-' ||
        ((*mode == 'u' || *mode == 'g' || *mode == 'o' || *mode == 'a') &&
         (mode[1] == '+' || mode[1] == '-'))) {
        const char *op = mode;
        while (*op && *op != '+' && *op != '-') ++op;
        if (!*op || !op[1]) valid = 0;
        else {
            write_enabled = *op == '+';
            int has_write = 0;
            for (const char *p = op + 1; *p; ++p) {
                if (*p == 'w') has_write = 1;
                else if (*p != 'r' && *p != 'x' && *p != 'X' && *p != 's' && *p != 't') valid = 0;
            }
            change_write = has_write;
            if (op[1] == 0) valid = 0;
        }
    } else {
        unsigned permission = 0;
        if (!*mode) valid = 0;
        for (const char *p = mode; *p; ++p) {
            if (*p < '0' || *p > '7' || permission > 07777u) { valid = 0; break; }
            permission = permission * 8u + (unsigned)(*p - '0');
        }
        if (permission > 07777u) valid = 0;
        write_enabled = (permission & 0222u) != 0;
    }
    if (!valid) { write_s("chmod: invalid mode\r\n"); return 1; }

    int failed = 0;
    for (int i = 2; i < argc; ++i) {
        DWORD attributes = GetFileAttributesA(argv[i]);
        if (attributes == (DWORD)-1) {
            write_s("chmod: cannot access "); write_s(argv[i]); write_s("\r\n");
            failed = 1; continue;
        }
        if (change_write) {
            if (write_enabled) attributes &= ~1u;
            else attributes |= 1u;
        }
        WCHAR wide[260];
        if (!utf8_to_wide(argv[i], wide, 260) || !SetFileAttributesW(wide, attributes)) {
            write_s("chmod: cannot change mode of "); write_s(argv[i]); write_s("\r\n");
            failed = 1;
        }
    }
    return failed;
}

static int svbox_applet(const char *name, int argc, char **argv, int *status)
{
    if (svbox_is(name, "basename")) *status = svbox_basename(argc, argv, 0);
    else if (svbox_is(name, "dirname")) *status = svbox_basename(argc, argv, 1);
    else if (svbox_is(name, "date")) *status = svbox_date(argc, argv);
    else if (svbox_is(name, "sleep")) *status = svbox_sleep(argc, argv);
    else if (svbox_is(name, "seq")) *status = svbox_seq(argc, argv);
    else if (svbox_is(name, "true")) *status = 0;
    else if (svbox_is(name, "false")) *status = 1;
    else if (svbox_is(name, "wc")) *status = svbox_wc(argc, argv);
    else if (svbox_is(name, "head")) *status = svbox_head_tail(argc, argv, 0);
    else if (svbox_is(name, "tail")) *status = svbox_head_tail(argc, argv, 1);
    else if (svbox_is(name, "grep")) *status = svbox_grep(argc, argv);
    else if (svbox_is(name, "cut")) *status = svbox_cut(argc, argv);
    else if (svbox_is(name, "sort")) *status = svbox_sort(argc, argv);
    else if (svbox_is(name, "uniq")) *status = svbox_uniq(argc, argv);
    else if (svbox_is(name, "chmod")) *status = svbox_chmod(argc, argv);
    else return 0;
    return 1;
}

static int svbox_list(void)
{
    write_s("svindowsbox applets:\r\n"
            "basename dirname date sleep seq true false wc head tail\r\n"
            "grep cut sort uniq chmod\r\n");
    return 0;
}
