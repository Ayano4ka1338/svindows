/* Command line, atoms, path helpers and Win32 string APIs.
 * Included by the owning translation unit. */
/* ------------------------------------------------------------------ */
/* Command line (from PEB->ProcessParameters)                          */
/* ------------------------------------------------------------------ */

static char g_cmdline[260];
static WCHAR g_empty_wcmdline[1];

__declspec(dllexport) char *GetCommandLineA(void)
{
    PEB *peb = NtCurrentPeb();
    unsigned char *pp = (unsigned char *)peb->ProcessParameters;
    if (pp) {
        /* CommandLine UNICODE_STRING at offset 0x70; Buffer at +8. */
        WORD len = *(WORD *)(pp + 0x70) / 2;
        WCHAR *buf = *(WCHAR **)(pp + 0x78);
        unsigned i = 0;
        for (; i < len && i < sizeof(g_cmdline) - 1 && buf[i]; i++)
            g_cmdline[i] = (char)buf[i];
        g_cmdline[i] = 0;
    }
    return g_cmdline;
}

__declspec(dllexport) WCHAR *GetCommandLineW(void)
{
    PEB *peb = NtCurrentPeb();
    unsigned char *pp = (unsigned char *)peb->ProcessParameters;
    if (pp) {
        WCHAR *buffer = *(WCHAR **)(pp + 0x78);
        if (buffer)
            return buffer;
    }
    return g_empty_wcmdline;
}

__declspec(dllexport) WORD GetUserDefaultUILanguage(void)
{
    return 0x0409; /* en-US, matching the supplied inbox binaries */
}

__declspec(dllexport) BOOL GetVersionExW(void *version_info)
{
    if (!version_info)
        return 0;
    DWORD size = *(DWORD *)version_info;
    if (size < 20)
        return 0;
    DWORD *v = (DWORD *)version_info;
    v[1] = 10;       /* major */
    v[2] = 0;        /* minor */
    v[3] = 14393;    /* RS1 build used by Explorer */
    v[4] = 2;        /* VER_PLATFORM_WIN32_NT */
    return 1;
}

__declspec(dllexport) BOOL GetProductInfo(DWORD major, DWORD minor,
                                           DWORD sp_major, DWORD sp_minor,
                                           DWORD *product)
{
    (void)major; (void)minor; (void)sp_major; (void)sp_minor;
    if (!product)
        return 0;
    *product = 0x30; /* PRODUCT_PROFESSIONAL */
    return 1;
}

__declspec(dllexport) void OutputDebugStringW(const WCHAR *text)
{
    (void)text;
}

#define LOCAL_ATOM_CAPACITY 128
#define UINT32_K DWORD
#define UINT_PTR_K unsigned long long
static WCHAR g_atom_names[LOCAL_ATOM_CAPACITY][64];
static WORD g_atom_count;

/* Case-insensitive lookup in the process atom table. */
static WORD KiFindAtomW(const WCHAR *name)
{
    if (!name)
        return 0;
    /* Integer atom: callers pass MAKEINTATOM(i) as (WCHAR *)(UINT_PTR_K)i. */
    if ((UINT_PTR_K)name < 0xC000)
        return (WORD)(UINT_PTR_K)name;
    for (WORD i = 0; i < g_atom_count; i++) {
        unsigned j = 0;
        for (;;) {
            WCHAR a = g_atom_names[i][j], b = name[j];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b)
                break;
            if (!a)
                return (WORD)(0xc000 + i);
            j++;
        }
    }
    return 0;
}

__declspec(dllexport) WORD GlobalFindAtomW(const WCHAR *name)
{
    return KiFindAtomW(name);
}

__declspec(dllexport) WORD FindAtomW(const WCHAR *name)
{
    return KiFindAtomW(name);
}

/* Minimal shlwapi path helpers (hosted for the api-ms-win-core-shlwapi
 * contracts): shell32 derefs these results without NULL checks, so a stub
 * returning 0 was fatal. They only need the separator semantics, not the
 * full canonicalization rules. */
