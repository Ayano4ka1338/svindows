/* UNICODE_STRING, ANSI/wide conversion and string helpers.
 * Included by user/w/ntdll/Ntdllrtl.c. */
/* ------------------------------------------------------------------ */
/* UNICODE_STRING helpers                                              */
/* ------------------------------------------------------------------ */

typedef struct _UNICODE_STRING_NT {
    WORD Length;             /* bytes */
    WORD MaximumLength;
    WCHAR *Buffer;
} UNICODE_STRING_NT;

typedef struct _STRING_NT {
    WORD Length;
    WORD MaximumLength;
    char *Buffer;
} STRING_NT;

void RtlInitUnicodeString(UNICODE_STRING_NT *ustr, PCWSTR text)
{
    UINT n = 0;
    while (text && text[n])
        n++;
    ustr->Length = (WORD)(n * sizeof(WCHAR));
    ustr->MaximumLength = (WORD)(n ? n * sizeof(WCHAR) + 2 : 0);
    ustr->Buffer = (WCHAR *)text;
}

void RtlInitAnsiString(STRING_NT *astr, const char *text)
{
    UINT n = 0;
    while (text && text[n])
        n++;
    astr->Length = (WORD)n;
    astr->MaximumLength = (WORD)(n + 1);
    astr->Buffer = (char *)text;
}

NTSTATUS RtlFreeUnicodeString(UNICODE_STRING_NT *ustr)
{
    if (ustr && ustr->Buffer)
        RtlFreeHeap(0, 0, ustr->Buffer);
    if (ustr)
        memset(ustr, 0, sizeof(*ustr));
    return STATUS_SUCCESS;
}

LONG RtlCompareUnicodeString(const UNICODE_STRING_NT *a,
                             const UNICODE_STRING_NT *b, ULONG case_sensitive)
{
    UINT na = a->Length / 2, nb = b->Length / 2;
    UINT i;
    for (i = 0; i < na && i < nb; i++) {
        WCHAR ca = a->Buffer[i], cb = b->Buffer[i];
        if (!case_sensitive) {
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
        }
        if (ca != cb)
            return (LONG)ca - (LONG)cb;
    }
    return (LONG)(na - nb);
}

BOOLEAN RtlEqualUnicodeString(const UNICODE_STRING_NT *a,
                              const UNICODE_STRING_NT *b,
                              ULONG case_sensitive)
{
    return RtlCompareUnicodeString(a, b, case_sensitive) == 0;
}

