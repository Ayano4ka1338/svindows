/* COM bootstrap, version APIs and USER class support.
 * Included by the owning translation unit. */
/* ------------------------------------------------------------------ */
/* COM bootstrap (COM API-set contracts are currently aliased here)    */
/* ------------------------------------------------------------------ */

/* TEB.ReservedForOle on x64. Genuine OLE32 expects CoInitializeEx to
 * establish a per-apartment block here before OleInitialize increments its
 * apartment/OLE counters. The full combase apartment object will replace this
 * bootstrap allocation as more COM services move out of the kernel32 host. */
#define TEB_RESERVED_FOR_OLE_OFFSET 0x1758

__declspec(dllexport) LONG CoInitializeEx(void *reserved, DWORD flags)
{
    (void)reserved;
    (void)flags;
    void *ole = (void *)(ULONGLONG)
        read_gs_qword(TEB_RESERVED_FOR_OLE_OFFSET);
    if (ole)
        return 1; /* S_FALSE: apartment was already initialized */

    ole = nt_alloc(0x1000);
    if (!ole)
        return (LONG)0x8007000e; /* E_OUTOFMEMORY */
    memset(ole, 0, 0x1000);
    write_gs_qword(TEB_RESERVED_FOR_OLE_OFFSET, (ULONGLONG)ole);
    return 0; /* S_OK */
}

__declspec(dllexport) LONG CoInitialize(void *reserved)
{
    return CoInitializeEx(reserved, 2); /* COINIT_APARTMENTTHREADED */
}

__declspec(dllexport) void CoUninitialize(void)
{
    /* Keep the apartment block alive for the lifetime of this TEB. Native COM
     * also retains substantial per-thread state until thread teardown. */
}

__declspec(dllexport) void *CoTaskMemAlloc(SIZE_T bytes)
{
    return HeapAlloc(GetProcessHeap(), 0, bytes);
}

__declspec(dllexport) void *CoTaskMemRealloc(void *memory, SIZE_T bytes)
{
    if (!memory)
        return CoTaskMemAlloc(bytes);
    return HeapReAlloc(GetProcessHeap(), 0, memory, bytes);
}

__declspec(dllexport) void CoTaskMemFree(void *memory)
{
    if (memory)
        HeapFree(GetProcessHeap(), 0, memory);
}

__declspec(dllexport) LONG CreateBindCtx(DWORD reserved, void **bind_context);

typedef struct _GUID_K {
    DWORD Data1;
    WORD Data2;
    WORD Data3;
    BYTE Data4[8];
} GUID_K;

static WCHAR guid_hex(unsigned int value)
{
    return (WCHAR)(value < 10 ? '0' + value : 'A' + value - 10);
}

__declspec(dllexport) int StringFromGUID2(const GUID_K *guid, WCHAR *out,
                                          int capacity)
{
    if (!guid || !out || capacity < 39)
        return 0;
    int p = 0;
    out[p++] = '{';
#define PUT_HEX(value, digits) do { \
        unsigned long long _v = (unsigned long long)(value); \
        for (int _i = (digits) - 1; _i >= 0; _i--) \
            out[p++] = guid_hex((unsigned int)((_v >> (_i * 4)) & 0xf)); \
    } while (0)
    PUT_HEX(guid->Data1, 8); out[p++] = '-';
    PUT_HEX(guid->Data2, 4); out[p++] = '-';
    PUT_HEX(guid->Data3, 4); out[p++] = '-';
    PUT_HEX(guid->Data4[0], 2); PUT_HEX(guid->Data4[1], 2); out[p++] = '-';
    for (int i = 2; i < 8; i++) PUT_HEX(guid->Data4[i], 2);
    out[p++] = '}';
    out[p] = 0;
#undef PUT_HEX
    return p + 1;
}

static const GUID_K g_iid_iunknown = {
    0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x46 }
};
static const GUID_K g_iid_ibindctx = {
    0x0000000E, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x46 }
};

