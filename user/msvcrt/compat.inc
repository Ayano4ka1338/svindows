/* Extended CRT and legacy MSVC startup compatibility functions.
 * Included by user/msvcrt.c. */
/* ------------------------------------------------------------------ */
/* CRT surface the inbox shell binaries link directly                  */
/* ------------------------------------------------------------------ */

/* Floating-point code the compiler emits expects this sentinel linked in. */
int _fltused;

__declspec(dllexport) void *realloc(void *p, SIZE_T n)
{
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return 0;
    }
    void *fresh = malloc(n);
    if (fresh)
        memcpy(fresh, p, n);
    free(p);
    return fresh;
}

/* The CRT lock set: real msvcrt guards its streams and the locale with a
 * fixed array of locks. We are single-threaded per stream, so the ids are
 * accepted and remembered but not enforced. */
__declspec(dllexport) void _lock(int locknum) { (void)locknum; }
__declspec(dllexport) void _unlock(int locknum) { (void)locknum; }

/* atexit chains: DLLs register their teardown callbacks here. They only run
 * at process exit, which is ExitProcess for every program this CRT hosts. */
static void (*g_atexit_chain[64])(void);
static int g_atexit_count;
__declspec(dllexport) int __dllonexit(void (*fn)(void), void ***begin,
                                      void ***end)
{
    (void)begin; (void)end;
    if (g_atexit_count >= 64)
        return 0;
    g_atexit_chain[g_atexit_count++] = fn;
    return 1;
}
__declspec(dllexport) int _onexit(void (*fn)(void))
{
    if (g_atexit_count >= 64)
        return 0;
    g_atexit_chain[g_atexit_count++] = fn;
    return 1;
}

__declspec(dllexport) void abort(void)
{
    ExitProcess(3);
}

/* errno accessors: the classic CRT exports the variable through helpers. */
__declspec(dllexport) int _get_errno(int *value)
{
    if (value)
        *value = g_errno;
    return 0;
}
__declspec(dllexport) int _set_errno(int value)
{
    g_errno = value;
    return 0;
}

/* Math leaf routines over the FPU; explorer's layout and telemetry paths
 * call them with plain finite values. */
__declspec(dllexport) double floor(double x)
{
    return (double)(long long)x - (x < 0 && x != (double)(long long)x ? 1 : 0);
}
__declspec(dllexport) double ceil(double x)
{
    double f = floor(x);
    return f == x ? x : f + 1.0;
}
static double kfabs(double x) { return x < 0 ? -x : x; }
__declspec(dllexport) double sqrt(double x)
{
    if (x <= 0)
        return 0;
    double r = x > 1 ? x : 1, prev = 0;
    while (kfabs(r - prev) > 1e-12 * (r > 1 ? r : 1)) {
        prev = r;
        r = (r + x / r) / 2;
    }
    return r;
}
__declspec(dllexport) double pow(double base, double exp)
{
    if (exp == 0)
        return 1;
    if (base == 0)
        return 0;
    int e = (int)exp;
    if ((double)e == exp) {
        double acc = 1;
        for (int i = 0; i < kfabs((double)e); i++)
            acc *= base;
        return e > 0 ? acc : 1 / acc;
    }
    return 0; /* fractional exponents are not used by the shell paths */
}
__declspec(dllexport) float floorf(float x) { return (float)floor(x); }
__declspec(dllexport) float ceilf(float x) { return (float)ceil(x); }

/* Time: a monotonic-ish epoch over GetTickCount keeps difftime/mktime/localtime
 * self-consistent without timezone rules. */
typedef long long __time_t_shim;
__declspec(dllexport) __time_t_shim time(__time_t_shim *out)
{
    return 0;
}
__declspec(dllexport) double difftime(__time_t_shim a, __time_t_shim b)
{
    return (double)(a - b);
}
struct tm_shim { int sec, min, hour, mday, mon, year, wday, yday, isdst; };
static struct tm_shim g_tm;
__declspec(dllexport) struct tm_shim *localtime(const __time_t_shim *t)
{
    (void)t;
    memset(&g_tm, 0, sizeof(g_tm));
    return &g_tm;
}
__declspec(dllexport) __time_t_shim mktime(struct tm_shim *tm)
{
    (void)tm;
    return 0;
}