NTSTATUS RtlAnsiStringToUnicodeString(UNICODE_STRING_NT *dst,
                                      const STRING_NT *src, BOOLEAN allocate)
{
    UINT chars = src->Length;
    if (!allocate) {
        if (dst->MaximumLength < (WORD)((chars + 1) * sizeof(WCHAR)))
            return 0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    } else {
        dst->Buffer =
            RtlAllocateHeap(0, 0, (chars + 1) * sizeof(WCHAR));
        if (!dst->Buffer)
            return STATUS_NO_MEMORY;
        dst->MaximumLength = (WORD)((chars + 1) * sizeof(WCHAR));
    }
    for (UINT i = 0; i < chars; i++)
        dst->Buffer[i] = (WCHAR)(BYTE)src->Buffer[i];
    dst->Buffer[chars] = 0;
    dst->Length = (WORD)(chars * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

NTSTATUS RtlCreateUnicodeStringFromAsciiz(UNICODE_STRING_NT *out,
                                          const char *text)
{
    UINT n = 0;
    while (text[n])
        n++;
    out->Buffer = RtlAllocateHeap(0, 8 /* zero */, (n + 1) * sizeof(WCHAR));
    if (!out->Buffer)
        return STATUS_NO_MEMORY;
    for (UINT i = 0; i < n; i++)
        out->Buffer[i] = (WCHAR)(BYTE)text[i];
    out->Buffer[n] = 0;
    out->Length = (WORD)(n * sizeof(WCHAR));
    out->MaximumLength = (WORD)((n + 1) * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

NTSTATUS RtlMultiByteToUnicodeN(WCHAR *dst, ULONG dst_bytes, ULONG *written,
                                const char *src, ULONG src_len)
{
    ULONG n = src_len;
    if (n > dst_bytes / 2)
        n = dst_bytes / 2;
    for (ULONG i = 0; i < n; i++)
        dst[i] = (WCHAR)(BYTE)src[i];
    if (written)
        *written = n * 2;
    return STATUS_SUCCESS;
}

/* The OEM code page is 437. The first 128 byte values are ASCII. */
static const WCHAR oem437_high[128] = {
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7,
    0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
    0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
    0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192,
    0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba,
    0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
    0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
    0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
    0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
    0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4,
    0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229,
    0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
    0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
};

NTSTATUS RtlOemToUnicodeN(WCHAR *out, ULONG out_bytes, ULONG *written,
                          const char *source, ULONG source_bytes)
{
    if (written) *written = 0;
    ULONG count = out_bytes / sizeof(WCHAR);
    if (count > source_bytes) count = source_bytes;
    if (count && (!out || !source)) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < count; i++) {
        BYTE value = (BYTE)source[i];
        out[i] = value < 0x80 ? value : oem437_high[value - 0x80];
    }
    if (written) *written = count * sizeof(WCHAR);
    return STATUS_SUCCESS;
}

static char unicode_to_oem437(WCHAR value)
{
    if (value < 0x80) return (char)value;
    for (ULONG i = 0; i < sizeof(oem437_high) / sizeof(oem437_high[0]); i++)
        if (oem437_high[i] == value) return (char)(0x80 + i);
    return '?';
}

NTSTATUS RtlUnicodeToOemN(char *out, ULONG out_bytes, ULONG *written,
                          const WCHAR *source, ULONG source_bytes)
{
    if (written) *written = 0;
    ULONG count = source_bytes / sizeof(WCHAR);
    if (count > out_bytes) count = out_bytes;
    if (count && (!out || !source)) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < count; i++) out[i] = unicode_to_oem437(source[i]);
    if (written) *written = count;
    return STATUS_SUCCESS;
}

NTSTATUS RtlMultiByteToUnicodeSize(ULONG *size, const char *src, ULONG len)
{
    (void)src;
    *size = len * 2;
    return STATUS_SUCCESS;
}

NTSTATUS RtlUnicodeToMultiByteN(char *dst, ULONG dst_len, ULONG *written,
                                const WCHAR *src, ULONG src_bytes)
{
    ULONG n = src_bytes / 2;
    if (n > dst_len)
        n = dst_len;
    for (ULONG i = 0; i < n; i++)
        dst[i] = (char)(src[i] < 0x100 ? src[i] : '?');
    if (written)
        *written = n;
    return STATUS_SUCCESS;
}

NTSTATUS RtlUnicodeToMultiByteSize(ULONG *size, const WCHAR *src,
                                   ULONG src_bytes)
{
    (void)src;
    *size = src_bytes / 2;
    return STATUS_SUCCESS;
}

NTSTATUS RtlUnicodeStringToInteger(const UNICODE_STRING_NT *ustr,
                                   ULONG base, ULONG *value)
{
    if (!ustr || !value)
        return STATUS_INVALID_PARAMETER;
    UINT n = ustr->Length / 2;
    UINT i = 0;
    while (i < n &&
           (ustr->Buffer[i] == ' ' ||
            (ustr->Buffer[i] >= 9 && ustr->Buffer[i] <= 13)))
        i++;
    ULONG negative = 0;
    if (i < n && (ustr->Buffer[i] == '-' || ustr->Buffer[i] == '+')) {
        negative = ustr->Buffer[i] == '-';
        i++;
    }
    if (!base || base == 2 || base == 8 || base == 10 || base == 16) {
        /* default detection when base==0 */
    } else
        return STATUS_INVALID_PARAMETER;
    if (!base) {
        base = 10;
        if (i + 1 < n && ustr->Buffer[i] == '0' &&
            (ustr->Buffer[i + 1] | 32) == 'x') {
            base = 16;
            i += 2;
        } else if (i < n && ustr->Buffer[i] == '0')
            base = 8;
    }
    ULONGLONG acc = 0;
    BOOLEAN any = FALSE;
    for (; i < n; i++) {
        WCHAR c = ustr->Buffer[i];
        ULONG d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if ((c | 32) >= 'a' && (c | 32) <= 'z') d = (c | 32) - 'a' + 10;
        else break;
        if (d >= base) break;
        acc = acc * base + d;
        any = TRUE;
    }
    if (!any)
        return STATUS_INVALID_PARAMETER;
    *value = negative ? (ULONG)-(LONG)acc : (ULONG)acc;
    return STATUS_SUCCESS;
}

BOOLEAN RtlGetIntegerAtom(PCWSTR text, WORD *atom)
{
    if (!text)
        return FALSE;
    UINT i = 0;
    if (text[i] == '#')
        i++;
    ULONG value = 0;
    BOOLEAN any = FALSE;
    for (; text[i]; i++) {
        if (text[i] < '0' || text[i] > '9')
            return FALSE;
        value = value * 10 + (ULONG)(text[i] - '0');
        if (value > 65535)
            return FALSE;
        any = TRUE;
    }
    if (!any || !value)
        return FALSE;
    if (atom)
        *atom = (WORD)value;
    return TRUE;
}

NTSTATUS RtlPrefixString(const STRING_NT *prefix, const STRING_NT *str,
                         BOOLEAN case_sensitive)
{
    if (prefix->Length > str->Length)
        return STATUS_INVALID_PARAMETER; /* not a prefix */
    for (WORD i = 0; i < prefix->Length; i++) {
        char a = str->Buffer[i], b = prefix->Buffer[i];
        if (!case_sensitive) {
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
        }
        if (a != b)
            return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

ULONG RtlLengthRequiredSid(BYTE count)
{
    return 8ul + 4ul * count;
}

/* ------------------------------------------------------------------ */
/* Wide/ANSI string and conversion surface                             */
/* ------------------------------------------------------------------ */

int wcscmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)*a - (int)*b;
}

int _wcsicmp(const WCHAR *a, const WCHAR *b)
{
    for (;;) {
        WCHAR ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca)
            return (LONG)ca - (LONG)cb;
    }
}

int _wcsnicmp(const WCHAR *a, const WCHAR *b, SIZE_T count)
{
    for (SIZE_T i = 0; i < count; i++) {
        WCHAR ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca)
            return (LONG)ca - (LONG)cb;
    }
    return 0;
}

