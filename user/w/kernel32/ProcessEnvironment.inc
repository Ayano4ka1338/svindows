/* Process environment APIs share one live PEB block and synchronize its
 * complete replacement with Ps, so subsequent children inherit the changes. */
extern NTSTATUS SvNtSetProcessEnvironment(const WCHAR *, DWORD);
extern DWORD RtlNtStatusToDosError(NTSTATUS);
__declspec(dllexport) int MultiByteToWideChar(UINT, DWORD, const char *, int, WCHAR *, int);
__declspec(dllexport) int WideCharToMultiByte(UINT, DWORD, const WCHAR *, int, char *, int, const char *, BOOL *);
static volatile LONG g_environment_api_lock;
static WCHAR *g_owned_environment;

static void environment_lock(void)
{
    while (__atomic_exchange_n(&g_environment_api_lock, 1, __ATOMIC_ACQUIRE))
        Sleep(0);
}
static void environment_unlock(void)
{ __atomic_store_n(&g_environment_api_lock, 0, __ATOMIC_RELEASE); }

static const WCHAR *environment_current(void)
{
    static const WCHAR empty[2] = {0,0};
    PEB *peb = NtCurrentPeb();
    BYTE *parameters = peb ? peb->ProcessParameters : 0;
    const WCHAR *block = parameters ? *(WCHAR **)(parameters + 0x80) : 0;
    return block ? block : empty;
}
static SIZE_T environment_units(const WCHAR *block)
{
    if (!*block) return 2;
    const WCHAR *scan = block;
    while (*scan) { while (*scan) scan++; scan++; }
    return (SIZE_T)(scan - block) + 1;
}
static BOOL environment_matches(const WCHAR *entry, const WCHAR *name)
{
    while (*name && *entry && *entry != '=' && nls_fold_w(*entry) == nls_fold_w(*name)) { entry++; name++; }
    return !*name && *entry == '=';
}
static int environment_compare(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a != '=' && *b && *b != '=') {
        WCHAR left = nls_fold_w(*a++), right = nls_fold_w(*b++);
        if (left != right) return left < right ? -1 : 1;
    }
    WCHAR left = *a == '=' ? 0 : nls_fold_w(*a), right = *b == '=' ? 0 : nls_fold_w(*b);
    return left == right ? 0 : left < right ? -1 : 1;
}

__declspec(dllexport) WCHAR *GetEnvironmentStringsW(void)
{
    environment_lock(); const WCHAR *source = environment_current();
    SIZE_T units = environment_units(source);
    WCHAR *copy = HeapAlloc(GetProcessHeap(), 0, units * sizeof(WCHAR));
    if (copy) memcpy(copy, source, units * sizeof(WCHAR));
    environment_unlock(); return copy;
}
__declspec(dllexport) BOOL FreeEnvironmentStringsW(WCHAR *block)
{ return block && HeapFree(GetProcessHeap(), 0, block); }
__declspec(dllexport) char *GetEnvironmentStringsA(void)
{
    WCHAR *wide = GetEnvironmentStringsW(); if (!wide) return 0;
    SIZE_T units = environment_units(wide);
    if (units > 0x7FFFFFFF) { HeapFree(GetProcessHeap(), 0, wide); SetLastError(8); return 0; }
    int bytes = WideCharToMultiByte(0, 0, wide, (int)units, 0, 0, 0, 0);
    char *copy = bytes ? HeapAlloc(GetProcessHeap(), 0, (SIZE_T)bytes) : 0;
    if (copy && !WideCharToMultiByte(0, 0, wide, (int)units, copy, bytes, 0, 0)) {
        HeapFree(GetProcessHeap(), 0, copy); copy = 0;
    }
    HeapFree(GetProcessHeap(), 0, wide); return copy;
}
__declspec(dllexport) BOOL FreeEnvironmentStringsA(char *block)
{ return block && HeapFree(GetProcessHeap(), 0, block); }

__declspec(dllexport) DWORD GetEnvironmentVariableW(const WCHAR *name, WCHAR *out, DWORD size)
{
    if (!name || !*name) { SetLastError(87); return 0; }
    environment_lock(); const WCHAR *value = 0;
    for (const WCHAR *entry = environment_current(); *entry; ) {
        if (environment_matches(entry, name)) { while (*entry != '=') entry++; value = entry + 1; break; }
        while (*entry) entry++; entry++;
    }
    DWORD result = value ? copy_wide_value(value, out, size) : 0;
    environment_unlock(); SetLastError(value ? 0 : 203);
    return result;
}
__declspec(dllexport) DWORD GetEnvironmentVariableA(const char *name, char *out, DWORD size)
{
    if (!name || !*name) { SetLastError(87); return 0; }
    int units = MultiByteToWideChar(0, 0, name, -1, 0, 0);
    WCHAR *wide_name = units ? HeapAlloc(GetProcessHeap(), 0, (SIZE_T)units * 2) : 0;
    if (!wide_name) { SetLastError(8); return 0; }
    MultiByteToWideChar(0, 0, name, -1, wide_name, units);
    environment_lock(); const WCHAR *value = 0;
    for (const WCHAR *entry = environment_current(); *entry; ) {
        if (environment_matches(entry, wide_name)) { while (*entry != '=') entry++; value = entry + 1; break; }
        while (*entry) entry++; entry++;
    }
    DWORD result = 0;
    if (value) {
        int required = WideCharToMultiByte(0, 0, value, -1, 0, 0, 0, 0);
        if (required) {
            if (!out || size < (DWORD)required) result = (DWORD)required;
            else if (WideCharToMultiByte(0, 0, value, -1, out, (int)size, 0, 0)) result = (DWORD)required - 1;
        }
    }
    environment_unlock(); HeapFree(GetProcessHeap(), 0, wide_name);
    SetLastError(value ? 0 : 203); return result;
}