static const WCHAR *KiPathSkipRootLike(const WCHAR *path)
{
    /* Past the last separator, or the start when there is none. */
    const WCHAR *last = path;
    for (const WCHAR *p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            last = p + 1;
    return last;
}

__declspec(dllexport) const WCHAR *PathFindFileNameW(const WCHAR *path)
{
    if (!path)
        return 0;
    return KiPathSkipRootLike(path);
}

__declspec(dllexport) const WCHAR *PathFindExtensionW(const WCHAR *path)
{
    if (!path)
        return 0;
    const WCHAR *name = KiPathSkipRootLike(path);
    const WCHAR *dot = 0;
    for (const WCHAR *p = name; *p; p++)
        if (*p == '.')
            dot = p;
    return dot ? dot : name; /* points at the NUL when there is no dot */
}

__declspec(dllexport) const char *PathFindFileNameA(const char *path)
{
    if (!path)
        return 0;
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            last = p + 1;
    return last;
}

__declspec(dllexport) const char *PathFindExtensionA(const char *path)
{
    if (!path)
        return 0;
    const char *name = PathFindFileNameA(path);
    const char *dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.')
            dot = p;
    return dot ? dot : name;
}

__declspec(dllexport) WORD GlobalAddAtomW(const WCHAR *name)
{
    if (!name)
        return 0;
    for (WORD i = 0; i < g_atom_count; i++) {
        unsigned j = 0;
        while (g_atom_names[i][j] && name[j] &&
               g_atom_names[i][j] == name[j])
            j++;
        if (!g_atom_names[i][j] && !name[j])
            return (WORD)(0xc000 + i);
    }
    if (g_atom_count >= LOCAL_ATOM_CAPACITY)
        return 0;
    WORD slot = g_atom_count++;
    unsigned j = 0;
    for (; name[j] && j < 63; j++)
        g_atom_names[slot][j] = name[j];
    g_atom_names[slot][j] = 0;
    return (WORD)(0xc000 + slot);
}

/* The A variants see ASCII names; widen in place before the shared lookup. */
__declspec(dllexport) WORD GlobalAddAtomA(const char *name)
{
    if (!name)
        return 0;
    WCHAR wide[64];
    unsigned i = 0;
    for (; name[i] && i < 63; i++)
        wide[i] = (WCHAR)(unsigned char)name[i];
    wide[i] = 0;
    return GlobalAddAtomW(wide);
}

__declspec(dllexport) WORD GlobalFindAtomA(const char *name)
{
    if (!name)
        return 0;
    if ((UINT_PTR_K)name < 0xC000)
        return (WORD)(UINT_PTR_K)name;
    WCHAR wide[64];
    unsigned i = 0;
    for (; name[i] && i < 63; i++)
        wide[i] = (WCHAR)(unsigned char)name[i];
    wide[i] = 0;
    return KiFindAtomW(wide);
}

__declspec(dllexport) WORD AddAtomA(const char *name)
{
    return GlobalAddAtomA(name);
}

__declspec(dllexport) WORD FindAtomA(const char *name)
{
    return GlobalFindAtomA(name);
}

__declspec(dllexport) WORD GlobalAddAtomExW(const WCHAR *name, DWORD flags)
{
    (void)flags;
    return GlobalAddAtomW(name);
}

__declspec(dllexport) WORD GlobalAddAtomExA(const char *name, DWORD flags)
{
    (void)flags;
    return GlobalAddAtomA(name);
}

/* Deleting keeps the slot: per-atom reference counting is not worth a table
 * yet, and USER only deletes classes at process exit. */
__declspec(dllexport) WORD GlobalDeleteAtom(WORD atom)
{
    return 0;
}

__declspec(dllexport) WORD DeleteAtom(WORD atom)
{
    return 0;
}

static UINT32_K KiAtomNameToWide(WORD atom, WCHAR *out, UINT32_K capacity)
{
    if (!out || !capacity)
        return 0;
    if (atom < 0xC000) {
        /* Integer atoms stringify as "#123". */
        char digits[8];
        int n = 0;
        if (!atom)
            n = 1, digits[0] = '0';
        for (WORD v = atom; v; v /= 10)
            digits[n++] = (char)('0' + v % 10);
        UINT32_K len = 0;
        out[len++] = '#';
        while (n && len + 1 < capacity)
            out[len++] = digits[--n];
        out[len] = 0;
        return len;
    }
    WORD slot = atom - 0xC000;
    if (slot >= g_atom_count)
        return 0;
    UINT32_K len = 0;
    while (g_atom_names[slot][len] && len + 1 < capacity) {
        out[len] = g_atom_names[slot][len];
        len++;
    }
    out[len] = 0;
    return len;
}

__declspec(dllexport) UINT32_K GlobalGetAtomNameW(WORD atom, WCHAR *out,
                                                UINT32_K capacity)
{
    return KiAtomNameToWide(atom, out, capacity);
}

__declspec(dllexport) UINT32_K GetAtomNameW(WORD atom, WCHAR *out,
                                          UINT32_K capacity)
{
    return KiAtomNameToWide(atom, out, capacity);
}

__declspec(dllexport) UINT32_K GlobalGetAtomNameA(WORD atom, char *out,
                                                UINT32_K capacity)
{
    WCHAR wide[64];
    UINT32_K len = KiAtomNameToWide(atom, wide, 64);
    if (!len || !out || !capacity)
        return 0;
    if (len >= capacity)
        len = capacity - 1;
    for (UINT32_K i = 0; i < len; i++)
        out[i] = (char)wide[i];
    out[len] = 0;
    return len;
}

__declspec(dllexport) UINT32_K GetAtomNameA(WORD atom, char *out,
                                          UINT32_K capacity)
{
    return GlobalGetAtomNameA(atom, out, capacity);
}

/* ------------------------------------------------------------------ */
/* String helpers                                                      */
/* ------------------------------------------------------------------ */

__declspec(dllexport) int lstrlenA(const char *s)
{
    int n = 0;
    while (s && s[n])
        n++;
    return n;
}

__declspec(dllexport) char *lstrcpyA(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

__declspec(dllexport) char *lstrcatA(char *dst, const char *src)
{
    char *d = dst;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dst;
}

__declspec(dllexport) int lstrlenW(const WCHAR *s)
{
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static WCHAR nls_fold_w(WCHAR c)
{
    return (c >= 'a' && c <= 'z') ? (WCHAR)(c - ('a' - 'A')) : c;
}

static BYTE nls_fold_a(BYTE c)
{
    return (c >= 'a' && c <= 'z') ? (BYTE)(c - ('a' - 'A')) : c;
}

__declspec(dllexport) DWORD GetThreadLocale(void)
{
    return LANG_ENGLISH_US;
}

__declspec(dllexport) DWORD GetUserDefaultLCID(void)
{
    return LANG_ENGLISH_US;
}

/* CompareString returns CSTR_LESS_THAN/EQUAL/GREATER_THAN (1/2/3), never
 * strcmp-style zero on success.  Supporting NORM_IGNORECASE is sufficient for
 * command-line parsers and path matching while SVINDOWS remains ASCII-only. */
__declspec(dllexport) int CompareStringW(DWORD locale, DWORD flags,
                                         const WCHAR *a, int a_len,
                                         const WCHAR *b, int b_len)
{
    (void)locale;
    if (!a || !b) return 0;
    if (a_len < 0) a_len = lstrlenW(a);
    if (b_len < 0) b_len = lstrlenW(b);
    int common = a_len < b_len ? a_len : b_len;
    for (int i = 0; i < common; i++) {
        WCHAR ca = (flags & 1) ? nls_fold_w(a[i]) : a[i];
        WCHAR cb = (flags & 1) ? nls_fold_w(b[i]) : b[i];
        if (ca != cb) return ca < cb ? 1 : 3;
    }
    return a_len == b_len ? 2 : (a_len < b_len ? 1 : 3);
}

__declspec(dllexport) int CompareStringA(DWORD locale, DWORD flags,
                                         const char *a, int a_len,
                                         const char *b, int b_len)
{
    (void)locale;
    if (!a || !b) return 0;
    if (a_len < 0) { a_len = 0; while (a[a_len]) a_len++; }
    if (b_len < 0) { b_len = 0; while (b[b_len]) b_len++; }
    int common = a_len < b_len ? a_len : b_len;
    for (int i = 0; i < common; i++) {
        BYTE ca = (BYTE)a[i], cb = (BYTE)b[i];
        if (flags & 1) { ca = nls_fold_a(ca); cb = nls_fold_a(cb); }
        if (ca != cb) return ca < cb ? 1 : 3;
    }
    return a_len == b_len ? 2 : (a_len < b_len ? 1 : 3);
}

__declspec(dllexport) int FindStringOrdinal(DWORD find_flags,
                                            const WCHAR *source,
                                            int source_len,
                                            const WCHAR *value,
                                            int value_len,
                                            BOOL ignore_case)
{
    if (!source || !value) return -1;
    if (source_len < 0) source_len = lstrlenW(source);
    if (value_len < 0) value_len = lstrlenW(value);
    if (value_len > source_len) return -1;
    int first = 0, last = source_len - value_len, step = 1;
    if (find_flags & 1) { first = last; last = 0; step = -1; }
    for (int pos = first;; pos += step) {
        int i = 0;
        for (; i < value_len; i++) {
            WCHAR a = source[pos + i], b = value[i];
            if (ignore_case) { a = nls_fold_w(a); b = nls_fold_w(b); }
            if (a != b) break;
        }
        if (i == value_len) return pos;
        if (pos == last) break;
    }
    return -1;
}

static int wide_ieq(const WCHAR *a, const WCHAR *b)
{
    while (*a && *b) {
        WCHAR ca = (*a >= 'a' && *a <= 'z') ? (WCHAR)(*a - 32) : *a;
        WCHAR cb = (*b >= 'a' && *b <= 'z') ? (WCHAR)(*b - 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

static DWORD copy_wide_value(const WCHAR *value, WCHAR *out, DWORD size)
{
    DWORD n = (DWORD)lstrlenW(value);
    if (!out || size == 0)
        return n + 1;
    if (size <= n) {
        for (DWORD i = 0; i + 1 < size; i++) out[i] = value[i];
        if (size) out[size - 1] = 0;
        return n + 1;
    }
    for (DWORD i = 0; i <= n; i++) out[i] = value[i];
    return n;
}

__declspec(dllexport) DWORD GetEnvironmentVariableW(const WCHAR *name,
                                                     WCHAR *out, DWORD size)
{
    static const WCHAR path_name[] = {'P','A','T','H',0};
    static const WCHAR path_value[] = {'C',':','\\',0};
    static const WCHAR ext_name[] = {'P','A','T','H','E','X','T',0};
    static const WCHAR ext_value[] =
        {'.','C','O','M',';','.','E','X','E',';','.','B','A','T',';',
         '.','C','M','D',0};
    static const WCHAR root_name[] =
        {'S','Y','S','T','E','M','R','O','O','T',0};
    static const WCHAR root_value[] = {'C',':','\\','W','I','N','D','O','W','S',0};
    const WCHAR *value = 0;
    if (name && wide_ieq(name, path_name)) value = path_value;
    else if (name && wide_ieq(name, ext_name)) value = ext_value;
    else if (name && wide_ieq(name, root_name)) value = root_value;
    if (!value) {
        SetLastError(203); /* ERROR_ENVVAR_NOT_FOUND */
        return 0;
    }
    return copy_wide_value(value, out, size);
}

__declspec(dllexport) DWORD GetCurrentDirectoryW(DWORD size, WCHAR *out)
{
    static const WCHAR root[] = {'C',':','\\',0};
    PEB *peb = NtCurrentPeb();
    unsigned char *pp = peb ? (unsigned char *)peb->ProcessParameters : 0;
    if (pp) {
        WCHAR *buffer = *(WCHAR **)(pp + 0x40);
        if (buffer)
            return copy_wide_value(buffer, out, size);
    }
    return copy_wide_value(root, out, size);
}

__declspec(dllexport) DWORD GetFullPathNameW(const WCHAR *path, DWORD size,
                                             WCHAR *out, WCHAR **file_part)
{
    WCHAR full[520];
    int n = 0;
    BOOL absolute = path && path[0] && path[1] == ':';
    if (!absolute) {
        full[n++] = 'C'; full[n++] = ':'; full[n++] = '\\';
    }
    for (int i = 0; path && path[i] && n < 519; i++)
        full[n++] = path[i];
    full[n] = 0;
    DWORD result = copy_wide_value(full, out, size);
    if (file_part && out && size > (DWORD)n) {
        *file_part = out;
        for (int i = 0; i < n; i++)
            if (out[i] == '\\' || out[i] == '/') *file_part = &out[i + 1];
    }
    return result;
}

__declspec(dllexport) DWORD GetLongPathNameW(const WCHAR *path, WCHAR *out,
                                             DWORD size)
{
    return copy_wide_value(path, out, size);
}

__declspec(dllexport) WCHAR *CharUpperW(WCHAR *text)
{
    ULONGLONG value = (ULONGLONG)text;
    if ((value >> 16) == 0) {
        WCHAR c = (WCHAR)value;
        if (c >= 'a' && c <= 'z') c -= 32;
        return (WCHAR *)(ULONGLONG)c;
    }
    for (WCHAR *p = text; p && *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    return text;
}

static BOOL wide_in_set(WCHAR c, const WCHAR *set)
{
    while (set && *set)
        if (c == *set++) return 1;
    return 0;
}

__declspec(dllexport) BOOL StrTrimW(WCHAR *text, const WCHAR *trim)
{
    if (!text || !trim) return 0;
    int start = 0, len = lstrlenW(text);
    while (start < len && wide_in_set(text[start], trim)) start++;
    while (len > start && wide_in_set(text[len - 1], trim)) len--;
    BOOL changed = start != 0 || text[len] != 0;
    int out = 0;
    while (start < len) text[out++] = text[start++];
    text[out] = 0;
    return changed;
}

/* Format an unsigned value into buf (base 10 or 16); returns the length. */
static int fmt_uint(char *buf, ULONGLONG v, int base, int is_signed,
                    long long sv)
{
    char tmp[24];
    int n = 0, len = 0;
    int neg = 0;
    if (is_signed) {
        if (sv < 0) {
            neg = 1;
            v = (ULONGLONG)(-sv);
        } else {
            v = (ULONGLONG)sv;
        }
    }
    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        int d = (int)(v % base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    }
    if (neg)
        buf[len++] = '-';
    while (n)
        buf[len++] = tmp[--n];
    return len;
}

/* A small wsprintfA: supports %d %u %x %p %s %c %% (no width/precision). */
__declspec(dllexport) int wsprintfA(char *out, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int o = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            out[o++] = *p;
            continue;
        }
        p++;
        switch (*p) {
        case 'd': o += fmt_uint(out + o, 0, 10, 1, __builtin_va_arg(ap, int)); break;
        case 'u': o += fmt_uint(out + o, __builtin_va_arg(ap, unsigned), 10, 0, 0); break;
        case 'x': o += fmt_uint(out + o, __builtin_va_arg(ap, unsigned), 16, 0, 0); break;
        case 'p':
            out[o++] = '0'; out[o++] = 'x';
            o += fmt_uint(out + o, (ULONGLONG)__builtin_va_arg(ap, void *), 16, 0, 0);
            break;
        case 'c': out[o++] = (char)__builtin_va_arg(ap, int); break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            while (s && *s)
                out[o++] = *s++;
            break;
        }
        case '%': out[o++] = '%'; break;
        default:  out[o++] = '%'; out[o++] = *p; break;
        }
    }
    out[o] = 0;
    __builtin_va_end(ap);
    return o;
}