SIZE_T wcslen(const WCHAR *s)
{
    SIZE_T n = 0;
    while (s[n])
        n++;
    return n;
}

WCHAR *wcschr(const WCHAR *text, WCHAR value)
{
    for (;;) {
        if (*text == value) return (WCHAR *)text;
        if (!*text) return 0;
        text++;
    }
}

ULONG RtlLengthSid(const void *sid)
{
    if (!sid) return 0;
    const BYTE *bytes = (const BYTE *)sid;
    if (bytes[0] != 1 || bytes[1] > 15) return 0;
    return 8u + 4u * bytes[1];
}

static volatile LONG g_pointer_key_state;
static ULONGLONG g_pointer_key;

static ULONGLONG pointer_key(void)
{
    LONG state = g_pointer_key_state;
    if (state == 2) return g_pointer_key;
    if (__sync_bool_compare_and_swap(&g_pointer_key_state, 0, 1)) {
        ULONG lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        ULONGLONG seed = ((ULONGLONG)hi << 32) | lo;
        seed ^= read_teb_qword(0x60);
        seed ^= read_teb_qword(0x40) * 0x9e3779b97f4a7c15ULL;
        seed ^= seed >> 30;
        seed *= 0xbf58476d1ce4e5b9ULL;
        seed ^= seed >> 27;
        seed *= 0x94d049bb133111ebULL;
        seed ^= seed >> 31;
        g_pointer_key = seed ? seed : 0xd6e8feb86659fd93ULL;
        __sync_synchronize();
        g_pointer_key_state = 2;
        return g_pointer_key;
    }
    while (g_pointer_key_state == 1) __asm__ volatile("pause");
    return g_pointer_key;
}

