/* Small buffered-state stdio layer over the Win32 file API. */
#ifndef EOF
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
typedef void *QG_HANDLE;
typedef unsigned long QG_DWORD;
typedef unsigned long long QG_SIZE;
typedef int QG_BOOL;
extern QG_HANDLE CreateFileA(const char *, QG_DWORD, QG_DWORD, void *,
                             QG_DWORD, QG_DWORD, QG_HANDLE);
__declspec(dllimport) QG_BOOL ReadFile(QG_HANDLE, void *, QG_DWORD,
                                        QG_DWORD *, void *);
__declspec(dllimport) QG_BOOL WriteFile(QG_HANDLE, const void *, QG_DWORD,
                                         QG_DWORD *, void *);
__declspec(dllimport) QG_BOOL SetFilePointerEx(QG_HANDLE, long long,
                                               long long *, QG_DWORD);
__declspec(dllimport) QG_BOOL CloseHandle(QG_HANDLE);
__declspec(dllimport) QG_HANDLE GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(QG_HANDLE, QG_DWORD, QG_SIZE);
__declspec(dllimport) QG_BOOL HeapFree(QG_HANDLE, QG_DWORD, void *);
__declspec(dllimport) QG_BOOL DeleteFileA(const char *);
extern long long _get_osfhandle(int fd);
struct _QG_FILE {
    unsigned int magic;
    QG_HANDLE handle;
    int can_read, can_write, eof, error, pushed, append;
    unsigned char push_byte;
};
#define QG_FILE_MAGIC 0x51474649u
int qg_is_file(FILE *f) { return f && f->magic == QG_FILE_MAGIC; }

__declspec(dllexport) FILE *_fdopen(int fd, const char *mode)
{
    if (fd < 0 || !mode) return 0;
    FILE *f = (FILE *)HeapAlloc(GetProcessHeap(), 8, sizeof(*f));
    if (!f) return 0;
    f->magic = QG_FILE_MAGIC;
    f->handle = (QG_HANDLE)(QG_SIZE)_get_osfhandle(fd);
    f->can_read = mode[0] == 'r' || mode[1] == '+';
    f->can_write = mode[0] != 'r' || mode[1] == '+';
    f->eof = f->error = f->pushed = f->append = 0;
    f->push_byte = 0;
    return f;
}

static QG_HANDLE qg_heap(void) { return GetProcessHeap(); }

__declspec(dllexport) FILE *fopen(const char *path, const char *mode)
{
    if (!path || !mode || !*mode) return 0;
    int plus = mode[1] == '+';
    int rd = mode[0] == 'r' || plus;
    int wr = mode[0] == 'w' || mode[0] == 'a' || plus;
    if (!rd && !wr) return 0;
    QG_DWORD access = (rd ? 0x80000000u : 0) | (wr ? 0x40000000u : 0);
    QG_DWORD disposition = mode[0] == 'w' ? 2u :
                           mode[0] == 'a' ? 4u : 3u;
    QG_HANDLE h = CreateFileA(path, access, 3, 0, disposition, 0x80, 0);
    if ((QG_SIZE)h == ~(QG_SIZE)0) return 0;
    FILE *f = (FILE *)HeapAlloc(qg_heap(), 8, sizeof(*f));
    if (!f) { CloseHandle(h); return 0; }
    f->magic = QG_FILE_MAGIC; f->handle = h; f->can_read = rd; f->can_write = wr;
    f->eof = f->error = f->pushed = f->append = 0; f->push_byte = 0;
    f->append = mode[0] == 'a';
    if (mode[0] == 'a') SetFilePointerEx(h, 0, 0, 2);
    return f;
}

__declspec(dllexport) int fclose(FILE *f)
{
    if (!qg_is_file(f)) return EOF;
    int ok = CloseHandle(f->handle);
    HeapFree(qg_heap(), 0, f);
    return ok ? 0 : EOF;
}

__declspec(dllexport) size_t fread(void *dst, size_t size, size_t count, FILE *f)
{
    if (!qg_is_file(f) || !dst || !size || !count || !f->can_read) return 0;
    size_t wanted = size * count, done = 0;
    if (wanted / size != count) { f->error = 1; return 0; }
    if (f->pushed) { ((unsigned char *)dst)[done++] = f->push_byte; f->pushed = 0; }
    while (done < wanted) {
        QG_DWORD n = 0, chunk = (QG_DWORD)(wanted - done);
        if (!ReadFile(f->handle, (unsigned char *)dst + done, chunk, &n, 0)) {
            f->error = 1; break;
        }
        done += n;
        if (n < chunk) f->eof = 1;
        if (!n) { f->eof = 1; break; }
    }
    return done / size;
}

