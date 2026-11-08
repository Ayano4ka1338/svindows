/* PE resources, local memory, FLS, system information and code pages.
 * Included by the owning translation unit. */
/* ------------------------------------------------------------------ */
/* PE resource directory                                              */
/* ------------------------------------------------------------------ */

/* IMAGE_RESOURCE_DIRECTORY: 16 bytes, with the two entry counts at +12 and
 * +14 and the entry array immediately after. Characteristics and TimeDateStamp
 * are DWORDs -- declaring them as WORDs shrinks the header to 12 bytes, which
 * reads the counts out of MajorVersion/MinorVersion and puts the entry array
 * four bytes too early, so every lookup walks garbage. */
typedef struct _RES_DIRECTORY {
    DWORD Characteristics;
    DWORD TimeDateStamp;
    WORD  MajorVersion;
    WORD  MinorVersion;
    WORD  NumberOfNamedEntries;
    WORD  NumberOfIdEntries;
} RES_DIRECTORY;

typedef struct _RES_ENTRY {
    DWORD NameOrId;
    DWORD OffsetToData;
} RES_ENTRY;

typedef struct _RES_DATA_ENTRY {
    DWORD DataRva;
    DWORD Size;
    DWORD CodePage;
    DWORD Reserved;
} RES_DATA_ENTRY;

#define RES_NAME_IS_STRING 0x80000000u
#define RES_SUBDIR         0x80000000u

/* The resource data directory's RVA. A localized binary's .mui companion is a
 * machine-neutral PE32 image, and PE32 puts the data directory at optional
 * header + 96 rather than + 112 -- reading it at the 64-bit offset yields
 * garbage, which is exactly what made every resource lookup in a .mui miss. */
static DWORD res_dir_rva(BYTE *base)
{
    if (!base)
        base = (BYTE *)NtCurrentPeb()->ImageBaseAddress;
    DWORD pe = *(DWORD *)(base + 0x3C);
    BYTE *opt = base + pe + 24;
    WORD magic = *(WORD *)opt;
    DWORD directories = (magic == 0x010B) ? 96 : 112;
    return *(DWORD *)(opt + directories + 2 * 8);
}

static BYTE *res_section(BYTE *base)
{
    if (!base)
        base = (BYTE *)NtCurrentPeb()->ImageBaseAddress;
    DWORD rva = res_dir_rva(base);
    return rva ? base + rva : 0;
}


static const RES_ENTRY *res_find_entry(const RES_DIRECTORY *dir,
                                       const void *id)
{
    const RES_ENTRY *entries = (const RES_ENTRY *)(dir + 1);
    WORD total = (WORD)(dir->NumberOfNamedEntries + dir->NumberOfIdEntries);
    BOOL by_name = ((ULONGLONG)id >> 16) != 0;
    for (WORD i = 0; i < total; i++) {
        if (by_name) {
            if (entries[i].NameOrId & RES_NAME_IS_STRING) {
                const WCHAR *p = (const WCHAR *)((const BYTE *)dir +
                    (entries[i].NameOrId & 0x7FFFFFFFu));
                WORD len = *p++;
                const WCHAR *want = (const WCHAR *)id;
                WORD j = 0;
                while (j < len && want[j] && p[j] == want[j])
                    j++;
                if (j == len && !want[j])
                    return &entries[i];
            }
        } else {
            if (!(entries[i].NameOrId & RES_NAME_IS_STRING) &&
                (WORD)entries[i].NameOrId == (WORD)(ULONGLONG)id)
                return &entries[i];
        }
    }
    return 0;
}

/* A resource tree is normally three levels deep (type / name / language), but
 * it is legal for a level to end in a data entry instead of another directory,
 * and the .mui companions do exactly that: their string blocks sit directly
 * under the name level with no language directory at all. Treating a leaf as a
 * failure made every lookup in a .mui miss. */