void *RtlEncodePointer(void *pointer)
{
    ULONGLONG value = (ULONGLONG)pointer ^ pointer_key();
    return (void *)((value << 17) | (value >> 47));
}

void *RtlDecodePointer(void *pointer)
{
    ULONGLONG value = (ULONGLONG)pointer;
    value = (value >> 17) | (value << 47);
    return (void *)(value ^ pointer_key());
}

NTSTATUS RtlCopySid(ULONG capacity, void *destination, const void *source)
{
    ULONG length = RtlLengthSid(source);
    if (!destination || !length) return STATUS_INVALID_PARAMETER;
    if (capacity < length) return (NTSTATUS)0xC0000023L;
    memcpy(destination, source, length);
    return STATUS_SUCCESS;
}

WCHAR *wcsncpy_s(WCHAR *dst, SIZE_T cap, const WCHAR *src, SIZE_T count)
{
    if (!dst || !cap)
        return dst;
    SIZE_T n = 0;
    while (src && src[n] && n < count - 1)
        n++;
    memcpy(dst, src, n * sizeof(WCHAR));
    dst[n] = 0;
    return dst;
}

NTSTATUS wcscpy_s(WCHAR *dst, SIZE_T cap, const WCHAR *src)
{
    if (!dst || !cap)
        return STATUS_INVALID_PARAMETER;
    SIZE_T n = wcslen(src ? src : L"");
    if (n >= cap) {
        dst[0] = 0;
        return 0x8007007AL; /* E_INVALIDARG-ish buffer error */
    }
    memcpy(dst, src, (n + 1) * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

NTSTATUS wcscat_s(WCHAR *dst, SIZE_T cap, const WCHAR *src)
{
    SIZE_T used = wcslen(dst);
    return wcscpy_s(dst + used, cap - used, src);
}

NTSTATUS wcsncat_s(WCHAR *dst, SIZE_T cap, const WCHAR *src, SIZE_T count)
{
    SIZE_T used = wcslen(dst);
    if (used >= cap)
        return STATUS_INVALID_PARAMETER;
    SIZE_T i = 0;
    while (i < count && src[i]) {
        if (used + i >= cap - 1)
            return STATUS_INVALID_PARAMETER;
        dst[used + i] = src[i];
        i++;
    }
    dst[used + i] = 0;
    return STATUS_SUCCESS;
}

const WCHAR *wcsrchr(const WCHAR *text, WCHAR c)
{
    const WCHAR *found = 0;
    for (; *text; text++)
        if (*text == c)
            found = text;
    return found;
}

const char *strrchr(const char *text, int c)
{
    const char *found = 0;
    for (; *text; text++)
        if (*text == (char)c)
            found = text;
    return found;
}

int _stricmp(const char *a, const char *b)
{
    for (;;) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca)
            return (unsigned char)ca - (unsigned char)cb;
    }
}

int _strnicmp(const char *a, const char *b, SIZE_T count)
{
    for (SIZE_T i = 0; i < count; i++) {
        BYTE left = (BYTE)a[i], right = (BYTE)b[i];
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right || !left) return (int)left - (int)right;
    }
    return 0;
}

char *strncpy(char *destination, const char *source, SIZE_T count)
{
    SIZE_T i = 0;
    while (i < count && source[i]) {
        destination[i] = source[i];
        i++;
    }
    while (i < count) destination[i++] = 0;
    return destination;
}