__declspec(dllexport) BOOL SetEnvironmentVariableW(const WCHAR *name, const WCHAR *value)
{
    if (!name || !*name) { SetLastError(87); return 0; }
    SIZE_T name_chars = 0, value_chars = 0;
    for (; name[name_chars]; name_chars++) if (name[name_chars] == '=') { SetLastError(87); return 0; }
    if (value) while (value[value_chars]) value_chars++;
    if (name_chars > 32767 || value_chars > 32767) { SetLastError(206); return 0; }
    environment_lock();
    const WCHAR *source = environment_current(); SIZE_T count = 0, units = 1;
    BOOL found = FALSE;
    for (const WCHAR *entry = source; *entry; ) {
        SIZE_T length = 0; while (entry[length]) length++;
        if (environment_matches(entry, name)) found = TRUE;
        else { count++; units += length + 1; }
        entry += length + 1;
    }
    if (!value && !found) { environment_unlock(); return TRUE; }
    WCHAR *replacement = 0, *new_entry = 0; const WCHAR **entries = 0;
    if (value) { count++; units += name_chars + value_chars + 2; }
    if (!count) units = 2;
    if (units > 0xFFFFFFFFu) { environment_unlock(); SetLastError(8); return FALSE; }
    replacement = HeapAlloc(GetProcessHeap(), 0, units * 2);
    if (count) entries = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*entries));
    if (value) new_entry = HeapAlloc(GetProcessHeap(), 0, (name_chars + value_chars + 2) * 2);
    if (!replacement || (count && !entries) || (value && !new_entry)) {
        if (replacement) HeapFree(GetProcessHeap(), 0, replacement);
        if (entries) HeapFree(GetProcessHeap(), 0, entries);
        if (new_entry) HeapFree(GetProcessHeap(), 0, new_entry);
        environment_unlock(); SetLastError(8); return FALSE;
    }
    SIZE_T used = 0;
    for (const WCHAR *entry = source; *entry; ) {
        if (!environment_matches(entry, name)) entries[used++] = entry;
        while (*entry) entry++; entry++;
    }
    if (value) {
        memcpy(new_entry, name, name_chars * 2); new_entry[name_chars] = '=';
        memcpy(new_entry + name_chars + 1, value, (value_chars + 1) * 2);
        entries[used++] = new_entry;
    }
    for (SIZE_T i = 1; i < count; i++) {
        const WCHAR *entry = entries[i]; SIZE_T position = i;
        while (position && environment_compare(entries[position - 1], entry) > 0) {
            entries[position] = entries[position - 1]; position--;
        }
        entries[position] = entry;
    }
    used = 0;
    for (SIZE_T i = 0; i < count; i++) {
        const WCHAR *entry = entries[i]; do { replacement[used++] = *entry; } while (*entry++);
    }
    replacement[used++] = 0; if (!count) replacement[used] = 0;
    NTSTATUS status = SvNtSetProcessEnvironment(replacement, (DWORD)units);
    if (entries) HeapFree(GetProcessHeap(), 0, entries);
    if (new_entry) HeapFree(GetProcessHeap(), 0, new_entry);
    if (NT_SUCCESS(status)) {
        WCHAR *previous = g_owned_environment; g_owned_environment = replacement;
        if (previous) HeapFree(GetProcessHeap(), 0, previous);
    } else HeapFree(GetProcessHeap(), 0, replacement);
    environment_unlock();
    if (!NT_SUCCESS(status)) { SetLastError(RtlNtStatusToDosError(status)); return FALSE; }
    return TRUE;
}

__declspec(dllexport) BOOL SetEnvironmentVariableA(const char *name, const char *value)
{
    if (!name || !*name) { SetLastError(87); return FALSE; }
    int name_units = MultiByteToWideChar(0, 0, name, -1, 0, 0);
    int value_units = value ? MultiByteToWideChar(0, 0, value, -1, 0, 0) : 0;
    WCHAR *wide_name = name_units ? HeapAlloc(GetProcessHeap(), 0, (SIZE_T)name_units * 2) : 0;
    WCHAR *wide_value = value_units ? HeapAlloc(GetProcessHeap(), 0, (SIZE_T)value_units * 2) : 0;
    if (!wide_name || (value && !wide_value)) {
        if (wide_name) HeapFree(GetProcessHeap(), 0, wide_name);
        if (wide_value) HeapFree(GetProcessHeap(), 0, wide_value);
        SetLastError(8); return FALSE;
    }
    MultiByteToWideChar(0, 0, name, -1, wide_name, name_units);
    if (value) MultiByteToWideChar(0, 0, value, -1, wide_value, value_units);
    BOOL result = SetEnvironmentVariableW(wide_name, wide_value);
    HeapFree(GetProcessHeap(), 0, wide_name);
    if (wide_value) HeapFree(GetProcessHeap(), 0, wide_value);
    return result;
}
