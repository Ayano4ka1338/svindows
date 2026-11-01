/* Environment expansion, executable lookup and batch-file execution. */
#define CMD_LINE_MAX 1024
#define CMD_ENV_COUNT 32
#define CMD_HISTORY_COUNT 16

typedef struct _CMD_ENV_ENTRY {
    char Name[32];
    char Value[260];
} CMD_ENV_ENTRY;

static CMD_ENV_ENTRY g_environment[CMD_ENV_COUNT] = {
    { "PATH", "C:\\;C:\\usr\\bin;C:\\windows\\system32" },
    { "PATHEXT", ".EXE;.COM;.BAT;.ELF" },
    /* Defaults for musl/Alpine programs launched from the PE command shell. */
    { "HOME", "/home/user" },
    { "USER", "user" },
    { "LOGNAME", "user" },
    { "TMPDIR", "/tmp" },
    /* Let Imlib2 inspect the image directly if libmagic has a false negative. */
    { "FEH_SKIP_MAGIC", "1" },
};
static unsigned g_environment_count = 7;
static char g_history[CMD_HISTORY_COUNT][CMD_LINE_MAX];
static unsigned g_history_count;
static char g_current_directory[260] = "C:\\";

/* Collapse drive prefixes, repeated separators and . / .. components into a
 * canonical absolute path rooted on the single mounted C: volume. */
static int normalize_path(const char *input, char output[260])
{
    if (!input || !*input) return 0;
    char joined[520];
    unsigned j = 0, p = 0;
    int absolute = input[0] == '\\' || input[0] == '/' || input[1] == ':';
    if (input[1] == ':' && lower(input[0]) != 'c') return 0;
    if (!absolute) {
        while (g_current_directory[p] && j + 1 < sizeof(joined))
            joined[j++] = g_current_directory[p++];
        if (g_current_directory[p]) return 0;
        if (j && joined[j - 1] != '\\') {
            if (j + 1 >= sizeof(joined)) return 0;
            joined[j++] = '\\';
        }
    }
    p = 0;
    if (input[1] == ':') p = 2;
    while (input[p] == '\\' || input[p] == '/') p++;
    if (absolute) { joined[j++] = 'C'; joined[j++] = ':'; joined[j++] = '\\'; }
    while (input[p] && j + 1 < sizeof(joined)) joined[j++] = input[p++];
    if (input[p]) return 0;
    joined[j] = 0;

    unsigned out = 0;
    output[out++] = 'C'; output[out++] = ':'; output[out++] = '\\';
    p = (joined[1] == ':') ? 2 : 0;
    while (joined[p] == '\\' || joined[p] == '/') p++;
    while (joined[p]) {
        char component[64]; unsigned n = 0;
        while (joined[p] && joined[p] != '\\' && joined[p] != '/') {
            if (n + 1 >= sizeof(component)) return 0;
            component[n++] = joined[p++];
        }
        component[n] = 0;
        while (joined[p] == '\\' || joined[p] == '/') p++;
        if (!n || (n == 1 && component[0] == '.')) continue;
        if (n == 2 && component[0] == '.' && component[1] == '.') {
            if (out > 3) {
                if (output[out - 1] == '\\') out--;
                while (out > 3 && output[out - 1] != '\\') out--;
            }
            continue;
        }
        if (out > 3 && output[out - 1] != '\\') {
            if (out + 1 >= 260) return 0;
            output[out++] = '\\';
        }
        for (unsigned i = 0; i < n; i++) {
            if (out + 1 >= 260) return 0;
            output[out++] = component[i];
        }
    }
    output[out] = 0;
    return 1;
}

static int begins_ci(const char *text, const char *prefix)
{
    while (*prefix) {
        if (!*text || lower(*text) != lower(*prefix)) return 0;
        text++;
        prefix++;
    }
    return 1;
}

static int ends_ci(const char *text, const char *suffix)
{
    unsigned a = slen(text), b = slen(suffix);
    if (b > a) return 0;
    return same(text + a - b, suffix);
}

