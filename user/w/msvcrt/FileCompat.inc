/* CRT file-descriptor and directory-search adapters over the Win32 file API. */
typedef struct {
    DWORD attributes;
    long long created, accessed, written, size;
    char name[260];
} QG_FINDDATA64;

__declspec(dllexport) int _vscprintf(const char *format,
                                     __builtin_va_list arguments)
{
    char buffer[256];
    return qg_vsnprintf(buffer, sizeof(buffer), format, arguments);
}
__declspec(dllexport) int _vsnprintf(char *buffer, SIZE_T capacity,
                                     const char *format,
                                     __builtin_va_list arguments)
{
    return qg_vsnprintf(buffer, capacity, format, arguments);
}

static int qg_wide_path(const unsigned short *wide, char *out, unsigned cap)
{
    unsigned i = 0;
    if (!wide) return 0;
    for (; wide[i] && i + 1 < cap; ++i)
        out[i] = wide[i] < 128 ? (char)wide[i] : '?';
    if (wide[i]) return 0;
    out[i] = 0;
    return 1;
}

__declspec(dllexport) int _commit(int fd) { return fd >= 0 ? 0 : -1; }
__declspec(dllexport) int _close(int fd)
{
    if (fd < 3 || fd >= 64 || !g_crt_fds[fd]) return -1;
    HANDLE h = g_crt_fds[fd];
    g_crt_fds[fd] = 0;
    return CloseHandle(h) ? 0 : -1;
}
__declspec(dllexport) long long _lseeki64(int fd, long long offset, int origin)
{
    return _lseek(fd, offset, origin);
}
__declspec(dllexport) long long _telli64(int fd)
{
    return _lseek(fd, 0, 1);
}
__declspec(dllexport) int _dup(int fd) { return fd >= 0 && fd < 64 ? fd : -1; }
__declspec(dllexport) long long _lseek(int fd, long long offset, int origin)
{
    HANDLE h = (HANDLE)(ULONGLONG)_get_osfhandle(fd);
    long long position = -1;
    if (!h || !SetFilePointerEx(h, offset, &position,
            origin == 0 ? 0 : origin == 1 ? 1 : origin == 2 ? 2 : 3)) return -1;
    return position;
}
__declspec(dllexport) int _read(int fd, void *buffer, unsigned count)
{
    HANDLE h = (HANDLE)(ULONGLONG)_get_osfhandle(fd);
    DWORD read = 0;
    return h && ReadFile(h, buffer, count, &read, 0) ? (int)read : -1;
}

__declspec(dllexport) int _wopen(const unsigned short *path, int flags, ...)
{
    char narrow[260];
    if (!qg_wide_path(path, narrow, sizeof(narrow))) return -1;
    DWORD access = (flags & 3) == 0 ? 0x80000000u :
                   (flags & 3) == 1 ? 0x40000000u : 0xC0000000u;
    DWORD disposition = (flags & 0x0200) ? 2u : (flags & 0x0100) ? 4u : 3u;
    HANDLE h = CreateFileW(path, access, 7, 0, disposition, 0x80, 0);
    if ((ULONGLONG)h == ~(ULONGLONG)0) return -1;
    for (int fd = 3; fd < 64; ++fd) {
        if (!g_crt_fds[fd]) { g_crt_fds[fd] = h; return fd; }
    }
    CloseHandle(h);
    return -1;
}

__declspec(dllexport) int _mkdir(const char *path)
{
    return path && CreateDirectoryA(path, 0) ? 0 : -1;
}

__declspec(dllexport) long long _findfirst64(const char *pattern,
                                               QG_FINDDATA64 *out)
{
    unsigned short wide[260];
    unsigned i = 0;
    if (!pattern || !out) return -1;
    for (; pattern[i] && i < 259; ++i) wide[i] = (unsigned char)pattern[i];
    if (pattern[i]) return -1;
    wide[i] = 0;
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(wide, &data);
    if (!h || (ULONGLONG)h == ~(ULONGLONG)0) return -1;
    memset(out, 0, sizeof(*out));
    out->attributes = data.dwFileAttributes;
    out->size = ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    for (i = 0; data.cFileName[i] && i < sizeof(out->name) - 1; ++i)
        out->name[i] = data.cFileName[i] < 128 ? (char)data.cFileName[i] : '?';
    return (long long)(ULONGLONG)h;
}
__declspec(dllexport) int _findnext64(long long handle, QG_FINDDATA64 *out)
{
    if (!out) return -1;
    WIN32_FIND_DATAW data;
    if (!FindNextFileW((HANDLE)(ULONGLONG)handle, &data)) return -1;
    memset(out, 0, sizeof(*out));
    out->attributes = data.dwFileAttributes;
    out->size = ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    for (unsigned i = 0; data.cFileName[i] && i < sizeof(out->name) - 1; ++i)
        out->name[i] = data.cFileName[i] < 128 ? (char)data.cFileName[i] : '?';
    return 0;
}
__declspec(dllexport) int _findclose(long long handle)
{
    return FindClose((HANDLE)(ULONGLONG)handle) ? 0 : -1;
}