ULONG __cdecl _wtoi(const WCHAR *text)
{
    ULONG v = 0;
    BOOLEAN negative = FALSE;
    if (*text == '-') {
        negative = TRUE;
        text++;
    }
    while (*text >= '0' && *text <= '9')
        v = v * 10 + (ULONG)(*text++ - '0');
    return negative ? (ULONG)-(LONG)v : v;
}

NTSTATUS wcstol(const WCHAR *text, WCHAR **end, LONG base, LONG *out)
{
    ULONG v = 0;
    BOOLEAN any = FALSE, neg = FALSE;
    if (*text == '-') {
        neg = TRUE;
        text++;
    }
    for (;;) {
        ULONG d;
        if (*text >= '0' && *text <= '9') d = (ULONG)(*text - '0');
        else if (base == 16 && (*text | 32) >= 'a' && (*text | 32) <= 'f')
            d = (ULONG)((*text | 32) - 'a' + 10);
        else break;
        if (d >= (ULONG)base) break;
        v = v * (ULONG)base + d;
        any = TRUE;
        text++;
    }
    if (end)
        *end = (WCHAR *)(any ? text : (const WCHAR *)text);
    *out = neg ? -(LONG)v : (LONG)v;
    return any ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

NTSTATUS wcstoul(const WCHAR *text, WCHAR **end, LONG base, ULONG *out)
{
    LONG signed_out = 0;
    NTSTATUS st = wcstol(text, end, base, &signed_out);
    *out = (ULONG)signed_out;
    return st;
}

WCHAR *_ultow_s(ULONG value, WCHAR *dst, SIZE_T cap, LONG base)
{
    WCHAR tmp[24];
    UINT n = 0;
    if (!base || base > 36)
        base = 10;
    do {
        ULONG d = value % (ULONG)base;
        tmp[n++] = (WCHAR)(d < 10 ? '0' + d : 'a' + d - 10);
        value /= (ULONG)base;
    } while (value);
    if (!cap || n + 1 > cap)
        return 0;
    for (UINT i = 0; i < n; i++)
        dst[i] = tmp[n - 1 - i];
    dst[n] = 0;
    return dst;
}

int isspace(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

int iswspace(WCHAR c)
{
    return c == ' ' || (c >= 9 && c <= 13);
}

void qsort(void *base, SIZE_T count, SIZE_T width,
           int (*compare)(const void *, const void *))
{
    BYTE *b = (BYTE *)base;
    for (SIZE_T i = 1; i < count; i++) {
        BYTE key[64];
        if (width > sizeof(key))
            return; /* refuse oversized elements */
        memcpy(key, b + i * width, width);
        SIZE_T j = i;
        while (j && compare(b + (j - 1) * width, key) > 0) {
            memcpy(b + j * width, b + (j - 1) * width, width);
            j--;
        }
        memcpy(b + j * width, key, width);
    }
}

/* ------------------------------------------------------------------ */
/* Error state, misc queries                                           */
/* ------------------------------------------------------------------ */

void RtlSetLastWin32Error(DWORD error)
{
    write_teb_dword(0x68, error);
}

DWORD RtlGetLastWin32Error(void)
{
    return read_teb_dword(0x68);
}

DWORD RtlNtStatusToDosError(NTSTATUS status)
{
    switch ((ULONG)status) {
    case 0xC0000034: return 2;   /* OBJECT_NAME_NOT_FOUND */
    case 0xC0000008: return 6;   /* INVALID_HANDLE */
    case 0xC0000022: return 5;   /* ACCESS_DENIED */
    case 0xC0000033: return 123; /* INVALID_NAME */
    case 0xC0000035: return 183; /* ALREADY_EXISTS */
    case 0xC0000023: return 122; /* INSUFFICIENT_BUFFER */
    case 0xC000007C: return 1008; /* NO_TOKEN */
    case 0xC0000121: return 5; /* CANNOT_DELETE */
    case 0x80000005: return 234; /* BUFFER_OVERFLOW / MORE_DATA */
    case 0x8000001A: return 259; /* NO_MORE_ITEMS */
    case 0xC0000017: return 8;   /* NO_MEMORY */
    case 0xC000007F: return 112; /* DISK_FULL */
    case 0xC000000D: return 87;  /* INVALID_PARAMETER */
    case 0xC0000225: return 1168; /* NOT_FOUND */
    case 0:          return 0;
    default:         return 1;   /* INVALID_FUNCTION */
    }
}

DWORD RtlNtStatusToDosErrorNoTeb(NTSTATUS status)
{
    return RtlNtStatusToDosError(status);
}

BOOLEAN RtlIsThreadWithinLoaderCallout(void)
{
    return FALSE;
}

void *RtlPcToFileHeader(void *pc, void **base)
{
    if (base)
        *base = 0;
    return 0;
}

ULONGLONG RtlGetActiveConsoleId(void)
{
    return 0;
}

WORD RtlGetThreadLangIdByIndex(DWORD index)
{
    (void)index;
    return 0x0409;
}

/* Exported DATA symbol: real binaries dereference it as WORD *. */
WORD NlsAnsiCodePage = 1252;

NTSTATUS RtlOpenCurrentUser(DWORD access, HANDLE *key)
{
    (void)access;
    if (!key)
        return STATUS_INVALID_PARAMETER;
    *key = (HANDLE)0x80000001ULL; /* HKEY_CURRENT_USER */
    return STATUS_SUCCESS;
}

BOOLEAN RtlGetNtProductType(ULONG *product)
{
    if (product)
        *product = 1; /* WINNT */
    return TRUE;
}

NTSTATUS RtlQueryElevationFlags(ULONG *flags)
{
    if (flags)
        *flags = 0;
    return STATUS_SUCCESS;
}

NTSTATUS RtlCheckRegistryKey(ULONG relative, const WCHAR *path)
{
    (void)relative; (void)path;
    return STATUS_NOT_FOUND;
}

ULONG RtlMapGenericMask(ULONG access, const void *mapping)
{
    /* The GENERIC mapping structure is {GenericRead, GenericWrite,
     * GenericExecute, GenericAll}; apply it literally. */
    const ULONG *m = (const ULONG *)mapping;
    ULONG result = 0;
    if (access & 0x80000000UL) result |= m[0];
    if (access & 0x40000000UL) result |= m[1];
    if (access & 0x20000000UL) result |= m[2];
    if (access & 0x10000000UL) result |= m[3];
    return result;
}

void RtlRunEncodeUnicodeString(void *seed, UNICODE_STRING_NT *ustr)
{
    (void)seed;
    (void)ustr;
}

void RtlRunDecodeUnicodeString(void *seed, UNICODE_STRING_NT *ustr)
{
    (void)seed;
    (void)ustr;
}

ULONG RtlRandomEx(ULONG *seed)
{
    *seed = *seed * 1103515245 + 12345;
    return *seed;
}

BOOLEAN RtlDllShutdownInProgress(void)
{
    return FALSE;
}

NTSTATUS RtlGetLastNtStatus(void)
{
    return (NTSTATUS)read_teb_dword(0x50); /* TEB LastStatusValue */
}

BOOLEAN RtlCreateUnicodeString(UNICODE_STRING_NT *out, const WCHAR *source)
{
    if (!out || !source) return 0;
    ULONG count = 0;
    while (count < 32767 && source[count]) count++;
    if (count == 32767) return 0;
    WCHAR *copy = RtlAllocateHeap(0, 0, (count + 1) * sizeof(*copy));
    if (!copy) return 0;
    memcpy(copy, source, (count + 1) * sizeof(*copy));
    *out = (UNICODE_STRING_NT){(WORD)(count * sizeof(*copy)),
        (WORD)((count + 1) * sizeof(*copy)), copy};
    return 1;
}