__declspec(dllexport) size_t fwrite(const void *src, size_t size, size_t count, FILE *f)
{
    if (!qg_is_file(f) || !src || !size || !count || !f->can_write) return 0;
    size_t wanted = size * count, done = 0;
    if (wanted / size != count) { f->error = 1; return 0; }
    if (f->append && !SetFilePointerEx(f->handle, 0, 0, 2)) { f->error = 1; return 0; }
    while (done < wanted) {
        QG_DWORD n = 0, chunk = (QG_DWORD)(wanted - done);
        if (!WriteFile(f->handle, (const unsigned char *)src + done, chunk, &n, 0) || !n) {
            f->error = 1; break;
        }
        done += n;
    }
    return done / size;
}

__declspec(dllexport) int fseek(FILE *f, long offset, int origin)
{
    if (!qg_is_file(f)) return -1;
    QG_DWORD method = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? 1 :
                      origin == SEEK_END ? 2 : 3;
    if (method == 3) return -1;
    if (f->pushed && origin == SEEK_CUR) offset--;
    f->pushed = f->eof = 0;
    return SetFilePointerEx(f->handle, offset, 0, method) ? 0 : -1;
}

__declspec(dllexport) long ftell(FILE *f)
{
    long long pos;
    if (!qg_is_file(f) || !SetFilePointerEx(f->handle, 0, &pos, 1)) return -1L;
    if (f->pushed) pos--;
    return (long)pos;
}

__declspec(dllexport) int fsetpos(FILE *f, const long long *position)
{
    return position && qg_is_file(f) &&
           SetFilePointerEx(f->handle, *position, 0, SEEK_SET) ? 0 : -1;
}
__declspec(dllexport) long long _filelengthi64(int fd)
{
    QG_HANDLE h = (QG_HANDLE)(QG_SIZE)_get_osfhandle(fd);
    long long saved = 0, end = 0;
    if (!h || !SetFilePointerEx(h, 0, &saved, SEEK_CUR) ||
        !SetFilePointerEx(h, 0, &end, SEEK_END)) return -1;
    SetFilePointerEx(h, saved, 0, SEEK_SET);
    return end;
}

__declspec(dllexport) int fgetc(FILE *f)
{
    unsigned char c;
    if (!qg_is_file(f) || !f->can_read) return EOF;
    if (f->pushed) { f->pushed = 0; return f->push_byte; }
    return fread(&c, 1, 1, f) == 1 ? c : EOF;
}
__declspec(dllexport) int getc(FILE *f) { return fgetc(f); }

__declspec(dllexport) int ungetc(int c, FILE *f)
{
    if (!qg_is_file(f) || c == EOF || f->pushed) return EOF;
    f->push_byte = (unsigned char)c; f->pushed = 1; f->eof = 0;
    return f->push_byte;
}

__declspec(dllexport) int feof(FILE *f) { return qg_is_file(f) ? f->eof : 0; }
__declspec(dllexport) int ferror(FILE *f) { return qg_is_file(f) ? f->error : 0; }
int qg_fflush(FILE *f) { return qg_is_file(f) ? 0 : EOF; }

__declspec(dllexport) char *strncpy(char *dst, const char *src, QG_SIZE n)
{
    QG_SIZE i = 0;
    while (i < n && src[i]) { dst[i] = src[i]; i++; }
    while (i < n) dst[i++] = 0;
    return dst;
}

__declspec(dllexport) char *strstr(const char *s, const char *needle)
{
    if (!*needle) return (char *)s;
    for (; *s; s++) { QG_SIZE i = 0; while (needle[i] && s[i] == needle[i]) i++; if (!needle[i]) return (char *)s; }
    return 0;
}
__declspec(dllexport) char *strcat(char *dst, const char *src)
{
    char *end = dst; while (*end) end++;
    while ((*end++ = *src++)) { }
    return dst;
}

