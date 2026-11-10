__declspec(dllimport) int LCMapStringW(DWORD locale, DWORD flags,
    const WCHAR *source, int length, WCHAR *out, int capacity);
__declspec(dllimport) BOOL GetStringTypeW(DWORD type, const WCHAR *source,
    int length, WORD *out);

#define USER_LCMAP_LOWERCASE 0x100u
#define USER_LCMAP_UPPERCASE 0x200u

static DWORD map_chars(WCHAR *text, DWORD count, DWORD flags)
{
    if (!text || !count || count > 0x7fffffffu) return 0;
    return (DWORD)LCMapStringW(0x0400, flags, text, (int)count, text, (int)count);
}

static WCHAR *map_char_or_string(WCHAR *text, DWORD flags)
{
    if ((ULONG_PTR)text <= 0xffff) {
        WCHAR value = (WCHAR)(ULONG_PTR)text;
        map_chars(&value, 1, flags);
        return (WCHAR *)(ULONG_PTR)value;
    }

    DWORD length = 0;
    while (text[length]) length++;
    map_chars(text, length, flags);
    return text;
}

USER32_API DWORD CharUpperBuffW(WCHAR *text, DWORD count)
{
    return map_chars(text, count, USER_LCMAP_UPPERCASE);
}

USER32_API WCHAR *CharUpperW(WCHAR *text)
{
    return map_char_or_string(text, USER_LCMAP_UPPERCASE);
}

USER32_API WCHAR *CharLowerW(WCHAR *text)
{
    return map_char_or_string(text, USER_LCMAP_LOWERCASE);
}

USER32_API const WCHAR *CharNextW(const WCHAR *text)
{
    return text && *text ? text + 1 : text;
}

USER32_API BOOL IsCharAlphaNumericW(WCHAR character)
{
    WORD type;
    return GetStringTypeW(1 /* CT_CTYPE1 */, &character, 1, &type) &&
           (type & 0x0104) != 0; /* C1_ALPHA | C1_DIGIT */
}
