typedef unsigned short WCHAR;

/* The Windows 7 calculator imports SHLWAPI ordinal 225, which is
 * SHStripMneumonicW. Remove mnemonic markers in place; doubled ampersands
 * represent one literal ampersand. */
__declspec(dllexport) WCHAR *SHStripMneumonicW(WCHAR *text)
{
    if (!text) return 0;
    WCHAR *read = text, *write = text;
    while (*read) {
        if (*read == '&') {
            read++;
            if (*read != '&') continue;
        }
        *write++ = *read++;
    }
    *write = 0;
    return text;
}

__declspec(dllimport) void *CoTaskMemAlloc(unsigned long long bytes);
__declspec(dllimport) int CompareStringOrdinal(const WCHAR *a, int a_len,
    const WCHAR *b, int b_len, int ignore_case);

__declspec(dllexport) int PathIsFileSpecW(const WCHAR *path)
{
    if (!path) return 0;
    for (; *path; path++)
        if (*path == '\\' || *path == '/' || *path == ':') return 0;
    return 1;
}

__declspec(dllexport) long SHStrDupW(const WCHAR *text, WCHAR **copy)
{
    if (!copy) return (long)0x80070057u;
    *copy = 0;
    if (!text) return (long)0x80070057u;
    unsigned long long length = 0;
    while (text[length]) length++;
    WCHAR *buffer = CoTaskMemAlloc((length + 1) * sizeof(*buffer));
    if (!buffer) return (long)0x8007000eu;
    for (unsigned long long i = 0; i <= length; i++) buffer[i] = text[i];
    *copy = buffer;
    return 0;
}

__declspec(dllexport) const WCHAR *StrChrW(const WCHAR *text, WCHAR ch)
{
    if (!text) return 0;
    while (*text && *text != ch) text++;
    return *text == ch ? text : 0;
}

__declspec(dllexport) const WCHAR *StrChrIW(const WCHAR *text, WCHAR ch)
{
    if (!text) return 0;
    for (; *text; text++)
        if (CompareStringOrdinal(text, 1, &ch, 1, 1) == 2) return text;
    return ch ? 0 : text;
}

__declspec(dllexport) const WCHAR *StrRChrW(const WCHAR *start, const WCHAR *end,
    WCHAR ch)
{
    if (!start) return 0;
    const WCHAR *match = 0;
    for (; (!end || start < end) && *start; start++)
        if (*start == ch) match = start;
    if ((!end || start < end) && !*start && !ch) return start;
    return match;
}

__declspec(dllexport) const WCHAR *StrStrIW(const WCHAR *text, const WCHAR *find)
{
    if (!text || !find) return 0;
    if (!*find) return text;
    for (; *text; text++) {
        int i = 0;
        while (text[i] && find[i] &&
               CompareStringOrdinal(text + i, 1, find + i, 1, 1) == 2) i++;
        if (!find[i]) return text;
    }
    return 0;
}

__declspec(dllexport) int StrToIntW(const WCHAR *text)
{
    if (!text) return 0;
    while (*text == ' ' || (*text >= 9 && *text <= 13)) text++;
    int negative = *text == '-';
    if (negative || *text == '+') text++;
    unsigned value = 0;
    while (*text >= '0' && *text <= '9') value = value * 10 + (*text++ - '0');
    return (int)(negative ? 0u - value : value);
}

typedef struct { unsigned char Bytes[16]; } SHELL_IID;
typedef struct { const SHELL_IID *Id; unsigned long Offset; } SHELL_QITAB;
typedef struct SHELL_UNKNOWN SHELL_UNKNOWN;
typedef struct {
    long (*Query)(SHELL_UNKNOWN *, const SHELL_IID *, void **);
    unsigned long (*AddRef)(SHELL_UNKNOWN *);
    unsigned long (*Release)(SHELL_UNKNOWN *);
} SHELL_UNKNOWN_VTABLE;
struct SHELL_UNKNOWN { const SHELL_UNKNOWN_VTABLE *Methods; };
static int shell_same_iid(const SHELL_IID *a, const SHELL_IID *b)
{
    for (unsigned i = 0; i < 16; i++) if (a->Bytes[i] != b->Bytes[i]) return 0;
    return 1;
}
__declspec(dllexport) long QISearch(void *object, const SHELL_QITAB *table,
    const SHELL_IID *id, void **out)
{
    static const SHELL_IID unknown = {{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46}};
    if (!out) return (long)0x80004003u;
    *out = 0;
    if (!object || !table || !id) return (long)0x80070057u;
    int identity = shell_same_iid(id, &unknown);
    for (; table->Id; table++) {
        if (identity || shell_same_iid(id, table->Id)) {
            SHELL_UNKNOWN *found = (SHELL_UNKNOWN *)((char *)object + table->Offset);
            found->Methods->AddRef(found);
            *out = found;
            return 0;
        }
    }
    return (long)0x80004002u;
}

__declspec(dllimport) int LoadStringW(void *, unsigned, WCHAR *, int);
__declspec(dllimport) int GetWindowTextW(void *, WCHAR *, int);
__declspec(dllimport) int MessageBoxW(void *, const WCHAR *, const WCHAR *, unsigned);
__declspec(dllimport) unsigned long FormatMessageW(unsigned long, const void *,
    unsigned long, unsigned long, WCHAR *, unsigned long, void *);
__declspec(dllimport) void *LocalFree(void *);
__declspec(dllimport) void CoTaskMemFree(void *);
__declspec(dllimport) void SetLastError(unsigned long);

__declspec(dllexport) int ShellMessageBoxW(void *instance, void *owner,
    const WCHAR *text, const WCHAR *title, unsigned flags, ...)
{
    WCHAR title_resource[512];
    WCHAR *text_resource = 0;
    unsigned long long text_id = (unsigned long long)text;
    unsigned long long title_id = (unsigned long long)title;
    if (text_id <= 0xffff) {
        const WCHAR *raw = 0;
        int count = LoadStringW(instance, (unsigned)text_id, (WCHAR *)&raw, 0);
        if (!count) return 0;
        text_resource = CoTaskMemAlloc(((unsigned long long)count + 1) * sizeof(WCHAR));
        if (!text_resource) { SetLastError(8); return 0; }
        for (int i = 0; i < count; i++) text_resource[i] = raw[i];
        text_resource[count] = 0;
        text = text_resource;
    }
    if (title_id && title_id <= 0xffff) {
        if (!LoadStringW(instance, (unsigned)title_id, title_resource, 512)) {
            CoTaskMemFree(text_resource); return 0;
        }
        title = title_resource;
    } else if (owner) {
        title_resource[0] = 0;
        GetWindowTextW(owner, title_resource, 512);
        title = title_resource;
    }
    WCHAR *message = 0;
    __builtin_va_list args;
    __builtin_va_start(args, flags);
    unsigned long length = FormatMessageW(0x500, text, 0, 0,
                                          (WCHAR *)&message, 0, &args);
    __builtin_va_end(args);
    CoTaskMemFree(text_resource);
    if (!length) return 0;
    int result = MessageBoxW(owner, message, title ? title : L"", flags);
    LocalFree(message);
    return result;
}
