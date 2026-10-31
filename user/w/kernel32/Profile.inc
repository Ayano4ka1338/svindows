/* Read and write legacy Win32 INI profile files. */
typedef struct _PROFILE_TEXT {
    const BYTE *Bytes;
    DWORD Units;
    BOOL Utf16;
    DWORD Skip;
} PROFILE_TEXT;

static WCHAR profile_unit(const PROFILE_TEXT *text, DWORD index)
{
    if (index >= text->Units) return 0;
    if (text->Utf16)
        return (WCHAR)(text->Bytes[index * 2] |
                       ((WORD)text->Bytes[index * 2 + 1] << 8));
    return text->Bytes[index];
}

static WCHAR profile_fold(WCHAR value)
{
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static BOOL profile_equal(const PROFILE_TEXT *text, DWORD begin, DWORD end,
                          const WCHAR *wanted)
{
    if (!wanted) return 0;
    DWORD i = 0;
    while (begin + i < end && wanted[i]) {
        if (profile_fold(profile_unit(text, begin + i)) !=
            profile_fold(wanted[i])) return 0;
        i++;
    }
    return begin + i == end && !wanted[i];
}

static BOOL profile_space(WCHAR value)
{
    return value == ' ' || value == '\t';
}

static void profile_trim(const PROFILE_TEXT *text, DWORD *begin, DWORD *end)
{
    while (*begin < *end && profile_space(profile_unit(text, *begin)))
        ++*begin;
    while (*end > *begin && profile_space(profile_unit(text, *end - 1)))
        --*end;
}

static DWORD profile_single(WCHAR *output, DWORD capacity,
                            const PROFILE_TEXT *text, DWORD begin, DWORD end,
                            const WCHAR *fallback)
{
    if (!output || !capacity) return 0;
    DWORD length = 0;
    if (text) {
        if (end > begin + 1 && profile_unit(text, begin) == '"' &&
            profile_unit(text, end - 1) == '"') { begin++; end--; }
        for (DWORD i = begin; i < end && length + 1 < capacity; i++)
            output[length++] = profile_unit(text, i);
    } else if (fallback) {
        while (fallback[length] && length + 1 < capacity) {
            output[length] = fallback[length];
            length++;
        }
    }
    output[length] = 0;
    return length;
}

static void profile_append(WCHAR *output, DWORD capacity, DWORD *used,
                           const PROFILE_TEXT *text, DWORD begin, DWORD end)
{
    /* Reserve two terminators for the MULTI_SZ result. */
    if (capacity < 2) return;
    for (DWORD i = begin; i < end && *used + 2 < capacity; i++)
        output[(*used)++] = profile_unit(text, i);
    if (*used + 2 <= capacity) output[(*used)++] = 0;
}

__declspec(dllexport) DWORD GetPrivateProfileStringW(
    const WCHAR *section, const WCHAR *key, const WCHAR *fallback,
    WCHAR *output, DWORD capacity, const WCHAR *filename)
{
    if (!output || !capacity || !filename) {
        SetLastError(87);
        return 0;
    }
    output[0] = 0;
    if (capacity > 1) output[1] = 0;

    WCHAR path[260];
    DWORD length = 0;
    BOOL absolute = (filename[0] && filename[1] == ':') ||
                    filename[0] == '\\' || filename[0] == '/';
    if (!absolute) {
        static const WCHAR prefix[] = {'C',':','\\','W','I','N','D','O','W','S','\\',0};
        while (prefix[length]) { path[length] = prefix[length]; length++; }
    }
    for (DWORD i = 0; filename[i]; i++) {
        if (length + 1 >= 260) { SetLastError(206); return 0; }
        path[length++] = filename[i];
    }
    path[length] = 0;
    HANDLE file = CreateFileW(path, 0x80000000u, 7, 0, 3, 0, 0);
    if (file == NT_INVALID_HANDLE)
        return section && key ? profile_single(output, capacity, 0, 0, 0,
                                                fallback) : 0;
    DWORD high = 0, size = GetFileSize(file, &high);
    if (high || size > 1024 * 1024) {
        CloseHandle(file);
        SetLastError(ERROR_INVALID_DATA);
        return 0;
    }
    BYTE *bytes = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    DWORD read = 0;
    BOOL ok = bytes && ReadFile(file, bytes, size, &read, 0) && read == size;
    CloseHandle(file);
    if (!ok) {
        if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
        SetLastError(bytes ? ERROR_INVALID_DATA : 8);
        return 0;
    }
    PROFILE_TEXT text = {bytes, size, 0, 0};
    if (size >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
        text.Utf16 = 1;
        text.Units = size / 2;
        text.Skip = 1;
    } else if (size >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb &&
               bytes[2] == 0xbf) text.Skip = 3;

    BOOL in_section = 0;
    DWORD used = 0;
    BOOL enumerate = !section || !key;
    DWORD result = 0;
    for (DWORD pos = text.Skip; pos < text.Units; ) {
        DWORD begin = pos;
        while (pos < text.Units && profile_unit(&text, pos) != '\r' &&
               profile_unit(&text, pos) != '\n') pos++;
        DWORD end = pos;
        while (pos < text.Units && (profile_unit(&text, pos) == '\r' ||
               profile_unit(&text, pos) == '\n')) pos++;
        profile_trim(&text, &begin, &end);
        if (begin == end || profile_unit(&text, begin) == ';' ||
            profile_unit(&text, begin) == '#') continue;
        if (profile_unit(&text, begin) == '[') {
            DWORD close = begin + 1;
            while (close < end && profile_unit(&text, close) != ']') close++;
            if (close == end) { in_section = 0; continue; }
            DWORD name_begin = begin + 1, name_end = close;
            profile_trim(&text, &name_begin, &name_end);
            in_section = profile_equal(&text, name_begin, name_end, section);
            if (!section) profile_append(output, capacity, &used, &text,
                                         name_begin, name_end);
            continue;
        }
        if (!section || !in_section) continue;
        DWORD equals = begin;
        while (equals < end && profile_unit(&text, equals) != '=') equals++;
        if (equals == end) continue;
        DWORD name_begin = begin, name_end = equals;
        DWORD value_begin = equals + 1, value_end = end;
        profile_trim(&text, &name_begin, &name_end);
        profile_trim(&text, &value_begin, &value_end);
        if (!key) {
            profile_append(output, capacity, &used, &text,
                           name_begin, name_end);
        } else if (profile_equal(&text, name_begin, name_end, key)) {
            result = profile_single(output, capacity, &text, value_begin,
                                    value_end, 0);
            HeapFree(GetProcessHeap(), 0, bytes);
            return result;
        }
    }
    if (enumerate) {
        if (capacity > 1) output[used < capacity ? used : capacity - 1] = 0;
        result = used ? used - 1 : 0;
    } else {
        result = profile_single(output, capacity, 0, 0, 0, fallback);
    }
    HeapFree(GetProcessHeap(), 0, bytes);
    return result;
}