__declspec(dllexport) int _wstat(const unsigned short *path, void *info)
{
    if (!path || !info) return -1;
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == (DWORD)-1) return -1;
    memset(info, 0, 128);
    /* MinGW x64 struct _stat64i32 puts st_mode at byte offset 6. */
    *(unsigned short *)((unsigned char *)info + 6) =
        (attributes & 0x10) ? 0x4000 : 0x8000;
    *(short *)((unsigned char *)info + 8) = 1;
    return 0;
}
__declspec(dllexport) int _fstat64(int fd, void *info)
{
    if (fd < 0 || !info) return -1;
    memset(info, 0, 256);
    long long size = _filelengthi64(fd);
    if (size < 0) return -1;
    /* MinGW x64 struct __stat64 places st_size after mode, inode and device. */
    *(long long *)((unsigned char *)info + 48) = size;
    return 0;
}
__declspec(dllexport) int _wstat64(const unsigned short *path, void *info)
{
    return _wstat(path, info);
}
__declspec(dllexport) int _wutime64(const unsigned short *path, const void *times)
{
    (void)path; (void)times;
    return 0;
}
__declspec(dllexport) int _wchmod(const unsigned short *path, int mode)
{
    (void)path; (void)mode;
    return 0;
}
__declspec(dllexport) long long _wfindfirst64(const unsigned short *pattern,
                                               QG_FINDDATA64 *out)
{
    char narrow[260];
    if (!qg_wide_path(pattern, narrow, sizeof(narrow))) return -1;
    return _findfirst64(narrow, out);
}
__declspec(dllexport) int _wfindnext64(long long handle, QG_FINDDATA64 *out)
{
    return _findnext64(handle, out);
}
__declspec(dllexport) unsigned short *_wgetcwd(unsigned short *out, int size)
{
    if (!out || size < 4) return 0;
    out[0] = 'C'; out[1] = ':'; out[2] = '\\'; out[3] = 0;
    return out;
}
__declspec(dllexport) int _wchdir(const unsigned short *path)
{
    return SetCurrentDirectoryW(path) ? 0 : -1;
}
__declspec(dllexport) int _wmkdir(const unsigned short *path)
{
    char narrow[260];
    return qg_wide_path(path, narrow, sizeof(narrow)) ? _mkdir(narrow) : -1;
}

__declspec(dllexport) int remove(const char *path)
{
    return path && DeleteFileA(path) ? 0 : -1;
}
__declspec(dllexport) int rename(const char *from, const char *to)
{
    return from && to && MoveFileA(from, to) ? 0 : -1;
}

__declspec(dllexport) int isspace(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}
__declspec(dllexport) unsigned short getwc(FILE *stream)
{
    int c = fgetc(stream);
    return c == EOF ? 0xffffu : (unsigned short)(unsigned char)c;
}
__declspec(dllexport) int putc(int value, FILE *stream)
{
    return fputc(value, stream);
}
__declspec(dllexport) int setvbuf(FILE *stream, char *buffer, int mode,
                                  SIZE_T size)
{
    (void)buffer; (void)mode; (void)size;
    return stream ? 0 : EOF;
}
__declspec(dllexport) unsigned short putwc(unsigned short c, FILE *stream)
{
    return fputc(c < 256 ? c : '?', stream) == EOF ? 0xffffu : c;
}
__declspec(dllexport) unsigned short ungetwc(unsigned short c, FILE *stream)
{
    return (unsigned short)ungetc(c < 256 ? c : '?', stream);
}

__declspec(dllexport) void qsort(void *base, SIZE_T count, SIZE_T width,
                                  int (*compare)(const void *, const void *))
{
    unsigned char *bytes = base, temp[256];
    if (!base || !compare || !width || width > sizeof(temp)) return;
    for (SIZE_T i = 1; i < count; ++i) {
        SIZE_T j = i;
        while (j && compare(bytes + (j - 1) * width, bytes + j * width) > 0) {
            memcpy(temp, bytes + (j - 1) * width, width);
            memcpy(bytes + (j - 1) * width, bytes + j * width, width);
            memcpy(bytes + j * width, temp, width);
            --j;
        }
    }
}

__declspec(dllexport) char *_ctime64(const long long *time_value)
{
    static char text[32];
    QG_TM *tm = _gmtime64(time_value);
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", tm);
    return text;
}
