__declspec(dllexport) HICON DuplicateIcon(HANDLE instance, HICON icon)
{
    (void)instance; (void)icon;
    SetLastError(50); return 0; /* Native icon handles have no clonable pixels yet. */
}
__declspec(dllexport) BOOL Shell_NotifyIconW(DWORD message, void *data)
{
    (void)message; (void)data;
    SetLastError(50); return 0; /* No shell notification area. */
}
__declspec(dllexport) BOOL SHTestTokenMembership(HANDLE token, DWORD rid)
{
    (void)token; (void)rid;
    SetLastError(50); return 0; /* No local-group identity provider. */
}
__declspec(dllexport) DWORD SHRestricted(DWORD restriction)
{
    (void)restriction;
    SetLastError(50); return 0; /* No shell policy provider. */
}
__declspec(dllexport) void RunFileDlg(HWND owner, HICON icon, const WCHAR *directory,
    const WCHAR *title, const WCHAR *description, unsigned flags)
{
    (void)owner; (void)icon; (void)directory; (void)title; (void)description; (void)flags;
    SetLastError(50); /* The shell Run dialog is unavailable. */
}

__declspec(dllimport) DWORD GetFileAttributesW(const WCHAR *);
__declspec(dllexport) BOOL PathYetAnotherMakeUniqueName(WCHAR *out,
    const WCHAR *directory, const WCHAR *short_name, const WCHAR *long_name)
{
    if (!out || (!short_name && !long_name)) { SetLastError(87); return 0; }
    const WCHAR *name = long_name ? long_name : short_name;
    WCHAR candidate[MAX_PATH], original[MAX_PATH];
    unsigned prefix = 0, length = 0;
    if (directory) {
        while (directory[prefix] && prefix < MAX_PATH - 1) {
            original[prefix] = directory[prefix]; ++prefix;
        }
        if (directory[prefix]) { SetLastError(206); return 0; }
        if (prefix && original[prefix - 1] != '\\' && original[prefix - 1] != '/') {
            if (prefix >= MAX_PATH - 1) { SetLastError(206); return 0; }
            original[prefix++] = '\\';
        }
    }
    while (name[length] && prefix + length < MAX_PATH - 1) {
        original[prefix + length] = name[length]; ++length;
    }
    if (name[length]) { SetLastError(206); return 0; }
    original[prefix + length] = 0;
    unsigned extension = prefix + length;
    for (unsigned i = prefix; i < prefix + length; ++i)
        if (original[i] == '.') extension = i;
    for (unsigned attempt = 1; attempt < 10000; ++attempt) {
        unsigned n = 0;
        if (attempt == 1) {
            while (original[n]) { candidate[n] = original[n]; ++n; }
        } else {
            for (; n < extension; ++n) candidate[n] = original[n];
            WCHAR digits[5]; unsigned value = attempt, count = 0;
            do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
            unsigned suffix = prefix + length - extension;
            if (n + count + 3 + suffix >= MAX_PATH) { SetLastError(206); return 0; }
            candidate[n++] = ' '; candidate[n++] = '(';
            while (count) candidate[n++] = digits[--count];
            candidate[n++] = ')';
            for (unsigned i = extension; original[i]; ++i) candidate[n++] = original[i];
        }
        candidate[n] = 0;
        if (GetFileAttributesW(candidate) == (DWORD)-1) {
            DWORD error = GetLastError();
            if (error != 2 && error != 3) return 0;
            copy_path(out, candidate); return 1;
        }
    }
    SetLastError(80); return 0;
}