static void *res_walk(BYTE *res, const void *type, const void *name,
                      WORD lang)
{
    const RES_ENTRY *e = res_find_entry((const RES_DIRECTORY *)res, type);
    if (!e)
        return 0;
    if (!(e->OffsetToData & RES_SUBDIR))
        return (void *)(res + e->OffsetToData);
    e = res_find_entry((const RES_DIRECTORY *)(res +
                       (e->OffsetToData & 0x7FFFFFFFu)), name);
    if (!e)
        return 0;
    if (!(e->OffsetToData & RES_SUBDIR))
        return (void *)(res + e->OffsetToData);
    const RES_DIRECTORY *lang_dir = (const RES_DIRECTORY *)(res +
                                    (e->OffsetToData & 0x7FFFFFFFu));
    const RES_ENTRY *le = (const RES_ENTRY *)(lang_dir + 1);
    WORD total = (WORD)(lang_dir->NumberOfNamedEntries +
                        lang_dir->NumberOfIdEntries);
    for (WORD i = 0; i < total; i++)
        if ((WORD)le[i].NameOrId == lang)
            return res + le[i].OffsetToData;
    return total ? (void *)(res + le[0].OffsetToData) : 0;
}

/*
 * The localized half of a Windows binary's resources lives in a separate
 * resource-only DLL next to it: notepad.exe carries icons, a manifest and a
 * version block, while every UI string sits in notepad.exe.mui. On Windows the
 * fallback is ntdll's alternate-resource-module machinery; here FindResourceEx
 * does it directly, which is where every caller already funnels through.
 *
 * Without this, LoadStringW finds nothing, and a program that checks it loaded
 * all its strings (notepad demands 49 of them) fails in a way that looks like a
 * clean exit with no error at all.
 */
static BYTE *res_mui_base(BYTE *module)
{
    static BYTE *cached_module;
    static BYTE *cached_mui;
    static BOOL  tried;

    if (!module)
        module = (BYTE *)NtCurrentPeb()->ImageBaseAddress;
    if (tried && module == cached_module)
        return cached_mui;

    char path[260];
    DWORD n = GetModuleFileNameA((HANDLE)module, path, sizeof(path));
    if (!n || n >= sizeof(path) - 5)
        return 0;

    /* Keep the executable's directory: localized resource files sit beside
     * the image, including when the app lives outside the volume root. */
    char name[260];
    DWORD j = 0;
    for (DWORD i = 0; i < n && j < sizeof(name) - 5; i++)
        name[j++] = path[i];
    name[j++] = '.'; name[j++] = 'm'; name[j++] = 'u'; name[j++] = 'i';
    name[j] = 0;

    /* Mapped, not initialized: a resource-only module must never have its entry
     * point called. NtLoadLibrary maps and links the image and stops there,
     * which is what LOAD_LIBRARY_AS_DATAFILE means on Windows. */
    cached_module = module;
    cached_mui = (BYTE *)NtLoadLibrary(name);
    tried = 1;
    return cached_mui;
}

/* Bring-up trace for the resource fallback: which module was asked, whether it
 * had a resource directory of its own, and what its .mui resolved to. */
static void res_trace_find(const void *module, const void *own,
                           const void *mui, const void *found)
{
    extern long NtDisplayString(const char *text);
    static int traced;
    if (traced >= 4)
        return;
    traced++;
    static const char hex[] = "0123456789abcdef";
    char m[128];
    int n = 0;
    const char *labels[4] = { "[k32] FindRes mod=0x", " own=0x", " mui=0x",
                              " found=0x" };
    const void *values[4] = { module, own, mui, found };
    for (int v = 0; v < 4; v++) {
        const char *p = labels[v];
        while (*p) m[n++] = *p++;
        for (int i = 0; i < 12; i++)
            m[n++] = hex[((ULONGLONG)values[v] >> (44 - 4 * i)) & 0xF];
    }
    m[n++] = '\n';
    m[n] = 0;
    NtDisplayString(m);
}

__declspec(dllexport) HANDLE FindResourceExW(HANDLE module, const WCHAR *type,
                                             const WCHAR *name, WORD lang)
{
    BYTE *res = res_section((BYTE *)module);
    void *found = res ? res_walk(res, type, name, lang) : 0;
    BYTE *mui_base = 0;
    if (!found) {
        mui_base = res_mui_base((BYTE *)module);
        BYTE *mui = mui_base ? res_section(mui_base) : 0;
        if (mui)
            found = res_walk(mui, type, name, lang);
    }
    res_trace_find(module, res, mui_base, found);
    if (!found)
        SetLastError(res ? 1814 /* RESOURCE_NAME_NOT_FOUND */
                         : 1815 /* RESOURCE_TYPE_NOT_FOUND */);
    return (HANDLE)found;
}

