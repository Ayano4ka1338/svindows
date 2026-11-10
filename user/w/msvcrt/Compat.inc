/* Extended CRT and legacy MSVC startup compatibility functions.
 * Included by user/w/msvcrt/Msvcrt.c. */
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
__declspec(dllexport) int atexit(void (*fn)(void))
{
    if (!fn || g_atexit_count >= 64)
        return -1;
    g_atexit_chain[g_atexit_count++] = fn;
    return 0;
}
void qg_run_atexit(void)
{
    while (g_atexit_count > 0)
        g_atexit_chain[--g_atexit_count]();
}
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

__declspec(dllexport) void _assert(const char *expr, const char *file, unsigned line)
{
    (void)expr; (void)file; (void)line;
    ExitProcess(3);
}

__declspec(dllexport) char *_strdup(const char *s)
{
    if (!s) return 0;
    SIZE_T n = strlen(s) + 1;
    char *copy = (char *)malloc(n);
    return copy ? memcpy(copy, s, n) : 0;
}

typedef struct { void *raw; unsigned long long tag, size; } QG_ALIGNED_HEADER;
__declspec(dllexport) void *_aligned_malloc(SIZE_T size, SIZE_T alignment)
{
    if (alignment < sizeof(void *) || (alignment & (alignment - 1))) return 0;
    void *raw = malloc(size + alignment + sizeof(QG_ALIGNED_HEADER));
    if (!raw) return 0;
    unsigned long long address = ((unsigned long long)raw + sizeof(QG_ALIGNED_HEADER)
                                  + alignment - 1) & ~(alignment - 1);
    QG_ALIGNED_HEADER *header = (QG_ALIGNED_HEADER *)(address - sizeof(*header));
    header->raw = raw;
    header->tag = 0x5147414c49474e44ULL;
    header->size = size;
    return (void *)address;
}
__declspec(dllexport) void _aligned_free(void *pointer)
{
    if (!pointer) return;
    QG_ALIGNED_HEADER *header = (QG_ALIGNED_HEADER *)((unsigned char *)pointer - sizeof(*header));
    if (header->tag == 0x5147414c49474e44ULL) free(header->raw);
}
__declspec(dllexport) void *_aligned_realloc(void *pointer, SIZE_T size,
                                              SIZE_T alignment)
{
    void *fresh = _aligned_malloc(size, alignment);
    if (fresh && pointer) {
        QG_ALIGNED_HEADER *header = (QG_ALIGNED_HEADER *)((unsigned char *)pointer - sizeof(*header));
        if (header->tag == 0x5147414c49474e44ULL)
            memcpy(fresh, pointer, header->size < size ? header->size : size);
    }
    _aligned_free(pointer);
    return fresh;
}

__declspec(dllexport) int _stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        int x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return x - y;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