static const char *env_get(const char *name)
{
    for (unsigned i = 0; i < g_environment_count; i++)
        if (same(g_environment[i].Name, name)) return g_environment[i].Value;
    return "";
}

static void env_set(const char *name, const char *value)
{
    unsigned slot = g_environment_count;
    for (unsigned i = 0; i < g_environment_count; i++)
        if (same(g_environment[i].Name, name)) { slot = i; break; }
    if (slot == CMD_ENV_COUNT) return;
    if (slot == g_environment_count) g_environment_count++;
    unsigned i = 0;
    for (; name[i] && i + 1 < sizeof(g_environment[slot].Name); i++)
        g_environment[slot].Name[i] = name[i];
    g_environment[slot].Name[i] = 0;
    i = 0;
    for (; value[i] && i + 1 < sizeof(g_environment[slot].Value); i++)
        g_environment[slot].Value[i] = value[i];
    g_environment[slot].Value[i] = 0;
}

static void expand_variables(const char *input, char output[CMD_LINE_MAX])
{
    unsigned in = 0, out = 0;
    while (input[in] && out + 1 < CMD_LINE_MAX) {
        if (input[in] != '%') {
            output[out++] = input[in++];
            continue;
        }
        unsigned end = in + 1;
        while (input[end] && input[end] != '%') end++;
        if (!input[end]) { output[out++] = input[in++]; continue; }
        char name[40];
        unsigned n = end - in - 1;
        if (n >= sizeof(name)) n = sizeof(name) - 1;
        for (unsigned i = 0; i < n; i++) name[i] = input[in + 1 + i];
        name[n] = 0;
        const char *value = same(name, "ERRORLEVEL") ? 0 : env_get(name);
        char number[16];
        if (!value) {
            unsigned pos = sizeof(number);
            DWORD v = g_errorlevel;
            do { number[--pos] = (char)('0' + v % 10); v /= 10; } while (v);
            unsigned j = 0;
            while (pos < sizeof(number)) number[j++] = number[pos++];
            number[j] = 0;
            value = number;
        }
        for (unsigned i = 0; value[i] && out + 1 < CMD_LINE_MAX; i++)
            output[out++] = value[i];
        in = end + 1;
    }
    output[out] = 0;
}

static int file_exists(const char *path)
{
    char absolute[260];
    if (!normalize_path(path, absolute)) return 0;
    DWORD attributes = GetFileAttributesA(absolute);
    return attributes != (DWORD)-1 && !(attributes & 0x10u);
}

static int has_extension(const char *name)
{
    int dot = 0;
    for (unsigned i = 0; name[i]; i++) {
        if (name[i] == '\\' || name[i] == '/') dot = 0;
        else if (name[i] == '.') dot = 1;
    }
    return dot;
}

static int try_program(const char *base, char result[260])
{
    char candidate[260];
    if (!normalize_path(base, candidate)) return 0;
    unsigned n = 0;
    while (candidate[n] && n + 1 < 260) { result[n] = candidate[n]; n++; }
    result[n] = 0;
    if (file_exists(result)) return 1;
    if (!has_extension(result)) {
        const char *extensions[4] = { ".exe", ".com", ".bat", ".elf" };
        for (unsigned e = 0; e < 4; e++) {
            unsigned p = n, i = 0;
            while (extensions[e][i] && p + 1 < 260)
                result[p++] = extensions[e][i++];
            if (extensions[e][i]) continue;
            result[p] = 0;
            if (file_exists(result)) return 1;
        }
    }
    return 0;
}

static int resolve_program(const char *name, char result[260])
{
    if (try_program(name, result)) return 1;
    if (name[0] == '\\' || name[0] == '/' || name[1] == ':' ||
        !*env_get("PATH")) return 0;
    const char *path = env_get("PATH");
    unsigned pos = 0;
    while (path[pos]) {
        char candidate[260];
        unsigned n = 0;
        int too_long = 0;
        while (path[pos] && path[pos] != ';') {
            if (n + 2 < sizeof(candidate)) candidate[n++] = path[pos];
            else too_long = 1;
            pos++;
        }
        if (path[pos] == ';') pos++;
        if (too_long) continue;
        if (n && candidate[n - 1] != '\\' && candidate[n - 1] != '/')
            candidate[n++] = '\\';
        unsigned i = 0;
        for (; name[i] && n + 1 < sizeof(candidate); i++)
            candidate[n++] = name[i];
        if (name[i]) continue;
        candidate[n] = 0;
        if (try_program(candidate, result)) return 1;
    }
    return 0;
}