__declspec(dllexport) HANDLE FindResourceW(HANDLE module, const WCHAR *name,
                                           const WCHAR *type)
{
    return FindResourceExW(module, type, name, 0);
}

/* LoadResource turns the directory entry FindResource returned into the
 * resource bytes. The RVA inside that entry is relative to the image the entry
 * was found in, so decide between the module and its .mui by where the entry
 * itself lives. */
__declspec(dllexport) HANDLE LoadResource(HANDLE module, HANDLE res)
{
    if (!res)
        return 0;
    RES_DATA_ENTRY *entry = (RES_DATA_ENTRY *)res;
    BYTE *base = (BYTE *)(module ? module
                                 : (HANDLE)NtCurrentPeb()->ImageBaseAddress);
    BYTE *mui_base = res_mui_base(base);
    if (mui_base) {
        DWORD rva = res_dir_rva(mui_base);
        BYTE *dir = rva ? mui_base + rva : 0;
        if (dir && (BYTE *)entry >= dir && (BYTE *)entry < dir + 0x400000)
            return (HANDLE)(mui_base + entry->DataRva);
    }
    return (HANDLE)(base + entry->DataRva);
}

__declspec(dllexport) void *LockResource(HANDLE res)
{
    return res;
}

__declspec(dllexport) DWORD SizeofResource(HANDLE module, HANDLE res)
{
    (void)module;
    return res ? ((RES_DATA_ENTRY *)res)->Size : 0;
}

__declspec(dllexport) BOOL FreeResource(HANDLE res)
{
    (void)res;
    return 1;
}

typedef BOOL (*ENUMRESLANGPROCW_K)(HANDLE, const WCHAR *, const WCHAR *,
                                   WORD, LONG_PTR);

__declspec(dllexport) BOOL EnumResourceLanguagesW(HANDLE module,
                                                  const WCHAR *type,
                                                  const WCHAR *name,
                                                  ENUMRESLANGPROCW_K callback,
                                                  LONG_PTR param)
{
    BYTE *res = res_section((BYTE *)module);
    if (!res)
        return 0;
    const RES_ENTRY *t = res_find_entry((const RES_DIRECTORY *)res, type);
    if (!t || !(t->OffsetToData & RES_SUBDIR))
        return 0;
    const RES_ENTRY *n = res_find_entry((const RES_DIRECTORY *)(res +
                        (t->OffsetToData & 0x7FFFFFFFu)), name);
    if (!n || !(n->OffsetToData & RES_SUBDIR))
        return 0;
    const RES_DIRECTORY *ld = (const RES_DIRECTORY *)(res +
                              (n->OffsetToData & 0x7FFFFFFFu));
    const RES_ENTRY *le = (const RES_ENTRY *)(ld + 1);
    WORD total = (WORD)(ld->NumberOfNamedEntries + ld->NumberOfIdEntries);
    for (WORD i = 0; i < total; i++)
        if (!callback(module, type, name, (WORD)le[i].NameOrId, param))
            break;
    return total != 0;
}

/* String resources live in RT_STRING blocks and, for a localized binary, in a
 * separate .mui image; res_load_string handles both. */
static int res_load_string(HANDLE module, UINT id, WCHAR *buf, int cch,
                           WORD lang);

__declspec(dllexport) int LoadStringW(HANDLE instance, UINT id, WCHAR *buf,
                                      int max_chars)
{
    return res_load_string(instance, id, buf, max_chars, 0);
}