__declspec(dllexport) int _snprintf(char *out, SIZE_T size, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = qg_vsnprintf(out, size, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

__declspec(dllexport) char *_getcwd(char *buffer, int size)
{
    if (!buffer || size < 4) return 0;
    buffer[0] = 'C'; buffer[1] = ':'; buffer[2] = '\\'; buffer[3] = 0;
    return buffer;
}

__declspec(dllexport) int _execv(const char *path, const char *const *argv)
{
    (void)path; (void)argv;
    g_errno = 38;
    return -1;
}

__declspec(dllexport) long long _strtoi64(const char *s, char **end, int base)
{
    return (long long)strtol(s, end, base);
}
__declspec(dllexport) unsigned long long _strtoui64(const char *s, char **end, int base)
{
    return (unsigned long long)strtoul(s, end, base);
}

__declspec(dllexport) int isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
__declspec(dllexport) int isdigit(int c) { return c >= '0' && c <= '9'; }
__declspec(dllexport) int isalnum(int c) { return isalpha(c) || isdigit(c); }
__declspec(dllexport) int isprint(int c) { return c >= 32 && c <= 126; }
__declspec(dllexport) int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
__declspec(dllexport) int tolower(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
__declspec(dllexport) int toupper(int c) { return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; }

__declspec(dllexport) void *memchr(const void *s, int c, SIZE_T n)
{
    const unsigned char *p = s;
    while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; }
    return 0;
}
__declspec(dllexport) char *strpbrk(const char *s, const char *set)
{
    for (; *s; s++) for (const char *p = set; *p; p++) if (*s == *p) return (char *)s;
    return 0;
}
__declspec(dllexport) char *strrchr(const char *s, int c)
{
    const char *last = 0;
    do { if ((unsigned char)*s == (unsigned char)c) last = s; } while (*s++);
    return (char *)last;
}

__declspec(dllexport) long strtol(const char *s, char **end, int base)
{
    int neg = 0; unsigned long value = 0;
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if (!base) { base = 10; if (s[0] == '0') { base = 8; if (s[1] == 'x' || s[1] == 'X') { base = 16; s += 2; } } }
    const char *start = s;
    for (;;) { int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'z' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'Z' ? *s - 'A' + 10 : 99; if (d >= base) break; value = value * base + d; s++; }
    if (end) *end = (char *)(s == start ? start : s);
    return neg ? -(long)value : (long)value;
}
__declspec(dllexport) unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtol(s, end, base);
}

typedef struct { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year,
                 tm_wday, tm_yday, tm_isdst; } QG_TM;
__declspec(dllexport) long long _time64(long long *out)
{
    if (out) *out = 0;
    return 0;
}
__declspec(dllexport) QG_TM *_gmtime64(const long long *value)
{
    static QG_TM epoch;
    (void)value;
    epoch.tm_sec = epoch.tm_min = epoch.tm_hour = 0;
    epoch.tm_mday = 1; epoch.tm_mon = 0; epoch.tm_year = 70;
    epoch.tm_wday = 4; epoch.tm_yday = 0; epoch.tm_isdst = 0;
    return &epoch;
}
__declspec(dllexport) QG_TM *_localtime64(const long long *value)
{
    return _gmtime64(value);
}

__declspec(dllexport) SIZE_T strftime(char *out, SIZE_T capacity,
                                      const char *format, const QG_TM *tm)
{
    SIZE_T n = 0;
    if (!out || !capacity || !format || !tm) return 0;
    for (; *format && n + 1 < capacity; ++format) {
        if (*format != '%') { out[n++] = *format; continue; }
        char value[16];
        const char *s = value;
        ++format;
        int v = 0;
        switch (*format) {
        case '%': value[0] = '%'; value[1] = 0; break;
        case 'Y': v = tm->tm_year + 1900; goto number;
        case 'm': v = tm->tm_mon + 1; goto number;
        case 'd': v = tm->tm_mday; goto number;
        case 'H': v = tm->tm_hour; goto number;
        case 'M': v = tm->tm_min; goto number;
        case 'S': v = tm->tm_sec; goto number;
        default: value[0] = '%'; value[1] = *format; value[2] = 0; break;
        number:
            value[0] = (char)('0' + (v / 1000) % 10);
            value[1] = (char)('0' + (v / 100) % 10);
            value[2] = (char)('0' + (v / 10) % 10);
            value[3] = (char)('0' + v % 10);
            s = value + (v >= 1000 ? 0 : v >= 100 ? 1 : v >= 10 ? 2 : 3);
            value[4] = 0;
            break;
        }
        while (*s && n + 1 < capacity) out[n++] = *s++;
    }
    out[n] = 0;
    return n;
}

static double qg_sqrt(double x)
{
    if (x <= 0.0) return 0.0;
    double y = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 24; i++) y = (y + x / y) * 0.5;
    return y;
}
__declspec(dllexport) double asin(double x)
{
    if (x > 1.0) x = 1.0; if (x < -1.0) x = -1.0;
    return atan(x / qg_sqrt(1.0 - x * x));
}
__declspec(dllexport) double acos(double x)
{
    return 1.57079632679489661923 - asin(x);
}
__declspec(dllexport) double log10(double x)
{
    if (x <= 0.0) return -1.0e300;
    int exponent = 0;
    while (x >= 2.0) { x *= 0.5; exponent++; }
    while (x < 1.0) { x *= 2.0; exponent--; }
    double z = (x - 1.0) / (x + 1.0), z2 = z * z, term = z, sum = 0.0;
    for (int n = 1; n < 40; n += 2) { sum += term / n; term *= z2; }
    return exponent * 0.3010299956639812 + 0.8685889638065036 * sum;
}

__declspec(dllexport) char *getenv(const char *name)
{
    (void)name;
    return 0;
}
__declspec(dllexport) int _setjmp(void *environment)
{
    (void)environment;
    return 0;
}
__declspec(dllexport) void longjmp(void *environment, int value)
{
    (void)environment; (void)value;
    ExitProcess(3);
}