static char *parse_word(char *text, char word[260])
{
    text = skip_spaces(text);
    unsigned n = 0;
    int quoted = 0;
    if (*text == '"') { quoted = 1; text++; }
    while (*text && n + 1 < 260) {
        if (quoted) {
            if (*text == '"') { text++; break; }
        } else if (*text == ' ' || *text == '\t') break;
        if (*text == '^' && text[1]) text++;
        word[n++] = *text++;
    }
    word[n] = 0;
    return skip_spaces(text);
}

static void command_set(char *arg)
{
    arg = skip_spaces(arg);
    if (!*arg) {
        for (unsigned i = 0; i < g_environment_count; i++) {
            write_s(g_environment[i].Name); write_s("=");
            write_s(g_environment[i].Value); write_s("\r\n");
        }
        return;
    }
    char *equals = arg;
    while (*equals && *equals != '=') equals++;
    if (!*equals) {
        write_s(env_get(arg)); write_s("\r\n");
        return;
    }
    *equals++ = 0;
    trim_right(arg);
    env_set(arg, equals);
}

static void command_history(void)
{
    for (unsigned i = 0; i < g_history_count; i++) {
        write_u64(i + 1); write_s("  "); write_s(g_history[i]);
        write_s("\r\n");
    }
}

static void execute_batch(const char *path)
{
    HANDLE file = CreateFileA(path, 0x80000000u, 1, 0, 3, 0, 0);
    if (file == INVALID_HANDLE_VALUE) { g_errorlevel = 1; return; }
    char contents[CMD_LINE_MAX];
    DWORD length = 0;
    BOOL loaded = ReadFile(file, contents, sizeof(contents) - 1, &length, 0);
    CloseHandle(file);
    if (!loaded) { g_errorlevel = 1; return; }
    contents[length] = 0;

    unsigned start = 0;
    for (unsigned end = 0; end <= length; end++) {
        if (contents[end] != '\n' && contents[end] != 0) continue;
        contents[end] = 0;
        if (end > start && contents[end - 1] == '\r') contents[end - 1] = 0;
        char *command = contents + start;
        trim_right(command);
        int quiet = *command == '@';
        if (quiet) command++;
        command = skip_spaces(command);
        if (*command) {
            char expanded[CMD_LINE_MAX];
            expand_variables(command, expanded);
            if (g_echo && !quiet) { write_s(expanded); write_s("\r\n"); }
            if (!execute_line(expanded)) break;
        }
        start = end + 1;
    }
}

static void command_if(char *arg)
{
    arg = skip_spaces(arg);
    int negate = 0;
    if (begins_ci(arg, "not ")) { negate = 1; arg = skip_spaces(arg + 4); }
    int condition = 0;
    char *command = arg;
    if (begins_ci(arg, "errorlevel ")) {
        arg = skip_spaces(arg + 11);
        DWORD level = 0;
        while (*arg >= '0' && *arg <= '9') level = level * 10 + (*arg++ - '0');
        condition = g_errorlevel >= level;
        command = skip_spaces(arg);
    } else if (begins_ci(arg, "exist ")) {
        char path[260];
        command = parse_word(arg + 6, path);
        condition = file_exists(path);
    } else {
        char left[260], right[260];
        command = parse_word(arg, left);
        char *equals = left;
        while (*equals && !(equals[0] == '=' && equals[1] == '=')) equals++;
        if (*equals) {
            *equals = 0;
            unsigned i = 0;
            equals += 2;
            while (equals[i] && i + 1 < sizeof(right)) { right[i] = equals[i]; i++; }
            right[i] = 0;
            condition = same(left, right);
        }
    }
    if (negate) condition = !condition;
    if (condition && *command) execute_line(command);
}
