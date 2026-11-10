/* The compatibility runtime is intentionally single-locale and byte-oriented.
 * MinGW's CRT uses these accessors when its headers expand locale macros. */
#ifndef CHAR_MAX
#define CHAR_MAX 127
#endif
__declspec(dllexport) unsigned int ___lc_codepage_func(void)
{
    return 1252;
}

__declspec(dllexport) int ___mb_cur_max_func(void)
{
    return 1;
}

typedef struct {
    char *decimal_point, *thousands_sep, *grouping;
    char *int_curr_symbol, *currency_symbol, *mon_decimal_point;
    char *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space;
    char n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn;
} QG_LCONV;
static QG_LCONV g_lconv = { ".", "", "", "", "", "", "", "", "", "",
                            CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
                            CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX };
__declspec(dllexport) QG_LCONV *localeconv(void)
{
    return &g_lconv;
}

__declspec(dllexport) unsigned long long wcslen(const unsigned short *s)
{
    unsigned long long n = 0;
    while (s[n]) n++;
    return n;
}

__declspec(dllexport) char *strerror(int error)
{
    (void)error;
    return "runtime error";
}

__declspec(dllexport) void *signal(int signal_number, void *handler)
{
    (void)signal_number;
    (void)handler;
    return (void *)1;
}

static void qg_wide_to_narrow(char *out, const unsigned short *in, unsigned cap)
{
    unsigned i = 0;
    if (in) for (; in[i] && i + 1 < cap; ++i)
        out[i] = in[i] < 128 ? (char)in[i] : '?';
    out[i] = 0;
}
__declspec(dllexport) void *_wfopen(const unsigned short *path,
                                    const unsigned short *mode)
{
    char p[260], m[8];
    qg_wide_to_narrow(p, path, sizeof(p));
    qg_wide_to_narrow(m, mode, sizeof(m));
    return fopen(p, m);
}
__declspec(dllexport) void *_wfreopen(const unsigned short *path,
                                      const unsigned short *mode, void *stream)
{
    if (stream) fclose((FILE *)stream);
    return _wfopen(path, mode);
}
__declspec(dllexport) unsigned short *_wfullpath(unsigned short *out,
                                                  const unsigned short *path,
                                                  unsigned long long capacity)
{
    if (!path) return 0;
    unsigned long long n = 0;
    while (path[n] && n + 1 < capacity) { if (out) out[n] = path[n]; n++; }
    if (path[n]) return 0;
    if (out) out[n] = 0;
    return out;
}

__declspec(dllexport) char *setlocale(int category, const char *locale)
{
    (void)category; (void)locale;
    return "C";
}
__declspec(dllexport) WCHAR *_wsetlocale(int category, const WCHAR *locale)
{
    static WCHAR c_locale[] = L"C";
    if (category < 0 || category > 5) return 0;
    if (!locale || !*locale || (locale[0] == 'C' && !locale[1]))
        return c_locale;
    return 0;
}

__declspec(dllexport) int strcoll(const char *left, const char *right)
{
    return strcmp(left, right);
}
__declspec(dllexport) unsigned long long strxfrm(char *out, const char *in,
                                                unsigned long long capacity)
{
    unsigned long long n = strlen(in);
    if (out && capacity) {
        unsigned long long count = n < capacity - 1 ? n : capacity - 1;
        memcpy(out, in, count); out[count] = 0;
    }
    return n;
}
__declspec(dllexport) int towlower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}
__declspec(dllexport) unsigned short *wcscat(unsigned short *dst,
                                              const unsigned short *src)
{
    unsigned long long n = wcslen(dst), i = 0;
    do { dst[n + i] = src[i]; } while (src[i++]);
    return dst;
}
__declspec(dllexport) unsigned short *wcscpy(unsigned short *dst,
                                              const unsigned short *src)
{
    unsigned long long i = 0;
    do { dst[i] = src[i]; } while (src[i++]);
    return dst;
}
__declspec(dllexport) int wcscoll(const unsigned short *left,
                                  const unsigned short *right)
{
    while (*left && *left == *right) { left++; right++; }
    return (int)*left - (int)*right;
}
__declspec(dllexport) unsigned long long wcsxfrm(unsigned short *out,
                                                  const unsigned short *in,
                                                  unsigned long long capacity)
{
    unsigned long long n = wcslen(in), count = n < capacity - (capacity != 0) ? n : capacity - (capacity != 0);
    for (unsigned long long i = 0; out && i < count; ++i) out[i] = in[i];
    if (out && capacity) out[count] = 0;
    return n;
}
__declspec(dllexport) unsigned long long wcsftime(unsigned short *out,
                                                  unsigned long long capacity,
                                                  const unsigned short *format,
                                                  const void *tm)
{
    (void)tm;
    unsigned long long i = 0;
    while (format[i] && i + 1 < capacity) { out[i] = format[i]; i++; }
    if (capacity) out[i] = 0;
    return i;
}