static BOOL guid_equal(const GUID_K *a, const GUID_K *b)
{
    if (!a || !b || a->Data1 != b->Data1 || a->Data2 != b->Data2 ||
        a->Data3 != b->Data3)
        return 0;
    for (int i = 0; i < 8; i++)
        if (a->Data4[i] != b->Data4[i])
            return 0;
    return 1;
}

typedef struct _BIND_CONTEXT BIND_CONTEXT;
typedef struct _BIND_CONTEXT_VTBL {
    LONG  (*QueryInterface)(BIND_CONTEXT *, const GUID_K *, void **);
    DWORD (*AddRef)(BIND_CONTEXT *);
    DWORD (*Release)(BIND_CONTEXT *);
    LONG  (*RegisterObjectBound)(BIND_CONTEXT *, void *);
    LONG  (*RevokeObjectBound)(BIND_CONTEXT *, void *);
    LONG  (*ReleaseBoundObjects)(BIND_CONTEXT *);
    LONG  (*SetBindOptions)(BIND_CONTEXT *, void *);
    LONG  (*GetBindOptions)(BIND_CONTEXT *, void *);
    LONG  (*GetRunningObjectTable)(BIND_CONTEXT *, void **);
    LONG  (*RegisterObjectParam)(BIND_CONTEXT *, WCHAR *, void *);
    LONG  (*GetObjectParam)(BIND_CONTEXT *, WCHAR *, void **);
    LONG  (*EnumObjectParam)(BIND_CONTEXT *, void **);
    LONG  (*RevokeObjectParam)(BIND_CONTEXT *, WCHAR *);
} BIND_CONTEXT_VTBL;

struct _BIND_CONTEXT {
    const BIND_CONTEXT_VTBL *Vtbl;
    volatile LONG References;
};

static LONG bind_context_query_interface(BIND_CONTEXT *context,
                                          const GUID_K *iid, void **object)
{
    if (!object)
        return (LONG)0x80070057L;
    *object = 0;
    if (!guid_equal(iid, &g_iid_iunknown) &&
        !guid_equal(iid, &g_iid_ibindctx))
        return (LONG)0x80004002L;
    __sync_add_and_fetch(&context->References, 1);
    *object = context;
    return 0;
}

static DWORD bind_context_add_ref(BIND_CONTEXT *context)
{
    return (DWORD)__sync_add_and_fetch(&context->References, 1);
}

static DWORD bind_context_release(BIND_CONTEXT *context)
{
    LONG references = __sync_sub_and_fetch(&context->References, 1);
    if (references < 1) {
        context->References = 1;
        references = 1;
    }
    return (DWORD)references;
}

static LONG bind_context_not_implemented(BIND_CONTEXT *context)
{
    (void)context;
    return (LONG)0x80004001L;
}

static const BIND_CONTEXT_VTBL g_bind_context_vtbl = {
    bind_context_query_interface,
    bind_context_add_ref,
    bind_context_release,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented
};
static BIND_CONTEXT g_bind_context = { &g_bind_context_vtbl, 1 };

__declspec(dllexport) LONG CreateBindCtx(DWORD reserved, void **bind_context)
{
    if (reserved || !bind_context)
        return (LONG)0x80070057L;
    *bind_context = 0;
    bind_context_add_ref(&g_bind_context);
    *bind_context = &g_bind_context;
    return 0;
}

__declspec(dllexport) LONG CoCreateInstance(const void *class_id,
                                             void *outer, DWORD context,
                                             const void *interface_id,
                                             void **object)
{
    (void)class_id; (void)outer; (void)context; (void)interface_id;
    if (object)
        *object = 0;
    return (LONG)0x80040154L; /* REGDB_E_CLASSNOTREG */
}

/* ------------------------------------------------------------------ */
/* Minimal VERSION.dll surface (the loader aliases VERSION to us)      */
/* ------------------------------------------------------------------ */