__declspec(dllexport) DWORD CheckForReadOnlyResource(HANDLE module, DWORD flag)
{
    (void)module;
    (void)flag;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Global / Local memory                                               */
/* ------------------------------------------------------------------ */

#define GMEM_ZEROINIT 0x0040u
#define LMEM_ZEROINIT 0x0040u

__declspec(dllexport) void *GlobalAlloc(UINT flags, SIZE_T bytes)
{
    if (!bytes)
        bytes = 1;
    void *p = HeapAlloc(GetProcessHeap(),
                        (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, bytes);
    if (!p)
        SetLastError(8);
    return p;
}

__declspec(dllexport) void *GlobalReAlloc(void *h, SIZE_T bytes, UINT flags)
{
    if (!h)
        return 0;
    if (!bytes)
        bytes = 1;
    void *p = HeapReAlloc(GetProcessHeap(),
                          (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0,
                          h, bytes);
    if (!p)
        SetLastError(8);
    return p;
}

__declspec(dllexport) void *GlobalFree(void *h)
{
    if (h)
        HeapFree(GetProcessHeap(), 0, h);
    return 0;
}

__declspec(dllexport) SIZE_T GlobalSize(void *h)
{
    if (!h) return 0;
    return HeapSize(GetProcessHeap(), 0, h);
}

__declspec(dllexport) void *GlobalLock(void *h)
{
    return h;
}

__declspec(dllexport) BOOL GlobalUnlock(void *h)
{
    (void)h;
    return 1;
}

__declspec(dllexport) void *LocalReAlloc(void *h, SIZE_T bytes, UINT flags)
{
    if (!h)
        return 0;
    if (!bytes)
        bytes = 1;
    void *fresh = HeapReAlloc(GetProcessHeap(),
                              (flags & LMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0,
                              h, bytes);
    if (!fresh)
        SetLastError(8);
    return fresh;
}

__declspec(dllexport) SIZE_T LocalSize(void *h)
{
    (void)h;
    return 0;
}

/* ------------------------------------------------------------------ */
/* String comparison and misc helpers                                  */
/* ------------------------------------------------------------------ */

static int wide_cmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)*a - (int)*b;
}

static WCHAR wide_lower(WCHAR c)
{
    return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + 32) : c;
}

static WCHAR wide_upper(WCHAR c)
{
    return (c >= 'a' && c <= 'z') ? (WCHAR)(c - 32) : c;
}

__declspec(dllexport) int lstrcmpW(const WCHAR *a, const WCHAR *b)
{
    return wide_cmp(a, b);
}

__declspec(dllexport) int lstrcmpiW(const WCHAR *a, const WCHAR *b)
{
    for (;;) {
        WCHAR ca = wide_lower(*a++);
        WCHAR cb = wide_lower(*b++);
        if (ca != cb || !ca)
            return (int)ca - (int)cb;
    }
}

__declspec(dllexport) int lstrcmpA(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

__declspec(dllexport) int lstrcmpiA(const char *a, const char *b)
{
    for (;;) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca)
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
    }
}

__declspec(dllexport) int MulDiv(int a, int b, int c)
{
    if (!c)
        return -1;
    long long rounded = c > 0 ? c / 2 : -(c / 2);
    long long r = ((long long)a * b + rounded) / c;
    return (int)r;
}

/* ------------------------------------------------------------------ */
/* Fiber-local storage                                                 */
/* ------------------------------------------------------------------ */

static void *g_fls_slots[128];
static void *g_fls_callbacks[128];
static DWORD g_fls_taken[128];

__declspec(dllexport) DWORD FlsAlloc(void *callback)
{
    for (DWORD i = 1; i < 128; i++) {
        if (__sync_bool_compare_and_swap(&g_fls_taken[i], 0, 1)) {
            g_fls_callbacks[i] = callback;
            g_fls_slots[i] = 0;
            return i;
        }
    }
    SetLastError(87);
    return 0xFFFFFFFFu;
}

__declspec(dllexport) BOOL FlsFree(DWORD index)
{
    if (index >= 128 || !g_fls_taken[index]) {
        SetLastError(87);
        return 0;
    }
    g_fls_taken[index] = 0;
    g_fls_callbacks[index] = 0;
    g_fls_slots[index] = 0;
    return 1;
}

__declspec(dllexport) void *FlsGetValue(DWORD index)
{
    return index < 128 ? g_fls_slots[index] : 0;
}

__declspec(dllexport) BOOL FlsSetValue(DWORD index, void *value)
{
    if (index >= 128 || !g_fls_taken[index])
        return 0;
    g_fls_slots[index] = value;
    return 1;
}

/* ------------------------------------------------------------------ */
/* System information and code pages                                   */
/* ------------------------------------------------------------------ */

