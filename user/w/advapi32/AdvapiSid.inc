/* Binary SID and SDDL SID-string operations. Returned strings and parsed SIDs
 * use LocalAlloc, so callers can release them with the Win32 LocalFree. */
extern void *LocalAlloc(DWORD flags, ULONGLONG bytes);
extern void SetLastError(DWORD error);

#define SID_MAX_SUBAUTHORITIES 15
#define SID_ERROR_INVALID_PARAMETER 87u
#define SID_ERROR_INSUFFICIENT_BUFFER 122u
#define SID_ERROR_INVALID_SID 1337u

static void sid_copy(void *out, const void *in, DWORD length)
{
    BYTE *d = out;
    const BYTE *s = in;
    while (length--) *d++ = *s++;
}

static BOOL_K sid_valid(const void *sid)
{
    if (!sid) return FALSE_K;
    const BYTE *raw = sid;
    return raw[0] == 1 && raw[1] <= SID_MAX_SUBAUTHORITIES;
}

__declspec(dllexport) BOOL_K IsValidSid(const void *sid)
{ return sid_valid(sid); }

__declspec(dllexport) DWORD GetLengthSid(const void *sid)
{
    if (!sid_valid(sid)) return 0;
    return 8 + 4 * ((const BYTE *)sid)[1];
}

__declspec(dllexport) BYTE *GetSidSubAuthorityCount(void *sid)
{ return sid_valid(sid) ? (BYTE *)sid + 1 : 0; }

__declspec(dllexport) DWORD *GetSidSubAuthority(void *sid, DWORD index)
{
    if (!sid_valid(sid) || index >= ((BYTE *)sid)[1]) return 0;
    return (DWORD *)((BYTE *)sid + 8 + 4 * index);
}

__declspec(dllexport) BOOL_K EqualSid(const void *left, const void *right)
{
    DWORD size = GetLengthSid(left);
    if (!size || size != GetLengthSid(right)) return FALSE_K;
    const BYTE *a = left, *b = right;
    for (DWORD i = 0; i < size; ++i)
        if (a[i] != b[i]) return FALSE_K;
    return TRUE_K;
}

__declspec(dllexport) BOOL_K CopySid(DWORD destination_size, void *destination,
                                     const void *source)
{
    DWORD size = GetLengthSid(source);
    if (!size || !destination) {
        SetLastError(SID_ERROR_INVALID_SID);
        return FALSE_K;
    }
    if (destination_size < size) {
        SetLastError(SID_ERROR_INSUFFICIENT_BUFFER);
        return FALSE_K;
    }
    sid_copy(destination, source, size);
    return TRUE_K;
}

static void sid_build(BYTE *out, ULONGLONG authority, BYTE count,
                      const DWORD *subauthorities)
{
    out[0] = 1;
    out[1] = count;
    for (int i = 0; i < 6; ++i)
        out[2 + i] = (BYTE)(authority >> ((5 - i) * 8));
    for (BYTE i = 0; i < count; ++i) {
        DWORD value = subauthorities[i];
        for (int j = 0; j < 4; ++j)
            out[8 + 4 * i + j] = (BYTE)(value >> (j * 8));
    }
}

static ULONGLONG sid_authority(const BYTE *sid)
{
    ULONGLONG authority = 0;
    for (int i = 0; i < 6; ++i)
        authority = (authority << 8) | sid[2 + i];
    return authority;
}

