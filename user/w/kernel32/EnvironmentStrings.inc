/* Command line, atoms, path helpers and Win32 string APIs.
 * Included by the owning translation unit. */
/* ------------------------------------------------------------------ */
/* Command line (from PEB->ProcessParameters)                          */
/* ------------------------------------------------------------------ */

static char g_cmdline[768];
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
    v[3] = 14393;    /* advertised compatibility build */
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
 * contracts). Callers expect valid pointers and need separator semantics,
 * not the full canonicalization rules. */
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

#include "UnicodeUpper.inc"

static WCHAR nls_fold_w(WCHAR c)
{
    unsigned left = 0, right = sizeof(g_unicode_upper) /
                                     sizeof(g_unicode_upper[0]);
    while (left < right) {
        unsigned mid = left + (right - left) / 2;
        if (g_unicode_upper[mid].From < c) left = mid + 1;
        else right = mid;
    }
    return left < sizeof(g_unicode_upper) / sizeof(g_unicode_upper[0]) &&
           g_unicode_upper[left].From == c ? g_unicode_upper[left].To : c;
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

__declspec(dllexport) WORD GetUserDefaultLangID(void)
{
    return (WORD)(GetUserDefaultLCID() & 0xffff);
}

static DWORD copy_system_directory(WCHAR *buffer, DWORD capacity,
                                   const WCHAR *path, DWORD length)
{
    if (!buffer || capacity <= length) {
        if (buffer && capacity) buffer[0] = 0;
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return length + 1;
    }
    for (DWORD i = 0; i <= length; i++) buffer[i] = path[i];
    return length;
}

__declspec(dllexport) DWORD GetWindowsDirectoryW(WCHAR *buffer, DWORD capacity)
{
    static const WCHAR path[] = {'C',':','\\','W','I','N','D','O','W','S',0};
    return copy_system_directory(buffer, capacity, path, 10);
}

__declspec(dllexport) DWORD GetSystemDirectoryW(WCHAR *buffer, DWORD capacity)
{
    static const WCHAR path[] = {'C',':','\\','W','I','N','D','O','W','S','\\',
                                  'S','Y','S','T','E','M','3','2',0};
    return copy_system_directory(buffer, capacity, path, 19);
}

/* NLS data for the en-US locale currently selected by the process and thread.
 * Unknown locale IDs or information types fail instead of returning values
 * for a different locale. */
typedef struct { DWORD Type; const char *Value; BOOL Numeric; } NLS_FIELD;
static const NLS_FIELD g_nls_en_us[] = {
    {0x0001,"0409",1}, {0x0002,"English (United States)",0},
    {0x0003,"ENU",0}, {0x0005,"1",1}, {0x0006,"United States",0},
    {0x0007,"USA",0}, {0x0009,"0409",1}, {0x000B,"1252",1},
    {0x000C,",",0}, {0x000D,"1",1}, {0x000E,".",0},
    {0x000F,",",0}, {0x0010,"3;0",0}, {0x0011,"2",1},
    {0x0012,"1",1}, {0x0014,"$",0}, {0x0015,"USD",0},
    {0x0016,".",0}, {0x0017,",",0}, {0x0018,"3;0",0},
    {0x0019,"2",1}, {0x001A,"2",1}, {0x001B,"0",1},
    {0x001C,"0",1}, {0x001D,"/",0}, {0x001E,":",0},
    {0x001F,"M/d/yyyy",0}, {0x0020,"dddd, MMMM d, yyyy",0},
    {0x0021,"0",1}, {0x0022,"0",1}, {0x0023,"0",1},
    {0x0028,"AM",0}, {0x0029,"PM",0},
    {0x0059,"en",0}, {0x005A,"US",0}, {0x005C,"en-US",0},
    {0x1001,"English",0}, {0x1002,"United States",0},
    {0x1004,"English (United States)",0}
};

static const NLS_FIELD *nls_field(DWORD locale, DWORD type)
{
    if (locale == 0x0400 || locale == 0x0800) locale = LANG_ENGLISH_US;
    if (locale != LANG_ENGLISH_US) return 0;
    for (DWORD i = 0; i < sizeof(g_nls_en_us)/sizeof(g_nls_en_us[0]); i++)
        if (g_nls_en_us[i].Type == type) return &g_nls_en_us[i];
    return 0;
}

__declspec(dllexport) int GetLocaleInfoW(DWORD locale, DWORD type,
                                          WCHAR *buffer, int capacity)
{
    BOOL number = (type & 0x20000000u) != 0;
    if (capacity < 0 || (type & 0x1fff0000u)) {
        SetLastError(87); return 0;
    }
    const NLS_FIELD *field = nls_field(locale, type & 0xffffu);
    if (!field || (number && !field->Numeric)) {
        SetLastError(87); return 0;
    }
    if (number) {
        if (!capacity) return 2;
        if (!buffer || capacity < 2) { SetLastError(122); return 0; }
        DWORD value = 0;
        for (const char *p = field->Value; *p; p++)
            value = value * (field->Type == 1 || field->Type == 9 ? 16 : 10)
                    + (DWORD)(*p - '0');
        *(DWORD *)buffer = value;
        return 2;
    }
    int length = 0;
    while (field->Value[length]) length++;
    length++;
    if (!capacity) return length;
    if (!buffer || capacity < length) { SetLastError(122); return 0; }
    for (int i = 0; i < length; i++) buffer[i] = (WCHAR)field->Value[i];
    return length;
}

__declspec(dllexport) int GetLocaleInfoA(DWORD locale, DWORD type,
                                          char *buffer, int capacity)
{
    BOOL number = (type & 0x20000000u) != 0;
    if (capacity < 0 || (type & 0x1fff0000u)) {
        SetLastError(87); return 0;
    }
    const NLS_FIELD *field = nls_field(locale, type & 0xffffu);
    if (!field || (number && !field->Numeric)) {
        SetLastError(87); return 0;
    }
    if (number) {
        if (!capacity) return 4;
        if (!buffer || capacity < 4) { SetLastError(122); return 0; }
        DWORD value = 0;
        for (const char *p = field->Value; *p; p++)
            value = value * (field->Type == 1 || field->Type == 9 ? 16 : 10)
                    + (DWORD)(*p - '0');
        *(DWORD *)buffer = value;
        return 4;
    }
    int length = 0;
    while (field->Value[length]) length++;
    length++;
    if (!capacity) return length;
    if (!buffer || capacity < length) { SetLastError(122); return 0; }
    for (int i = 0; i < length; i++) buffer[i] = field->Value[i];
    return length;
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

__declspec(dllexport) int CompareStringOrdinal(const WCHAR *a, int a_len,
                                               const WCHAR *b, int b_len,
                                               BOOL ignore_case)
{
    if (!a || !b || a_len < -1 || b_len < -1 ||
        (ignore_case != 0 && ignore_case != 1)) {
        SetLastError(87);
        return 0;
    }
    if (a_len == -1) a_len = lstrlenW(a);
    if (b_len == -1) b_len = lstrlenW(b);
    int common = a_len < b_len ? a_len : b_len;
    for (int i = 0; i < common; i++) {
        WCHAR ca = ignore_case ? nls_fold_w(a[i]) : a[i];
        WCHAR cb = ignore_case ? nls_fold_w(b[i]) : b[i];
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

__declspec(dllexport) WCHAR *GetEnvironmentStringsW(void)
{
    PEB *peb = NtCurrentPeb();
    unsigned char *params = peb ? (unsigned char *)peb->ProcessParameters : 0;
    const WCHAR *source = params ? *(WCHAR **)(params + 0x80) : 0;
    DWORD length = 1;
    if (source && *source) {
        const WCHAR *end = source;
        while (*end) {
            while (*end) end++;
            end++;
        }
        length = (DWORD)(end - source) + 1;
    } else {
        length = 2;
    }
    WCHAR *result = HeapAlloc(GetProcessHeap(), 0,
                              (SIZE_T)length * sizeof(WCHAR));
    if (!result) return 0;
    if (source && *source) {
        for (DWORD i = 0; i < length; i++) result[i] = source[i];
    } else {
        result[0] = 0;
        result[1] = 0;
    }
    return result;
}

__declspec(dllexport) BOOL FreeEnvironmentStringsW(WCHAR *block)
{
    return block && HeapFree(GetProcessHeap(), 0, block);
}

__declspec(dllexport) DWORD GetEnvironmentVariableW(const WCHAR *name,
                                                     WCHAR *out, DWORD size)
{
    static const WCHAR path_name[] = {'P','A','T','H',0};
    static const WCHAR path_value[] =
        {'C',':','\\',';','C',':','\\','u','s','r','\\','b','i','n',';',
         'C',':','\\','w','i','n','d','o','w','s','\\','s','y','s','t','e','m',
         '3','2',0};
    static const WCHAR ext_name[] = {'P','A','T','H','E','X','T',0};
    static const WCHAR ext_value[] =
        {'.','C','O','M',';','.','E','X','E',';','.','B','A','T',';',
         '.','C','M','D',0};
    static const WCHAR root_name[] =
        {'S','Y','S','T','E','M','R','O','O','T',0};
    static const WCHAR root_value[] = {'C',':','\\','W','I','N','D','O','W','S',0};
    PEB *peb = NtCurrentPeb();
    unsigned char *params = peb ? (unsigned char *)peb->ProcessParameters : 0;
    const WCHAR *entry = params ? *(WCHAR **)(params + 0x80) : 0;
    if (name && entry) {
        while (*entry) {
            const WCHAR *key = name, *scan = entry;
            while (*key && *scan && *scan != '=') {
                WCHAR a = (*key >= 'a' && *key <= 'z') ? *key - 32 : *key;
                WCHAR b = (*scan >= 'a' && *scan <= 'z') ? *scan - 32 : *scan;
                if (a != b) break;
                key++; scan++;
            }
            if (!*key && *scan == '=')
                return copy_wide_value(scan + 1, out, size);
            while (*entry) entry++;
            entry++;
        }
    }
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

static DWORD expand_environment_pass(const WCHAR *source, WCHAR *out)
{
    DWORD length = 0;
    while (*source) {
        if (*source == '%') {
            const WCHAR *end = source + 1;
            while (*end && *end != '%') end++;
            if (*end == '%' && end > source + 1 &&
                end - source < 260) {
                WCHAR name[260], value[260];
                DWORD count = (DWORD)(end - source - 1);
                for (DWORD i = 0; i < count; i++) name[i] = source[i + 1];
                name[count] = 0;
                DWORD got = GetEnvironmentVariableW(name, value, 260);
                if (got && got < 260) {
                    for (DWORD i = 0; i < got; i++) {
                        if (out) out[length] = value[i];
                        length++;
                    }
                    source = end + 1;
                    continue;
                }
            }
        }
        if (out) out[length] = *source;
        length++;
        source++;
    }
    if (out) out[length] = 0;
    return length;
}

__declspec(dllexport) DWORD ExpandEnvironmentStringsW(const WCHAR *source,
                                                       WCHAR *out, DWORD size)
{
    if (!source || (size && !out)) {
        SetLastError(87);
        return 0;
    }
    DWORD length = expand_environment_pass(source, 0);
    if (size > length) expand_environment_pass(source, out);
    else if (out && size) out[0] = 0;
    return length + 1;
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

__declspec(dllexport) DWORD GetCurrentDirectoryA(DWORD size, char *out)
{
    WCHAR wide[260];
    DWORD length = GetCurrentDirectoryW(260, wide);
    if (!length || length >= 260) {
        SetLastError(122);
        return 0;
    }
    if (!out || size <= length) {
        SetLastError(122);
        return length + 1;
    }
    for (DWORD i = 0; i <= length; i++)
        out[i] = wide[i] <= 127 ? (char)wide[i] : '?';
    SetLastError(0);
    return length;
}

__declspec(dllexport) BOOL SetCurrentDirectoryW(const WCHAR *path)
{
    if (!path || !*path) { SetLastError(87); return 0; }
    PEB *peb = NtCurrentPeb();
    unsigned char *pp = peb ? (unsigned char *)peb->ProcessParameters : 0;
    if (!pp) { SetLastError(87); return 0; }
    WCHAR *buffer = *(WCHAR **)(pp + 0x40);
    WORD capacity = *(WORD *)(pp + 0x3a);
    WCHAR canonical[260] = {0};
    DWORD length = 0;
    while (length < 260 && path[length]) {
        canonical[length] = path[length];
        length++;
    }
    if (!buffer || length == 260 || (length + 1) * sizeof(WCHAR) > capacity) {
        SetLastError(122);
        return 0;
    }
    extern NTSTATUS NtSetInformationProcess(HANDLE, DWORD, void *, DWORD);
    NTSTATUS status = NtSetInformationProcess((HANDLE)-1, 0x1002,
                                              canonical, sizeof(canonical));
    if (status < 0) {
        SetLastError(status == (NTSTATUS)0xC0000034L ||
                     status == (NTSTATUS)0xC000003AL ? 3 : 87);
        return 0;
    }
    length = 0;
    while (canonical[length]) length++;
    if ((length + 1) * sizeof(WCHAR) > capacity) {
        SetLastError(122);
        return 0;
    }
    for (DWORD i = 0; i <= length; ++i) buffer[i] = canonical[i];
    *(WORD *)(pp + 0x38) = (WORD)(length * sizeof(WCHAR));
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL SetCurrentDirectoryA(const char *path)
{
    if (!path || !*path) { SetLastError(87); return 0; }
    WCHAR wide[260];
    DWORD length = 0;
    while (length < 260 && path[length]) {
        wide[length] = (unsigned char)path[length];
        length++;
    }
    if (length == 260) { SetLastError(122); return 0; }
    wide[length] = 0;
    return SetCurrentDirectoryW(wide);
}

__declspec(dllexport) DWORD GetFullPathNameW(const WCHAR *path, DWORD size,
                                             WCHAR *out, WCHAR **file_part)
{
    if (file_part) *file_part = 0;
    if (!path || !*path || (size && !out)) {
        SetLastError(87);
        return 0;
    }
    WCHAR joined[520], full[260];
    DWORD n = 0;
    if (path[1] == ':') {
        if (path[2] != '\\' && path[2] != '/') {
            SetLastError(87);
            return 0;
        }
    } else if (path[0] == '\\' || path[0] == '/') {
        joined[n++] = 'C'; joined[n++] = ':';
    } else {
        DWORD cwd = GetCurrentDirectoryW(260, joined);
        if (!cwd || cwd >= 260) { SetLastError(206); return 0; }
        n = cwd;
        if (joined[n - 1] != '\\' && joined[n - 1] != '/')
            joined[n++] = '\\';
    }
    DWORD i = 0;
    while (path[i] && n + 1 < 520) joined[n++] = path[i++];
    if (path[i]) { SetLastError(206); return 0; }
    joined[n] = 0;
    if (joined[0] != 'C' && joined[0] != 'c') {
        SetLastError(3);
        return 0;
    }
    full[0] = 'C'; full[1] = ':'; full[2] = '\\';
    DWORD used = 3, scan = 2;
    while (joined[scan] == '\\' || joined[scan] == '/') scan++;
    while (joined[scan]) {
        DWORD start = scan;
        while (joined[scan] && joined[scan] != '\\' && joined[scan] != '/')
            scan++;
        DWORD count = scan - start;
        while (joined[scan] == '\\' || joined[scan] == '/') scan++;
        if (count == 1 && joined[start] == '.') continue;
        if (count == 2 && joined[start] == '.' && joined[start + 1] == '.') {
            if (used > 3) {
                while (used > 3 && full[used - 1] != '\\') used--;
                if (used > 3) used--;
            }
            continue;
        }
        if (used > 3) {
            if (used + 1 >= 260) { SetLastError(206); return 0; }
            full[used++] = '\\';
        }
        if (used + count >= 260) { SetLastError(206); return 0; }
        for (DWORD j = 0; j < count; j++) full[used++] = joined[start + j];
    }
    full[used] = 0;
    DWORD result = copy_wide_value(full, out, size);
    if (file_part && out && size > used && used > 3) {
        *file_part = out + 3;
        for (DWORD j = 3; j < used; j++)
            if (out[j] == '\\') *file_part = out + j + 1;
    }
    SetLastError(0);
    return result;
}

__declspec(dllexport) DWORD GetFileAttributesW(const WCHAR *path);

static BOOL search_path_candidate(const WCHAR *directory, DWORD dir_length,
                                  const WCHAR *file, const WCHAR *extension,
                                  WCHAR *candidate, DWORD *candidate_length)
{
    DWORD n = 0;
    for (DWORD i = 0; i < dir_length; i++) {
        if (n >= 518) return 0;
        candidate[n++] = directory[i];
    }
    if (n && candidate[n - 1] != '\\' && candidate[n - 1] != '/')
        candidate[n++] = '\\';
    DWORD file_start = n;
    while (*file) {
        if (n >= 518) return 0;
        candidate[n++] = *file++;
    }
    BOOL has_extension = 0;
    for (DWORD i = file_start; i < n; i++) {
        if (candidate[i] == '\\' || candidate[i] == '/') has_extension = 0;
        else if (candidate[i] == '.') has_extension = 1;
    }
    if (!has_extension && extension) {
        while (*extension) {
            if (n >= 518) return 0;
            candidate[n++] = *extension++;
        }
    }
    candidate[n] = 0;
    DWORD attributes = GetFileAttributesW(candidate);
    if (attributes == (DWORD)-1 || (attributes & 0x10)) return 0;
    *candidate_length = n;
    return 1;
}

static BOOL search_path_list(const WCHAR *list, const WCHAR *file,
                             const WCHAR *extension, WCHAR *candidate,
                             DWORD *candidate_length)
{
    if (!list) return 0;
    const WCHAR *segment = list;
    for (;;) {
        const WCHAR *end = segment;
        while (*end && *end != ';') end++;
        if (search_path_candidate(segment, (DWORD)(end - segment), file,
                                  extension, candidate, candidate_length))
            return 1;
        if (!*end) break;
        segment = end + 1;
    }
    return 0;
}

__declspec(dllexport) DWORD SearchPathW(const WCHAR *path, const WCHAR *file,
                                        const WCHAR *extension, DWORD capacity,
                                        WCHAR *out, WCHAR **file_part)
{
    if (file_part) *file_part = 0;
    if (!file || !*file || (capacity && !out)) {
        SetLastError(87);
        return 0;
    }
    WCHAR candidate[520];
    DWORD length = 0;
    BOOL found = 0;
    BOOL qualified = file[0] == '\\' || file[0] == '/' ||
                     (file[0] && file[1] == ':');
    if (!qualified)
        for (const WCHAR *p = file; *p; p++)
            if (*p == '\\' || *p == '/') qualified = 1;
    if (qualified)
        found = search_path_candidate(0, 0, file, extension,
                                      candidate, &length);
    else if (path)
        found = search_path_list(path, file, extension,
                                 candidate, &length);
    else {
        WCHAR current[260], env_path[520];
        static const WCHAR system_dir[] =
            {'C',':','\\','W','i','n','d','o','w','s','\\','S','y','s','t','e','m','3','2',0};
        static const WCHAR windows_dir[] =
            {'C',':','\\','W','i','n','d','o','w','s',0};
        static const WCHAR path_name[] = {'P','A','T','H',0};
        DWORD current_len = GetCurrentDirectoryW(260, current);
        if (current_len && current_len < 260)
            found = search_path_candidate(current, current_len, file,
                                          extension, candidate, &length);
        if (!found)
            found = search_path_candidate(system_dir, lstrlenW(system_dir),
                                          file, extension, candidate, &length);
        if (!found)
            found = search_path_candidate(windows_dir, lstrlenW(windows_dir),
                                          file, extension, candidate, &length);
        if (!found) {
            DWORD env_len = GetEnvironmentVariableW(path_name, env_path, 520);
            if (env_len && env_len < 520)
                found = search_path_list(env_path, file, extension,
                                         candidate, &length);
        }
    }
    if (!found) {
        SetLastError(2);
        return 0;
    }
    if (capacity <= length) return length + 1;
    for (DWORD i = 0; i <= length; i++) out[i] = candidate[i];
    if (file_part) {
        *file_part = out;
        for (DWORD i = 0; i < length; i++)
            if (out[i] == '\\' || out[i] == '/') *file_part = out + i + 1;
    }
    return length;
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