static unsigned int qg_rand_state = 1;
__declspec(dllexport) void srand(unsigned int seed) { qg_rand_state = seed; }
__declspec(dllexport) int rand(void)
{
    qg_rand_state = qg_rand_state * 1103515245u + 12345u;
    return (int)((qg_rand_state >> 1) & 0x7fffffffu);
}
__declspec(dllexport) double atof(const char *s)
{
    int sign = 1; double value = 0.0, scale = 0.1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '+') { if (*s++ == '-') sign = -1; }
    while (*s >= '0' && *s <= '9') value = value * 10.0 + (*s++ - '0');
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { value += (*s++ - '0') * scale; scale *= 0.1; } }
    return sign * value;
}
__declspec(dllexport) int abs(int n) { return n < 0 ? -n : n; }
__declspec(dllexport) double atan(double x)
{
    const double pi_over_2 = 1.57079632679489661923;
    int sign = x < 0.0; if (sign) x = -x;
    double offset = 0.0;
    if (x > 1.0) { x = 1.0 / x; offset = pi_over_2; }
    double x2 = x * x;
    double result = x * (0.9998660 + x2 * (-0.3302995 + x2 *
                    (0.1801410 + x2 * (-0.0851330 + x2 * 0.0208351))));
    result = offset ? offset - result : result;
    return sign ? -result : result;
}
__declspec(dllexport) double atan2(double y, double x)
{
    const double pi = 3.14159265358979323846;
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) return y >= 0.0 ? atan(y / x) + pi : atan(y / x) - pi;
    if (y > 0.0) return pi * 0.5;
    if (y < 0.0) return -pi * 0.5;
    return 0.0;
}
__declspec(dllexport) double fabs(double x) { return x < 0.0 ? -x : x; }
static double qg_reduce_angle(double x)
{
    const double tau = 6.28318530717958647692;
    if (x > tau || x < -tau) x -= (double)(long long)(x / tau) * tau;
    while (x > 3.14159265358979323846) x -= tau;
    while (x < -3.14159265358979323846) x += tau;
    return x;
}
__declspec(dllexport) double sin(double x)
{
    x = qg_reduce_angle(x); double x2 = x * x;
    return x * (1.0 + x2 * (-1.0/6.0 + x2 * (1.0/120.0 +
           x2 * (-1.0/5040.0 + x2 * (1.0/362880.0 + x2 * (-1.0/39916800.0))))));
}
__declspec(dllexport) double cos(double x)
{
    x = qg_reduce_angle(x); double x2 = x * x;
    return 1.0 + x2 * (-1.0/2.0 + x2 * (1.0/24.0 +
           x2 * (-1.0/720.0 + x2 * (1.0/40320.0 + x2 * (-1.0/3628800.0)))));
}
__declspec(dllexport) double tan(double x) { return sin(x) / cos(x); }

static FILE *qg_fds[32];
__declspec(dllexport) int open(const char *path, int flags, ...)
{
    const char *mode = (flags & 2) ? "r+" : (flags & 8) ? "a" :
                       (flags & 0x100) ? ((flags & 0x200) ? "w" : "a") : "r";
    FILE *f = fopen(path, mode);
    if (!f) return -1;
    for (int i = 3; i < 32; i++) if (!qg_fds[i]) { qg_fds[i] = f; return i; }
    fclose(f); return -1;
}
__declspec(dllexport) int close(int fd)
{
    if (fd < 3 || fd >= 32 || !qg_fds[fd]) return -1;
    FILE *f = qg_fds[fd]; qg_fds[fd] = 0; return fclose(f);
}
__declspec(dllexport) int write(int fd, const void *data, unsigned long count)
{
    if (fd < 3 || fd >= 32 || !qg_fds[fd]) return -1;
    return (int)fwrite(data, 1, count, qg_fds[fd]);
}
__declspec(dllexport) int unlink(const char *path) { return DeleteFileA(path) ? 0 : -1; }

static int qg_scan_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
__declspec(dllexport) int fscanf(FILE *f, const char *fmt, ...)
{
    if (!qg_is_file(f) || !fmt) return EOF;
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int assigned = 0;
    while (*fmt) {
        if (qg_scan_space((unsigned char)*fmt)) { while (qg_scan_space((unsigned char)*fmt)) fmt++; int c; do { c = fgetc(f); } while (qg_scan_space(c)); if (c != EOF) ungetc(c, f); continue; }
        if (*fmt != '%') { int c = fgetc(f); if (c != (unsigned char)*fmt++) break; continue; }
        fmt++; int spec = *fmt++;
        int c; do { c = fgetc(f); } while (qg_scan_space(c));
        if (c == EOF) break;
        if (spec == 's') {
            char *out = __builtin_va_arg(ap, char *); int n = 0;
            do { out[n++] = (char)c; c = fgetc(f); } while (c != EOF && !qg_scan_space(c) && n < 1023);
            if (c != EOF) ungetc(c, f); out[n] = 0; assigned++;
        } else if (spec == 'd' || spec == 'i') {
            int sign = 1, value = 0; if (c == '-' || c == '+') { sign = c == '-' ? -1 : 1; c = fgetc(f); }
            if (c < '0' || c > '9') { if (c != EOF) ungetc(c, f); break; }
            do { value = value * 10 + c - '0'; c = fgetc(f); } while (c >= '0' && c <= '9');
            if (c != EOF) ungetc(c, f); *__builtin_va_arg(ap, int *) = sign * value; assigned++;
        } else if (spec == 'f') {
            int sign = 1; double value = 0.0, scale = 0.1; if (c == '-' || c == '+') { sign = c == '-' ? -1 : 1; c = fgetc(f); }
            int digits = 0; while (c >= '0' && c <= '9') { value = value * 10 + c - '0'; digits++; c = fgetc(f); }
            if (c == '.') { c = fgetc(f); while (c >= '0' && c <= '9') { value += (c - '0') * scale; scale *= 0.1; digits++; c = fgetc(f); } }
            if (!digits) { if (c != EOF) ungetc(c, f); break; }
            if (c != EOF) ungetc(c, f); *__builtin_va_arg(ap, float *) = (float)(sign * value); assigned++;
        } else { ungetc(c, f); break; }
    }
    __builtin_va_end(ap);
    return assigned;
}