__declspec(dllexport) int iswalnum(WCHAR c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

__declspec(dllexport) SIZE_T wcsspn(const WCHAR *s, const WCHAR *accept)
{
    SIZE_T n = 0;
    for (; *s; s++) {
        const WCHAR *a = accept;
        while (*a && *a != *s)
            a++;
        if (!*a)
            break;
        n++;
    }
    return n;
}

__declspec(dllexport) const WCHAR *wcsstr(const WCHAR *text,
                                          const WCHAR *needle)
{
    if (!*needle)
        return text;
    for (; *text; text++) {
        const WCHAR *t = text, *n = needle;
        while (*t && *n && *t == *n) {
            t++;
            n++;
        }
        if (!*n)
            return text;
    }
    return 0;
}

__declspec(dllexport) int _wtoi(const WCHAR *s)
{
    int value = 0, sign = 1;
    if (!s)
        return 0;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9')
        value = value * 10 + (*s++ - '0');
    return sign * value;
}

__declspec(dllexport) unsigned long long _wcstoui64(const WCHAR *s,
                                                    WCHAR **end, int base)
{
    unsigned long long value = 0;
    if (!s)
        return 0;
    if ((!base || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    }
    if (!base)
        base = 10;
    for (; *s; s++) {
        int digit;
        if (*s >= '0' && *s <= '9')
            digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            digit = *s - 'A' + 10;
        else
            break;
        if (digit >= base)
            break;
        value = value * (unsigned)base + (unsigned)digit;
    }
    if (end)
        *end = (WCHAR *)s;
    return value;
}

__declspec(dllexport) SIZE_T wcstombs(char *dst, const WCHAR *src, SIZE_T max)
{
    SIZE_T n = 0;
    if (!dst) {
        for (; src[n]; n++)
            ;
        return n + 1;
    }
    for (; src[n] && n + 1 < max; n++)
        dst[n] = (char)src[n];
    dst[n] = 0;
    return n;
}

/* The _s string family: every caller passes a real buffer; the constraint is
 * honored by truncation rather than the invalid-parameter handler. */
__declspec(dllexport) int memmove_s(void *dst, SIZE_T dstsz,
                                    const void *src, SIZE_T count)
{
    if (!dst || !src)
        return 22 /* EINVAL */;
    if (count > dstsz)
        return 34 /* ERANGE */;
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        for (SIZE_T i = 0; i < count; i++)
            d[i] = s[i];
    } else {
        for (SIZE_T i = count; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return 0;
}

__declspec(dllexport) int wcscpy_s(WCHAR *dst, SIZE_T dstsz,
                                   const WCHAR *src)
{
    if (!dst || !src)
        return 22;
    SIZE_T n = 0;
    while (src[n])
        n++;
    if (n + 1 > dstsz)
        return 34;
    for (SIZE_T i = 0; i <= n; i++)
        dst[i] = src[i];
    return 0;
}

__declspec(dllexport) int wcsncpy_s(WCHAR *dst, SIZE_T dstsz,
                                    const WCHAR *src, SIZE_T count)
{
    if (!dst || !src || !dstsz)
        return 22;
    SIZE_T n = 0;
    while (n < count && src[n])
        n++;
    if (n + 1 > dstsz)
        return 34;
    for (SIZE_T i = 0; i < n; i++)
        dst[i] = src[i];
    dst[n] = 0;
    return 0;
}

__declspec(dllexport) void *bsearch(const void *key, const void *base,
                                    SIZE_T count, SIZE_T size,
                                    int (*compare)(const void *, const void *))
{
    if (!key || !base || !compare)
        return 0;
    SIZE_T low = 0, high = count;
    while (low < high) {
        SIZE_T mid = low + (high - low) / 2;
        const void *elem = (const char *)base + mid * size;
        int cmp = compare(key, elem);
        if (cmp == 0)
            return (void *)elem;
        if (cmp < 0)
            high = mid;
        else
            low = mid + 1;
    }
    return 0;
}

/*
 * MSVC C++ exception base class. Explorer and the inbox DLLs construct
 * std::exception objects for error paths that we cannot yet throw through
 * __CxxFrameHandler3, but the constructors and what() must behave: the
 * object layout is { vptr, char *message, int doFree }.
 */
typedef struct _msvc_exception {
    void *Vtable;
    const char *Message;
    int DoFree;
} msvc_exception;

static const char *g_exception_what = "unknown exception";

/* std::exception surface: the ctors/dtor and what() are exported under their
 * MSVC-mangled names via msvcrt.def so C++ modules construct real objects. */
__declspec(dllexport) void msvcrt_exception_ctor0(msvc_exception *self)
{
    self->Vtable = 0;
    self->Message = g_exception_what;
    self->DoFree = 0;
}

__declspec(dllexport) void msvcrt_exception_ctor_msg(msvc_exception *self,
                                                     const char *message)
{
    self->Vtable = 0;
    self->Message = message;
    self->DoFree = 0;
}

__declspec(dllexport) void msvcrt_exception_ctor_copy(msvc_exception *self,
                                                      const msvc_exception *other)
{
    self->Vtable = other->Vtable;
    self->Message = other->Message;
    self->DoFree = 0;
}

__declspec(dllexport) void msvcrt_exception_dtor(msvc_exception *self)
{
    self->Message = 0;
}

__declspec(dllexport) const char *msvcrt_exception_what(const msvc_exception *self)
{
    return self->Message ? self->Message : g_exception_what;
}

/* String comparison the inbox path helpers need; the loader falls back to
 * this module when ntdll does not export them. */
__declspec(dllexport) int wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
        if (!a[i])
            return 0;
    }
    return 0;
}

__declspec(dllexport) int strncmp(const char *a, const char *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (unsigned char)a[i] < (unsigned char)b[i] ? -1 : 1;
        if (!a[i])
            return 0;
    }
    return 0;
}

