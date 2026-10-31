/* Host-side behavioral check for the Quake FILE layer with an in-memory Win32
 * file API. It exercises the same implementation compiled into msvcrt.dll. */
#include <stdlib.h>
#include <stdint.h>

typedef struct { unsigned char bytes[256]; unsigned long size; long long pos; } MOCK_FILE;
static MOCK_FILE mock_file;
long long _get_osfhandle(int fd) { (void)fd; return (long long)(uintptr_t)&mock_file; }

typedef struct _QG_FILE FILE;
#define __declspec(x)
#define fopen qg_fopen
#define fclose qg_fclose
#define fread qg_fread
#define fwrite qg_fwrite
#define fseek qg_fseek
#define ftell qg_ftell
#define fgetc qg_fgetc
#define getc qg_getc
#define ungetc qg_ungetc
#define feof qg_feof
#define ferror qg_ferror
#define fflush qg_fflush_export
#define fprintf qg_fprintf
#define vfprintf qg_vfprintf_export
#define fscanf qg_fscanf
#define sprintf qg_sprintf
#define vsprintf qg_vsprintf
#include "../../user/w/msvcrt/FileStream.inc"
#undef __declspec

QG_HANDLE GetProcessHeap(void) { return (void *)1; }
void *HeapAlloc(QG_HANDLE heap, QG_DWORD flags, QG_SIZE size)
{ (void)heap; (void)flags; return malloc((size_t)size); }
QG_BOOL HeapFree(QG_HANDLE heap, QG_DWORD flags, void *p)
{ (void)heap; (void)flags; free(p); return 1; }
QG_HANDLE CreateFileA(const char *path, QG_DWORD access, QG_DWORD share,
                      void *sa, QG_DWORD disposition, QG_DWORD flags,
                      QG_HANDLE templ)
{
    (void)path; (void)access; (void)share; (void)sa; (void)flags; (void)templ;
    if (disposition == 2) mock_file.size = 0;
    mock_file.pos = 0;
    return &mock_file;
}
QG_BOOL ReadFile(QG_HANDLE h, void *buf, QG_DWORD n, QG_DWORD *done, void *ov)
{
    (void)ov; MOCK_FILE *f = h; QG_DWORD left = f->pos < f->size ? f->size - (QG_DWORD)f->pos : 0;
    *done = n < left ? n : left;
    for (QG_DWORD i = 0; i < *done; i++) ((unsigned char *)buf)[i] = f->bytes[f->pos++];
    return 1;
}
QG_BOOL WriteFile(QG_HANDLE h, const void *buf, QG_DWORD n, QG_DWORD *done, void *ov)
{
    (void)ov; MOCK_FILE *f = h; *done = n;
    if (f->pos + n > sizeof(f->bytes)) return 0;
    for (QG_DWORD i = 0; i < n; i++) f->bytes[f->pos++] = ((const unsigned char *)buf)[i];
    if ((QG_DWORD)f->pos > f->size) f->size = (QG_DWORD)f->pos;
    return 1;
}
QG_BOOL SetFilePointerEx(QG_HANDLE h, long long d, long long *out, QG_DWORD method)
{
    MOCK_FILE *f = h; long long base = method == 0 ? 0 : method == 1 ? f->pos : f->size;
    f->pos = base + d; if (out) *out = f->pos; return f->pos >= 0;
}
QG_BOOL CloseHandle(QG_HANDLE h) { return h == &mock_file; }
QG_BOOL DeleteFileA(const char *p) { (void)p; mock_file.size = 0; return 1; }

static int test_fprintf(FILE *f, const char *fmt, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = qg_vfprintf(f, fmt, ap); __builtin_va_end(ap); return n;
}

int main(void)
{
    FILE *f = fopen("save.dat", "wb");
    if (!f) return 1;
    const char payload[] = "quake-stream";
    if (fwrite(payload, 2, 6, f) != 6 || ftell(f) != 12 || fclose(f)) return 2;
    f = fopen("save.dat", "rb");
    if (!f) return 3;
    if (!f->can_read) return 30;
    if (mock_file.size != 12) return 32 + (int)mock_file.size;
    if (mock_file.pos != 0) return 80 + (int)mock_file.pos;
    char data[16] = {0};
    size_t got = fread(data, 3, 4, f);
    if (got != 4) return 40 + (int)got;
    if (data[0] != 'q') return 41;
    if (data[11] != 'm') return 42;
    if (feof(f)) return 43;
    if (fgetc(f) != EOF || !feof(f)) return 5;
    if (fseek(f, 0, SEEK_SET) || feof(f) || fgetc(f) != 'q') return 6;
    if (ungetc('X', f) != 'X' || fgetc(f) != 'X' || fgetc(f) != 'u') return 7;
    if (fseek(f, -2, SEEK_END) || fgetc(f) != 'a' || fgetc(f) != 'm') return 8;
    if (fclose(f)) return 9;

    f = fopen("settings.cfg", "wb");
    if (!f) return 10;
    if (test_fprintf(f, "%i %s %.2f\n", 42, "config", 1.25) <= 0 || fclose(f)) return 11;
    f = fopen("settings.cfg", "rb");
    if (!f) return 12;
    int number = 0; float value = 0; char word[32];
    if (fscanf(f, "%i %s %f", &number, word, &value) != 3 ||
        number != 42 || value < 1.24f || value > 1.26f || word[0] != 'c') return 13;
    if (fclose(f)) return 14;
    return 0;
}
