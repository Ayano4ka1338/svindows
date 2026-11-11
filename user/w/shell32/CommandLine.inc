__declspec(dllimport) void *LocalAlloc(DWORD, unsigned long long);
__declspec(dllimport) void *LocalFree(void *);
__declspec(dllimport) DWORD GetModuleFileNameW(HANDLE, WCHAR *, DWORD);

__declspec(dllexport) WCHAR **CommandLineToArgvW(const WCHAR *line, int *count)
{
    if (!line || !count) { SetLastError(87); return 0; }
    *count = 0;
    unsigned length = 0;
    while (line[length] && length < 32768) ++length;
    if (length == 32768) { SetLastError(87); return 0; }
    unsigned pointers = length ? length + 2 : 2;
    unsigned chars = length ? length + 2 : 32768;
    WCHAR **vector = LocalAlloc(0, (unsigned long long)pointers * sizeof(*vector) + chars * sizeof(WCHAR));
    if (!vector) { SetLastError(8); return 0; }
    WCHAR *out = (WCHAR *)(vector + pointers);
    if (!length) {
        DWORD copied = GetModuleFileNameW(0, out, chars);
        if (!copied || copied >= chars) { LocalFree(vector); return 0; }
        vector[0] = out; vector[1] = 0; *count = 1;
        return vector;
    }
    const WCHAR *read = line;
    int argc = 0;
    if (*read == ' ' || *read == '\t') { vector[argc++] = out; *out++ = 0; }
    while (*read) {
        while (*read == ' ' || *read == '\t') ++read;
        if (!*read) break;
        vector[argc++] = out;
        int quoted = 0;
        while (*read && (quoted || (*read != ' ' && *read != '\t'))) {
            unsigned slashes = 0;
            while (*read == '\\') { ++slashes; ++read; }
            if (*read == '"') {
                for (unsigned i = 0; i < slashes / 2; ++i) *out++ = '\\';
                if (slashes & 1) { *out++ = '"'; ++read; }
                else if (quoted && read[1] == '"') { *out++ = '"'; read += 2; }
                else { quoted = !quoted; ++read; }
            } else {
                for (unsigned i = 0; i < slashes; ++i) *out++ = '\\';
                if (*read && (quoted || (*read != ' ' && *read != '\t')))
                    *out++ = *read++;
            }
        }
        *out++ = 0;
    }
    vector[argc] = 0;
    *count = argc;
    return vector;
}