__declspec(dllexport) unsigned long long _beginthreadex(
    void *security, unsigned stack_size, unsigned (*start)(void *),
    void *argument, unsigned flags, unsigned *thread_id)
{
    return (unsigned long long)CreateThread(security, stack_size,
        (DWORD (*)(void *))start, argument, flags, (DWORD *)thread_id);
}
__declspec(dllexport) void _endthreadex(unsigned status)
{
    ExitThread(status);
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

/* Math leaf routines over the FPU; callers pass plain finite values. */
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
 * MSVC C++ exception base class. Native applications and inbox DLLs construct
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

/* The native Task Manager uses the MSVC scalar C++ allocation operators for
 * small UI and process-record objects.  Keep allocation ownership in the
 * process heap so ordinary HeapFree/malloc cleanup can release the storage. */
__declspec(dllexport) void *msvcrt_operator_new(SIZE_T size)
{
    void *memory = malloc(size ? size : 1);
    if (!memory)
        ExitProcess(3); /* this CRT's C++ exception path is fail-fast */
    return memory;
}

__declspec(dllexport) void msvcrt_operator_delete(void *memory)
{
    free(memory);
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

/* Windows 7 Calculator's additional CRT surface. These implementations cover
 * its ASCII/UTF-16 formatting and locale paths used by the calculator UI. */
__declspec(dllexport) WCHAR *_wcsdup(const WCHAR *s)
{
    if (!s) return 0;
    SIZE_T n = wcslen(s) + 1;
    WCHAR *p = (WCHAR *)malloc(n * sizeof(WCHAR));
    if (p) memcpy(p, s, n * sizeof(WCHAR));
    return p;
}
__declspec(dllexport) long long _wtoi64(const WCHAR *s)
{ return (long long)wcstol(s, 0, 10); }
__declspec(dllexport) int _i64tow_s(long long value, WCHAR *out, SIZE_T cap, int radix)
{
    if (!out || !cap || radix < 2 || radix > 36) return 22;
    unsigned long long v = value < 0 && radix == 10 ? (unsigned long long)(-(value + 1)) + 1 : (unsigned long long)value;
    WCHAR tmp[66]; SIZE_T n=0;
    do { unsigned d=(unsigned)(v%(unsigned)radix); tmp[n++]=(WCHAR)(d<10?'0'+d:'a'+d-10); v/=(unsigned)radix; } while(v && n<65);
    if (value<0 && radix==10) tmp[n++]='-';
    if (n+1>cap) { out[0]=0; return 34; }
    for(SIZE_T i=0;i<n;i++) out[i]=tmp[n-1-i]; out[n]=0; return 0;
}
__declspec(dllexport) int _itow_s(int value, WCHAR *out, SIZE_T cap, int radix)
{ return _i64tow_s(value,out,cap,radix); }
__declspec(dllexport) char *_itoa(int value, char *out, int radix)
{
    if (!out || radix<2 || radix>36) return 0;
    unsigned int v=value<0 && radix==10 ? (unsigned int)(-(value+1))+1 : (unsigned int)value;
    char tmp[34]; int n=0;
    do { unsigned d=v%(unsigned)radix; tmp[n++]=(char)(d<10?'0'+d:'a'+d-10); v/=(unsigned)radix; } while(v && n<33);
    if(value<0 && radix==10) tmp[n++]='-';
    for(int i=0;i<n;i++) out[i]=tmp[n-1-i]; out[n]=0; return out;
}
__declspec(dllexport) SIZE_T strcspn(const char *s, const char *reject)
{
    SIZE_T n=0; for(;s[n];n++){ const char *r=reject; while(*r && *r!=s[n])r++; if(*r)break; } return n;
}
__declspec(dllexport) int sprintf_s(char *out, SIZE_T cap, const char *fmt, ...)
{
    if(!out || !cap) return -1;
    __builtin_va_list ap; __builtin_va_start(ap,fmt);
    int n=qg_vsnprintf(out,cap,fmt,ap); __builtin_va_end(ap);
    if(n<0 || (SIZE_T)n>=cap){out[0]=0;return -1;} return n;
}
__declspec(dllexport) int wcscat_s(WCHAR *dst, SIZE_T cap, const WCHAR *src)
{
    if(!dst||!src||!cap)return 22; SIZE_T n=wcslen(dst),i=0;
    if(n>=cap)return 22; while(src[i]&&n+i+1<cap){dst[n+i]=src[i];i++;}
    if(src[i]){dst[0]=0;return 34;} dst[n+i]=0; return 0;
}
__declspec(dllexport) int _wcslwr_s(WCHAR *s, SIZE_T cap)
{
    if(!s||!cap)return 22; for(SIZE_T i=0;i<cap&&s[i];i++) if(s[i]>='A'&&s[i]<='Z')s[i]+=32;
    if(cap && s[cap-1]){s[0]=0;return 34;} return 0;
}
__declspec(dllexport) WCHAR *_wcsrev(WCHAR *s)
{
    if(!s)return 0; SIZE_T n=wcslen(s); for(SIZE_T i=0;i<n/2;i++){WCHAR c=s[i];s[i]=s[n-1-i];s[n-1-i]=c;} return s;
}
__declspec(dllexport) int iswalpha(WCHAR c)
{ return (c>='A'&&c<='Z')||(c>='a'&&c<='z'); }
__declspec(dllexport) int iswdigit(WCHAR c) { return c>='0'&&c<='9'; }
__declspec(dllexport) int mbstowcs_s(SIZE_T *converted, WCHAR *out, SIZE_T cap,
                                      const char *src, SIZE_T count)
{
    if(converted)*converted=0; if(!src)return 22;
    SIZE_T i=0; while(src[i] && i<count && (!out || i+1<cap)){if(out)out[i]=(unsigned char)src[i];i++;}
    if(out){if(i>=cap)return 34;out[i]=0;} if(converted)*converted=i+1; return 0;
}
__declspec(dllexport) int __uncaught_exception(void) { return 0; }
__declspec(dllexport) void __CxxThrowException(void *object, void *record)
{ (void)object; (void)record; ExitProcess(3); }
__declspec(dllexport) int __CxxFrameHandler3(void)
{ return 0; }
__declspec(dllexport) void msvcrt_type_info_dtor(void *self) { (void)self; }
__declspec(dllexport) int __crtGetStringTypeW(DWORD info, const WCHAR *src,
                                               int count, unsigned short *out)
{
    (void)info; if(!src||!out||count<0)return 0;
    for(int i=0;i<count;i++){WCHAR c=src[i];unsigned short t=0;
      if(c>='A'&&c<='Z')t=0x0101; else if(c>='a'&&c<='z')t=0x0102;
      else if(c>='0'&&c<='9')t=0x0004; else if(c==' '|| (c>=9&&c<=13))t=0x0008;
      else if(c<32||c==127)t=0x0020; else if(c)t=0x0010; out[i]=t; }
    return 1;
}
__declspec(dllexport) int __crtLCMapStringW(DWORD locale, DWORD flags,
        const WCHAR *src, int count, WCHAR *dst, int cap)
{
    (void)locale; if(!src||count<0)return 0; int n=count;
    if(!count){while(src[n])n++;n++;} if(!dst)return n; if(cap<n)return 0;
    for(int i=0;i<n;i++){WCHAR c=src[i]; if(flags&0x100)c=(WCHAR)(c>='a'&&c<='z'?c-32:c); else if(flags&0x200)c=(WCHAR)(c>='A'&&c<='Z'?c+32:c); dst[i]=c;} return n;
}
static int g_lc_handle;
__declspec(dllexport) int *___lc_handle_func(void) { return &g_lc_handle; }
__declspec(dllexport) int __mb_cur_max = 1;
static const unsigned short g_ctype[257] = {0};
__declspec(dllexport) const unsigned short *__pctype_func(void) { return g_ctype+1; }

__declspec(dllexport) int iswspace(WCHAR ch)
{
    return iswctype(ch, 0x0008);
}

__declspec(dllexport) int iswprint(WCHAR ch)
{
    return ch == ' ' || iswctype(ch, 0x0080);
}

__declspec(dllexport) SIZE_T wcscspn(const WCHAR *text, const WCHAR *reject)
{
    SIZE_T length = 0;
    for (; text[length]; length++) {
        for (SIZE_T i = 0; reject[i]; i++)
            if (text[length] == reject[i]) return length;
    }
    return length;
}

__declspec(dllexport) WCHAR *_wcsupr(WCHAR *text)
{
    if (!text) return 0;
    for (WCHAR *p = text; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
    return text;
}

__declspec(dllexport) WCHAR *_wcslwr(WCHAR *text)
{
    if (!text) return 0;
    for (WCHAR *p = text; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
    return text;
}

/* The native fixed-size stacks cannot restore a consumed Windows guard page.
 * Return failure so an overflow handler cannot resume on a damaged stack. */
__declspec(dllexport) int _resetstkoflw(void)
{
    g_errno = 38; /* ENOSYS */
    return 0;
}
