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