static BOOL wide_contains_ascii(const WCHAR *wide, const char *ascii)
{
    if (!wide || !ascii)
        return 0;
    for (; *wide; wide++) {
        const WCHAR *w = wide;
        const char *a = ascii;
        while (*a && *w == (WCHAR)(BYTE)*a) {
            w++;
            a++;
        }
        if (!*a)
            return 1;
    }
    return 0;
}

__declspec(dllexport) DWORD GetFileVersionInfoSizeExW(DWORD flags,
                                                       const WCHAR *filename,
                                                       DWORD *handle)
{
    (void)flags; (void)filename;
    if (handle)
        *handle = 0;
    return 64;
}

__declspec(dllexport) BOOL GetFileVersionInfoExW(DWORD flags,
                                                 const WCHAR *filename,
                                                 DWORD handle, DWORD length,
                                                 void *data)
{
    (void)flags; (void)filename; (void)handle;
    if (!data || length < 4) {
        SetLastError(122);
        return 0;
    }
    memset(data, 0, length);
    return 1;
}

__declspec(dllexport) BOOL VerQueryValueW(const void *block,
                                          const WCHAR *subblock,
                                          void **value, unsigned *length)
{
    static DWORD translation = 0x04B00409; /* en-US, Unicode code page */
    static WCHAR internal_name[] = {
        'w','h','e','r','e','.','e','x','e',0
    };
    (void)block;
    if (!subblock || !value || !length)
        return 0;
    if (wide_contains_ascii(subblock, "Translation")) {
        *value = &translation;
        *length = sizeof(translation);
        return 1;
    }
    if (wide_contains_ascii(subblock, "InternalName")) {
        *value = internal_name;
        *length = sizeof(internal_name) / sizeof(internal_name[0]);
        return 1;
    }
    *value = 0;
    *length = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Module file name (from PEB->ProcessParameters ImagePathName)        */
/* ------------------------------------------------------------------ */

__declspec(dllexport) DWORD GetModuleFileNameA(HANDLE module, char *buf,
                                               DWORD size)
{
    (void)module;
    PEB *peb = NtCurrentPeb();
    unsigned char *pp = (unsigned char *)peb->ProcessParameters;
    DWORD n = 0;
    if (pp && buf && size) {
        WORD   len = *(WORD *)(pp + 0x60) / 2;  /* ImagePathName.Length (chars) */
        WCHAR *w   = *(WCHAR **)(pp + 0x68);     /* ImagePathName.Buffer          */
        for (; n < len && n < size - 1 && w[n]; n++)
            buf[n] = (char)w[n];
    }
    if (buf && size)
        buf[n] = 0;
    return n;
}

__declspec(dllexport) DWORD GetModuleFileNameW(HANDLE module, WCHAR *buf,
                                               DWORD size)
{
    (void)module;
    PEB *peb = NtCurrentPeb();
    unsigned char *pp = (unsigned char *)peb->ProcessParameters;
    if (!pp || !buf || !size)
        return 0;

    DWORD len = *(WORD *)(pp + 0x60) / 2;
    WCHAR *src = *(WCHAR **)(pp + 0x68);
    DWORD n = 0;
    while (src && n < len && src[n] && n + 1 < size) {
        buf[n] = src[n];
        n++;
    }
    buf[n] = 0;
    if (n < len) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return size;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Minimal USER32 window-class registry                                */
/* ------------------------------------------------------------------ */

typedef WORD ATOM;
typedef long long LRESULT;
typedef LRESULT (*WNDPROC)(HANDLE window, DWORD message,
                          ULONGLONG wparam, long long lparam);

typedef struct _WNDCLASSEXW {
    DWORD   cbSize;
    DWORD   style;
    WNDPROC lpfnWndProc;
    int     cbClsExtra;
    int     cbWndExtra;
    HANDLE  hInstance;
    HANDLE  hIcon;
    HANDLE  hCursor;
    HANDLE  hbrBackground;
    const WCHAR *lpszMenuName;
    const WCHAR *lpszClassName;
    HANDLE  hIconSm;
} WNDCLASSEXW;

#define USER_MAX_CLASSES 32
#define USER_CLASS_NAME_MAX 128

typedef struct _USER_CLASS {
    BOOL    Used;
    ATOM    Atom;
    DWORD   Style;
    WNDPROC WindowProc;
    int     WindowExtra;
    HANDLE  Instance;
    WCHAR   Name[USER_CLASS_NAME_MAX];
} USER_CLASS;

static USER_CLASS g_user_classes[USER_MAX_CLASSES];

#define USER_MAX_WINDOWS 64
typedef struct _USER_WINDOW {
    BOOL       Used;
    USER_CLASS *Class;
    DWORD      ExStyle;
    DWORD      Style;
    int        X;
    int        Y;
    int        Width;
    int        Height;
    HANDLE     Parent;
    HANDLE     Menu;
    HANDLE     Instance;
    void       *CreateParameter;
    DWORD      Band;
} USER_WINDOW;

static USER_WINDOW g_user_windows[USER_MAX_WINDOWS];
static HANDLE g_user_message_event;

typedef struct _USER_CURSOR {
    DWORD ResourceId;
} USER_CURSOR;

static USER_CURSOR g_user_system_cursors[] = {
    { 32512 }, /* IDC_ARROW */
    { 32513 }, /* IDC_IBEAM */
    { 32514 }, /* IDC_WAIT */
    { 32515 }, /* IDC_CROSS */
    { 32516 }, /* IDC_UPARROW */
    { 32642 }, /* IDC_SIZENWSE */
    { 32643 }, /* IDC_SIZENESW */
    { 32644 }, /* IDC_SIZEWE */
    { 32645 }, /* IDC_SIZENS */
    { 32646 }, /* IDC_SIZEALL */
    { 32648 }, /* IDC_NO */
    { 32649 }, /* IDC_HAND */
    { 32650 }, /* IDC_APPSTARTING */
    { 32651 }  /* IDC_HELP */
};

static BOOL user_class_name_equal(const WCHAR *a, const WCHAR *b)
{
    if (!a || !b)
        return 0;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

__declspec(dllexport) ATOM RegisterClassExW(const WNDCLASSEXW *window_class)
{
    if (!window_class || window_class->cbSize < sizeof(WNDCLASSEXW) ||
        !window_class->lpszClassName || !window_class->lpfnWndProc) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    for (int i = 0; i < USER_MAX_CLASSES; i++) {
        USER_CLASS *entry = &g_user_classes[i];
        if (entry->Used && entry->Instance == window_class->hInstance &&
            user_class_name_equal(entry->Name, window_class->lpszClassName)) {
            SetLastError(1410); /* ERROR_CLASS_ALREADY_EXISTS */
            return 0;
        }
    }

    for (int i = 0; i < USER_MAX_CLASSES; i++) {
        USER_CLASS *entry = &g_user_classes[i];
        if (!entry->Used) {
            int n = 0;
            while (window_class->lpszClassName[n] &&
                   n + 1 < USER_CLASS_NAME_MAX) {
                entry->Name[n] = window_class->lpszClassName[n];
                n++;
            }
            entry->Name[n] = 0;
            entry->Atom = (ATOM)(i + 1);
            entry->Style = window_class->style;
            entry->WindowProc = window_class->lpfnWndProc;
            entry->WindowExtra = window_class->cbWndExtra;
            entry->Instance = window_class->hInstance;
            entry->Used = 1;
            SetLastError(0);
            return entry->Atom;
        }
    }

    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return 0;
}

extern ULONGLONG NtUserRegisterWindowMessage(const UNICODE_STRING_K *name);

__declspec(dllexport) DWORD RegisterWindowMessageW(const WCHAR *message_name)
{
    DWORD length = 0;
    if (message_name)
        while (length < 64 && message_name[length]) length++;
    if (!length || length >= 64) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    UNICODE_STRING_K name = {
        (WCHAR)(length * sizeof(WCHAR)),
        (WCHAR)((length + 1) * sizeof(WCHAR)),
        (WCHAR *)message_name
    };
    DWORD id = (DWORD)NtUserRegisterWindowMessage(&name);
    if (!id) SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return id;
}

__declspec(dllexport) HANDLE LoadCursorW(HANDLE instance,
                                         const WCHAR *cursor_name)
{
    /* The standard cursor identifiers are MAKEINTRESOURCE values.  SVINDOWS does
     * not draw cursor bitmaps in USER32 yet, but callers still need stable,
     * typed handles while registering their window classes. */
    ULONGLONG raw_name = (ULONGLONG)cursor_name;
    if (!instance && raw_name <= 0xFFFFu) {
        DWORD id = (DWORD)raw_name;
        for (DWORD i = 0;
             i < sizeof(g_user_system_cursors) / sizeof(g_user_system_cursors[0]);
             i++) {
            if (g_user_system_cursors[i].ResourceId == id) {
                SetLastError(0);
                return &g_user_system_cursors[i];
            }
        }
    }

    SetLastError(1813); /* ERROR_RESOURCE_TYPE_NOT_FOUND */
    return 0;
}

static USER_CLASS *user_find_class(const WCHAR *class_name, HANDLE instance)
{
    ULONGLONG raw_name = (ULONGLONG)class_name;
    for (int i = 0; i < USER_MAX_CLASSES; i++) {
        USER_CLASS *entry = &g_user_classes[i];
        if (!entry->Used)
            continue;
        if (raw_name <= 0xFFFFu) {
            if (entry->Atom == (ATOM)raw_name)
                return entry;
        } else if (user_class_name_equal(entry->Name, class_name) &&
                   (!instance || !entry->Instance || entry->Instance == instance)) {
            return entry;
        }
    }
    return 0;
}

__declspec(dllexport) HANDLE CreateWindowInBand(
    DWORD ex_style, const WCHAR *class_name, const WCHAR *window_name,
    DWORD style, int x, int y, int width, int height, HANDLE parent,
    HANDLE menu, HANDLE instance, void *parameter, DWORD band)
{
    (void)window_name;
    USER_CLASS *window_class = user_find_class(class_name, instance);
    if (!window_class) {
        SetLastError(1407); /* ERROR_CANNOT_FIND_WND_CLASS */
        return 0;
    }

    for (int i = 0; i < USER_MAX_WINDOWS; i++) {
        USER_WINDOW *window = &g_user_windows[i];
        if (!window->Used) {
            window->Class = window_class;
            window->ExStyle = ex_style;
            window->Style = style;
            window->X = x;
            window->Y = y;
            window->Width = width;
            window->Height = height;
            window->Parent = parent;
            window->Menu = menu;
            window->Instance = instance;
            window->CreateParameter = parameter;
            window->Band = band;
            window->Used = 1;
            SetLastError(0);
            return window;
        }
    }

    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return 0;
}

__declspec(dllexport) BOOL GetMessageW(void *message, HANDLE window,
                                       DWORD minimum, DWORD maximum)
{
    (void)window;
    (void)minimum;
    (void)maximum;
    if (!message) {
        SetLastError(87);
        return -1;
    }

    if (!g_user_message_event) {
        HANDLE event = CreateEventW(0, 0, 0, 0);
        if (!event)
            return -1;
        if (!__sync_bool_compare_and_swap(&g_user_message_event, 0, event))
            CloseHandle(event);
    }

    /* There is no WM_QUIT and no queued input yet. Waiting is the correct
     * observable behavior: returning zero would falsely tell the GUI thread to tear
     * down its GUI thread and immediately start another one. */
    for (;;) {
        NTSTATUS status = NtWaitForSingleObject(g_user_message_event, 0, 0);
        if (!NT_SUCCESS(status)) {
            SetLastError(ntstatus_to_win32(status));
            return -1;
        }
    }
}