__declspec(dllexport) int _wcsnicmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        WCHAR ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (!a[i])
            return 0;
    }
    return 0;
}

__declspec(dllexport) int _strnicmp(const char *a, const char *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return (unsigned char)ca < (unsigned char)cb ? -1 : 1;
        if (!a[i])
            return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The classic MSVC startup surface the Windows inbox utilities use    */
/*                                                                     */
/* notepad.exe and its siblings are built against the old msvcrt CRT   */
/* rather than the UCRT: their entry point calls __getmainargs, walks   */
/* the initializer tables through _initterm and only then reaches       */
/* WinMain. These are the remaining pieces of that surface.            */
/* ------------------------------------------------------------------ */

/* Word-split a command line the way the CRT does: whitespace separates
 * arguments and a double quote protects a run containing spaces. */
static int crt_split_args(char *line, char **argv, int max)
{
    int count = 0;
    char *p = line;
    while (*p && count < max) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        int quoted = 0;
        if (*p == '"') {
            quoted = 1;
            p++;
        }
        argv[count++] = p;
        while (*p && (quoted ? *p != '"' : (*p != ' ' && *p != '\t')))
            p++;
        if (*p)
            *p++ = 0;
    }
    return count;
}

#define CRT_MAX_ARGS 64

/* int __getmainargs(int *argc, char ***argv, char ***env, int wildcards,
 *                   int *new_mode) -- 0 on success. The argument vector points
 * into a private copy of _acmdln, because the CRT is entitled to keep it. */
__declspec(dllexport) int __getmainargs(int *argc, char ***argv, char ***env,
                                        int expand_wildcards, int *new_mode)
{
    (void)expand_wildcards;
    (void)new_mode;

    SIZE_T length = 0;
    const char *source = _acmdln ? _acmdln : "";
    while (source[length])
        length++;

    HANDLE heap = GetProcessHeap();
    char *copy = HeapAlloc(heap, 0, length + 1);
    char **vector = HeapAlloc(heap, 0, sizeof(char *) * (CRT_MAX_ARGS + 1));
    char **environment = HeapAlloc(heap, 0, sizeof(char *));
    if (!copy || !vector || !environment)
        return -1;
    for (SIZE_T i = 0; i <= length; i++)
        copy[i] = source[i];

    int count = crt_split_args(copy, vector, CRT_MAX_ARGS);
    vector[count] = 0;
    environment[0] = 0;

    if (argc) *argc = count;
    if (argv) *argv = vector;
    if (env)  *env = environment;
    return 0;
}

/* operator new's "new handler" hook: no handler is installed, so report that
 * the allocation failure was not retried. */
__declspec(dllexport) int _callnewh(SIZE_T size)
{
    (void)size;
    return 0;
}

/* No DBCS code page is active, so no byte is ever a lead byte. */
__declspec(dllexport) int _ismbblead(unsigned int c)
{
    (void)c;
    return 0;
}

__declspec(dllexport) SIZE_T wcsnlen(const WCHAR *s, SIZE_T max)
{
    SIZE_T n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

__declspec(dllexport) long _wtol(const WCHAR *s)
{
    while (*s == L' ' || *s == L'\t')
        s++;
    int negative = 0;
    if (*s == L'-' || *s == L'+')
        negative = (*s++ == L'-');
    long value = 0;
    while (*s >= L'0' && *s <= L'9')
        value = value * 10 + (*s++ - L'0');
    return negative ? -value : value;
}

/* iswctype for the ASCII range, which is all the classification the inbox
 * utilities need from us today. The masks are the C1_* constants. */
__declspec(dllexport) int iswctype(WCHAR c, unsigned short mask)
{
    unsigned short have = 0;
    if (c < 128) {
        if (c >= L'A' && c <= L'Z') have |= 0x0001 | 0x0100; /* UPPER | ALPHA */
        if (c >= L'a' && c <= L'z') have |= 0x0002 | 0x0100; /* LOWER | ALPHA */
        if (c >= L'0' && c <= L'9') have |= 0x0004;          /* DIGIT */
        if (c == L' ' || (c >= 0x09 && c <= 0x0D)) have |= 0x0008; /* SPACE */
        if (c < 0x20 || c == 0x7F) have |= 0x0020;           /* CNTRL */
        else if (!(have & (0x0001 | 0x0002 | 0x0004)) && c != L' ')
            have |= 0x0010;                                  /* PUNCT */
        if (c > 0x20 && c < 0x7F) have |= 0x0080;            /* not blank */
        if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
            (c >= L'A' && c <= L'F'))
            have |= 0x0040;                                  /* XDIGIT */
    }
    return have & mask;
}

/* std::terminate: the C++ runtime's last resort. Exported under its MSVC
 * mangled name by the link step. */
__declspec(dllexport) void msvcrt_terminate(void)
{
    ExitProcess(3);
}

/* The C++ exception personality (__CxxFrameHandler3) and _CxxThrowException
 * now live in ntdll.dll next to the unwinder they depend on, so programs link
 * them from ntdll.lib. */
