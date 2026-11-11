static unsigned shell_text_length(const WCHAR *text)
{ unsigned n = 0; if (text) while (n < 260 && text[n]) ++n; return n; }

__declspec(dllexport) const WCHAR *StrStrW(const WCHAR *text, const WCHAR *find)
{
    if (!text || !find) return 0;
    for (;; ++text) {
        unsigned i = 0;
        while (find[i] && text[i] == find[i]) ++i;
        if (!find[i]) return text;
        if (!*text) return 0;
    }
}

static WCHAR *shell_path_extension(WCHAR *path)
{
    WCHAR *extension = 0, *end = path;
    if (!path) return 0;
    for (; *end; ++end) {
        if (*end == '\\' || *end == '/' || *end == ':') extension = 0;
        else if (*end == '.') extension = end;
    }
    return extension ? extension : end;
}
__declspec(dllexport) void PathRemoveExtensionW(WCHAR *path)
{ WCHAR *extension = shell_path_extension(path); if (extension) *extension = 0; }

__declspec(dllexport) int PathAddExtensionW(WCHAR *path, const WCHAR *extension)
{
    if (!path) return 0;
    if (!extension) extension = L".exe";
    WCHAR *current = shell_path_extension(path);
    if (*current) return 0;
    unsigned length = shell_text_length(path), added = shell_text_length(extension);
    if (length + added >= 260) return 0;
    for (unsigned i = 0; i <= added; ++i) path[length + i] = extension[i];
    return 1;
}

__declspec(dllexport) int PathAppendW(WCHAR *path, const WCHAR *more)
{
    if (!path || !more) return 0;
    while (*more == '\\' || *more == '/') ++more;
    unsigned length = shell_text_length(path), added = shell_text_length(more);
    unsigned slash = length && added && path[length - 1] != '\\' && path[length - 1] != '/';
    if (length + added + slash >= 260) return 0;
    if (slash) path[length++] = '\\';
    for (unsigned i = 0; i <= added; ++i) path[length + i] = more[i];
    return 1;
}

__declspec(dllexport) WCHAR *StrFormatByteSizeW(long long bytes, WCHAR *out, unsigned capacity)
{
    if (!out || !capacity) return 0;
    const WCHAR *units[] = {L"bytes", L"KB", L"MB", L"GB", L"TB", L"PB", L"EB"};
    unsigned unit = 0;
    unsigned long long value = bytes < 0 ? (unsigned long long)(-(bytes + 1)) + 1 : bytes;
    unsigned long long divisor = 1;
    while (unit < 6 && value / divisor >= 1024) { divisor *= 1024; ++unit; }
    unsigned long long whole = value / divisor;
    unsigned decimals = unit ? (whole < 10 ? 2 : whole < 100 ? 1 : 0) : 0;
    WCHAR text[48], digits[24]; unsigned n = 0, count = 0;
    if (bytes < 0) text[n++] = '-';
    do { digits[count++] = '0' + whole % 10; whole /= 10; } while (whole);
    while (count) text[n++] = digits[--count];
    if (decimals) {
        unsigned long long remainder = value % divisor;
        text[n++] = '.';
        for (unsigned i = 0; i < decimals; ++i) {
            remainder *= 10; /* divisor <= 2^60, so this cannot overflow. */
            text[n++] = '0' + remainder / divisor;
            remainder %= divisor;
        }
    }
    text[n++] = ' ';
    for (unsigned i = 0; units[unit][i]; ++i) text[n++] = units[unit][i];
    if (n >= capacity) { out[0] = 0; return 0; }
    for (unsigned i = 0; i < n; ++i) out[i] = text[i];
    out[n] = 0;
    return out;
}