static void qg_emit(char *out, QG_SIZE cap, QG_SIZE *n, char c)
{
    if (*n + 1 < cap) out[*n] = c;
    (*n)++;
}

int qg_vsnprintf(char *out, QG_SIZE cap, const char *fmt,
                 __builtin_va_list ap)
{
    QG_SIZE n = 0;
    while (*fmt) {
        if (*fmt++ != '%') { qg_emit(out, cap, &n, fmt[-1]); continue; }
        if (*fmt == '%') { fmt++; qg_emit(out, cap, &n, '%'); continue; }
        char pad = ' '; int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int precision = 6;
        if (*fmt == '.') { fmt++; precision = 0; while (*fmt >= '0' && *fmt <= '9') precision = precision * 10 + (*fmt++ - '0'); }
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') fmt++; }
        char spec = *fmt ? *fmt++ : 0;
        char tmp[128]; int used = 0;
        if (spec == 's') {
            const char *s = __builtin_va_arg(ap, const char *); if (!s) s = "(null)";
            while (*s && used < (int)sizeof(tmp)-1) tmp[used++] = *s++;
        } else if (spec == 'c') {
            tmp[used++] = (char)__builtin_va_arg(ap, int);
        } else if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x') {
            long long v = is_long ? __builtin_va_arg(ap, long long) :
                          (spec == 'u' || spec == 'x' ? (long long)__builtin_va_arg(ap, unsigned) : (long long)__builtin_va_arg(ap, int));
            unsigned long long u = (spec == 'u' || spec == 'x') ? (unsigned long long)v :
                                   (v < 0 ? (unsigned long long)(-v) : (unsigned long long)v);
            int neg = (spec != 'u' && spec != 'x' && v < 0), base = spec == 'x' ? 16 : 10;
            do { int d = (int)(u % base); tmp[used++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= base; } while (u);
            if (neg) tmp[used++] = '-';
            for (int i = 0; i < used / 2; i++) { char c = tmp[i]; tmp[i] = tmp[used-1-i]; tmp[used-1-i] = c; }
        } else if (spec == 'f') {
            double d = __builtin_va_arg(ap, double); if (d < 0) { tmp[used++] = '-'; d = -d; }
            unsigned long long whole = (unsigned long long)d; double frac = d - (double)whole;
            char b[32]; int bn = 0; do { b[bn++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
            while (bn) tmp[used++] = b[--bn];
            if (precision > 9) precision = 9;
            if (precision) { tmp[used++] = '.'; for (int i = 0; i < precision; i++) { frac *= 10; int digit = (int)frac; tmp[used++] = (char)('0' + digit); frac -= digit; } }
        } else return -1;
        int padding = width - used;
        if (pad == '0' && used && tmp[0] == '-') { qg_emit(out, cap, &n, '-'); used--; for (int i = 0; i < padding; i++) qg_emit(out, cap, &n, '0'); for (int i = 1; i <= used; i++) qg_emit(out, cap, &n, tmp[i]); }
        else { while (padding-- > 0) qg_emit(out, cap, &n, pad); for (int i = 0; i < used; i++) qg_emit(out, cap, &n, tmp[i]); }
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return n > 0x7fffffff ? 0x7fffffff : (int)n;
}

__declspec(dllexport) int vsprintf(char *out, const char *fmt, __builtin_va_list ap)
{
    return qg_vsnprintf(out, 4096, fmt, ap);
}
__declspec(dllexport) int sprintf(char *out, const char *fmt, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = qg_vsnprintf(out, 4096, fmt, ap); __builtin_va_end(ap); return n;
}
int qg_vfprintf(FILE *f, const char *fmt, __builtin_va_list ap)
{
    if (!qg_is_file(f) || !fmt || !f->can_write) return -1;
    char *buffer = (char *)HeapAlloc(qg_heap(), 8, 4096);
    if (!buffer) return -1;
    int n = qg_vsnprintf(buffer, 4096, fmt, ap);
    int result = n < 0 || (size_t)fwrite(buffer, 1, (size_t)n, f) != (size_t)n ? -1 : n;
    HeapFree(qg_heap(), 0, buffer);
    return result;
}