typedef struct _SYSTEM_INFO_K {
    DWORD     dwOemId;
    DWORD     dwPageSize;
    void     *lpMinimumApplicationAddress;
    ULONGLONG lpMaximumApplicationAddress;
    ULONGLONG dwActiveProcessorMask;
    DWORD     dwNumberOfProcessors;
    DWORD     dwProcessorType;
    DWORD     dwAllocationGranularity;
    WORD      wProcessorLevel;
    WORD      wProcessorRevision;
} SYSTEM_INFO_K;

__declspec(dllexport) void GetSystemInfo(SYSTEM_INFO_K *info)
{
    if (!info)
        return;
    memset(info, 0, sizeof(*info));
    info->dwOemId = 9; /* PROCESSOR_ARCHITECTURE_AMD64 */
    info->dwPageSize = 4096;
    info->lpMinimumApplicationAddress = (void *)0x10000;
    info->lpMaximumApplicationAddress = 0x7FFFFFFEFFFFULL;
    info->dwActiveProcessorMask = 1;
    info->dwNumberOfProcessors = 1;
    info->dwProcessorType = 586;
    info->dwAllocationGranularity = 65536;
    info->wProcessorLevel = 6;
    info->wProcessorRevision = 1;
}

__declspec(dllexport) void GetNativeSystemInfo(SYSTEM_INFO_K *info)
{
    GetSystemInfo(info);
}

__declspec(dllexport) void GetStartupInfoW(void *info)
{
    if (!info)
        return;
    memset(info, 0, 104);
    *(DWORD *)info = 104;
}

__declspec(dllexport) UINT GetACP(void)
{
    return 1252;
}

__declspec(dllexport) UINT GetOEMCP(void)
{
    return 437;
}

__declspec(dllexport) BOOL IsValidCodePage(UINT code_page)
{
    return code_page == 437 || code_page == 1252 || code_page == 65001;
}

typedef struct _CPINFO_K {
    UINT MaxCharSize;
    BYTE DefaultChar[2];
    BYTE LeadByte[12];
} CPINFO_K;

__declspec(dllexport) BOOL GetCPInfo(UINT code_page, CPINFO_K *out)
{
    if (!out || !IsValidCodePage(code_page)) {
        SetLastError(87);
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->MaxCharSize = code_page == 65001 ? 4 : 1;
    out->DefaultChar[0] = '?';
    return 1;
}

#define LCMAP_LOWERCASE 0x100u
#define LCMAP_UPPERCASE 0x200u

__declspec(dllexport) int LCMapStringW(DWORD locale, DWORD flags,
                                       const WCHAR *src, int src_len,
                                       WCHAR *dst, int dst_len)
{
    (void)locale;
    if (!src || (flags & ~(LCMAP_LOWERCASE | LCMAP_UPPERCASE)))
        return 0;
    int n = src_len < 0 ? lstrlenW(src) : src_len;
    int cap = dst_len < 0 ? n : dst_len;
    int m = n < cap ? n : cap;
    if (dst) {
        for (int i = 0; i < m; i++)
            dst[i] = (flags & LCMAP_UPPERCASE) ? wide_upper(src[i])
                                               : wide_lower(src[i]);
    }
    return m;
}

__declspec(dllexport) BOOL GetStringTypeW(DWORD type, const WCHAR *src,
                                          int len, WORD *out)
{
    if (type != 1 || !src || !out)
        return 0;
    if (len < 0)
        len = lstrlenW(src);
    for (int i = 0; i < len; i++) {
        WCHAR c = src[i];
        WORD f;
        if (c >= 'A' && c <= 'Z') f = 0x0101;
        else if (c >= 'a' && c <= 'z') f = 0x0102;
        else if (c >= '0' && c <= '9') f = 0x0104;
        else if (c == ' ' || (c >= 0x09 && c <= 0x0D)) f = 0x0108;
        else if (c < 0x20 || c == 0x7F) f = 0x0020;
        else f = 0x0010;
        out[i] = f;
    }
    return 1;
}

__declspec(dllexport) BOOL GetStringTypeExW(DWORD locale, DWORD type,
                                            const WCHAR *src, int len,
                                            WORD *out)
{
    (void)locale;
    return GetStringTypeW(type, src, len, out);
}

__declspec(dllexport) void OutputDebugStringA(const char *text)
{
    (void)text;
}