static DWORD sid_subauthority(const BYTE *sid, BYTE index)
{
    const BYTE *p = sid + 8 + index * 4;
    return (DWORD)p[0] | ((DWORD)p[1] << 8) |
           ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static int sid_digit(WCHAR c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static BOOL_K sid_parse_number(const WCHAR **text, ULONGLONG max,
                               ULONGLONG *number)
{
    const WCHAR *p = *text;
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    }
    ULONGLONG value = 0;
    int digits = 0;
    while (*p && *p != '-') {
        int digit = sid_digit(*p++);
        if (digit < 0 || digit >= base || value > (max - digit) / base)
            return FALSE_K;
        value = value * base + digit;
        ++digits;
    }
    if (!digits) return FALSE_K;
    *number = value;
    *text = p;
    return TRUE_K;
}

__declspec(dllexport) BOOL_K ConvertStringSidToSidW(const WCHAR *text,
                                                     void **out_sid)
{
    if (!out_sid) return FALSE_K;
    *out_sid = 0;
    if (!text) { SetLastError(SID_ERROR_INVALID_PARAMETER); return FALSE_K; }
    /* Common SDDL aliases used by shell security descriptors. */
    const WCHAR *alias = 0;
    if (text[0] && text[1] && !text[2]) {
        if (text[0] == 'B' && text[1] == 'A') alias = L"S-1-5-32-544";
        else if (text[0] == 'B' && text[1] == 'U') alias = L"S-1-5-32-545";
        else if (text[0] == 'W' && text[1] == 'D') alias = L"S-1-1-0";
        else if (text[0] == 'S' && text[1] == 'Y') alias = L"S-1-5-18";
        else if (text[0] == 'A' && text[1] == 'U') alias = L"S-1-5-11";
    }
    if (alias) text = alias;
    if (text[0] != 'S' || text[1] != '-') goto invalid;
    text += 2;
    ULONGLONG revision, authority, value;
    DWORD subauthorities[SID_MAX_SUBAUTHORITIES];
    BYTE count = 0;
    if (!sid_parse_number(&text, 255, &revision) || revision != 1 ||
        *text++ != '-' ||
        !sid_parse_number(&text, 0xffffffffffffULL, &authority))
        goto invalid;
    while (*text) {
        if (*text++ != '-' || count == SID_MAX_SUBAUTHORITIES ||
            !sid_parse_number(&text, 0xffffffffULL, &value))
            goto invalid;
        subauthorities[count++] = (DWORD)value;
    }
    BYTE *sid = LocalAlloc(0, 8 + 4 * count);
    if (!sid) return FALSE_K;
    sid_build(sid, authority, count, subauthorities);
    *out_sid = sid;
    return TRUE_K;
invalid:
    SetLastError(SID_ERROR_INVALID_SID);
    return FALSE_K;
}

static WCHAR *sid_format_number(WCHAR *out, ULONGLONG value)
{
    WCHAR reverse[20];
    int length = 0;
    do { reverse[length++] = (WCHAR)('0' + value % 10); value /= 10; }
    while (value);
    while (length) *out++ = reverse[--length];
    return out;
}

__declspec(dllexport) BOOL_K ConvertSidToStringSidW(const void *sid,
                                                     WCHAR **out_text)
{
    if (!out_text) return FALSE_K;
    *out_text = 0;
    if (!sid_valid(sid)) {
        SetLastError(SID_ERROR_INVALID_SID);
        return FALSE_K;
    }
    const BYTE *raw = sid;
    WCHAR *text = LocalAlloc(0, 192 * sizeof(WCHAR));
    if (!text) return FALSE_K;
    WCHAR *p = text;
    *p++ = 'S'; *p++ = '-'; *p++ = '1'; *p++ = '-';
    p = sid_format_number(p, sid_authority(raw));
    for (BYTE i = 0; i < raw[1]; ++i) {
        *p++ = '-';
        p = sid_format_number(p, sid_subauthority(raw, i));
    }
    *p = 0;
    *out_text = text;
    return TRUE_K;
}

__declspec(dllexport) BOOL_K CreateWellKnownSid(DWORD type,
                                                 const void *domain_sid,
                                                 void *out_sid, DWORD *size)
{
    (void)domain_sid;
    if (!size) return FALSE_K;
    ULONGLONG authority = 5;
    DWORD parts[2] = {0, 0};
    BYTE count = 1;
    switch (type) {
    case 1: authority = 1; parts[0] = 0; break; /* World */
    case 2: authority = 2; parts[0] = 0; break; /* Local */
    case 9: parts[0] = 2; break; /* Network */
    case 11: parts[0] = 4; break; /* Interactive */
    case 12: parts[0] = 6; break; /* Service */
    case 13: parts[0] = 7; break; /* Anonymous */
    case 17: parts[0] = 11; break; /* Authenticated user */
    case 18: parts[0] = 12; break; /* Restricted */
    case 22: parts[0] = 18; break; /* LocalSystem */
    case 23: parts[0] = 19; break; /* LocalService */
    case 24: parts[0] = 20; break; /* NetworkService */
    case 25: parts[0] = 32; break; /* Builtin domain */
    case 26: case 27: case 28: case 29: case 36:
        parts[0] = 32;
        parts[1] = type == 26 ? 544 : type == 27 ? 545 :
                   type == 28 ? 546 : type == 29 ? 547 : 555;
        count = 2;
        break;
    default:
        SetLastError(SID_ERROR_INVALID_PARAMETER);
        return FALSE_K;
    }
    DWORD needed = 8 + 4 * count;
    if (!out_sid || *size < needed) {
        *size = needed;
        SetLastError(SID_ERROR_INSUFFICIENT_BUFFER);
        return FALSE_K;
    }
    sid_build(out_sid, authority, count, parts);
    *size = needed;
    return TRUE_K;
}
