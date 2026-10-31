/* Activation contexts backed by a validated manifest and a per-thread stack. */
typedef struct _ACTCTXW_K {
    DWORD Size, Flags;
    const WCHAR *Source;
    WORD Architecture, Language;
    const WCHAR *AssemblyDirectory, *ResourceName, *ApplicationName;
    HANDLE Module;
} ACTCTXW_K;

typedef struct _ACTCTX_K {
    volatile long References;
    DWORD ManifestSize;
    BYTE Manifest[1];
} ACTCTX_K;

typedef struct _ACTCTX_FRAME_K {
    struct _ACTCTX_FRAME_K *Previous;
    ACTCTX_K *Context;
    ULONGLONG Cookie;
} ACTCTX_FRAME_K;

#define ACTCTX_RESOURCE_NAME_VALID 0x008u
#define ACTCTX_HMODULE_VALID       0x080u
#define ACTCTX_ALLOWED_FLAGS       (ACTCTX_RESOURCE_NAME_VALID | ACTCTX_HMODULE_VALID)
#define ERROR_SXS_CANT_GEN_ACTCTX  14001u
#define ERROR_INVALID_DATA         13u

static volatile long actctx_tls_state;
static DWORD actctx_tls_index;
static volatile ULONGLONG actctx_next_cookie;

static DWORD actctx_stack_index(void)
{
    long state = actctx_tls_state;
    if (state == 2) return actctx_tls_index;
    if (__sync_bool_compare_and_swap(&actctx_tls_state, 0, 1)) {
        DWORD index = TlsAlloc();
        actctx_tls_index = index;
        __sync_synchronize();
        actctx_tls_state = index == TLS_OUT_OF_INDEXES ? 3 : 2;
        return index;
    }
    while ((state = actctx_tls_state) == 1) __asm__ volatile("pause");
    return state == 2 ? actctx_tls_index : TLS_OUT_OF_INDEXES;
}

static void actctx_release(ACTCTX_K *ctx)
{
    if (ctx && __sync_sub_and_fetch(&ctx->References, 1) == 0)
        HeapFree(GetProcessHeap(), 0, ctx);
}

/* Accept UTF-8 or UTF-16 manifests with an assembly root. A malformed or
 * non-manifest file must not yield an apparently usable activation context. */
static BOOL actctx_manifest_valid(const BYTE *data, DWORD size)
{
    if (!data || size < 20) return 0;
    DWORD offset = size >= 3 && data[0] == 0xef && data[1] == 0xbb &&
                   data[2] == 0xbf ? 3 : 0;
    BOOL utf16 = size >= offset + 2 && data[offset] == '<' &&
                 data[offset + 1] == 0;
    const char root[] = "<assembly";
    for (DWORD i = offset; i < size; i++) {
        DWORD j = 0;
        for (; j < sizeof(root) - 1; j++) {
            DWORD at = i + j * (utf16 ? 2 : 1);
            if (at >= size || data[at] != (BYTE)root[j] ||
                (utf16 && (at + 1 >= size || data[at + 1]))) break;
        }
        if (j == sizeof(root) - 1) return 1;
    }
    return 0;
}

__declspec(dllexport) HANDLE CreateActCtxW(const ACTCTXW_K *input)
{
    if (!input || input->Size != sizeof(*input) ||
        (input->Flags & ~ACTCTX_ALLOWED_FLAGS) ||
        ((input->Flags & ACTCTX_RESOURCE_NAME_VALID) &&
         !(input->Flags & ACTCTX_HMODULE_VALID)) ||
        (!(input->Flags & ACTCTX_HMODULE_VALID) && !input->Source) ||
        ((input->Flags & ACTCTX_HMODULE_VALID) && !input->Module)) {
        SetLastError(87);
        return NT_INVALID_HANDLE;
    }

    const BYTE *manifest = 0;
    DWORD size = 0;
    BYTE *owned = 0;
    if (input->Flags & ACTCTX_HMODULE_VALID) {
        const WCHAR *name = input->Flags & ACTCTX_RESOURCE_NAME_VALID
            ? input->ResourceName : (const WCHAR *)(ULONGLONG)1;
        if (!name) { SetLastError(87); return NT_INVALID_HANDLE; }
        HANDLE resource = FindResourceW(input->Module, name,
                                        (const WCHAR *)(ULONGLONG)24);
        if (!resource) return NT_INVALID_HANDLE;
        size = SizeofResource(input->Module, resource);
        manifest = (const BYTE *)LockResource(LoadResource(input->Module, resource));
    } else {
        HANDLE file = CreateFileW(input->Source, 0x80000000u, 7, 0, 3, 0, 0);
        if (file == NT_INVALID_HANDLE) return file;
        DWORD high = 0;
        size = GetFileSize(file, &high);
        if (high || size > 1024 * 1024 || size < 20) {
            CloseHandle(file);
            SetLastError(ERROR_INVALID_DATA);
            return NT_INVALID_HANDLE;
        }
        owned = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
        DWORD read = 0;
        BOOL ok = owned && ReadFile(file, owned, size, &read, 0) && read == size;
        CloseHandle(file);
        if (!ok) {
            if (owned) HeapFree(GetProcessHeap(), 0, owned);
            SetLastError(owned ? ERROR_INVALID_DATA : 8);
            return NT_INVALID_HANDLE;
        }
        manifest = owned;
    }

    if (size > 1024 * 1024 || !actctx_manifest_valid(manifest, size)) {
        if (owned) HeapFree(GetProcessHeap(), 0, owned);
        SetLastError(ERROR_SXS_CANT_GEN_ACTCTX);
        return NT_INVALID_HANDLE;
    }
    ACTCTX_K *ctx = (ACTCTX_K *)HeapAlloc(GetProcessHeap(), 0,
                                          sizeof(*ctx) + size);
    if (!ctx) {
        if (owned) HeapFree(GetProcessHeap(), 0, owned);
        SetLastError(8);
        return NT_INVALID_HANDLE;
    }
    ctx->References = 1;
    ctx->ManifestSize = size;
    for (DWORD i = 0; i < size; i++) ctx->Manifest[i] = manifest[i];
    if (owned) HeapFree(GetProcessHeap(), 0, owned);
    return (HANDLE)ctx;
}

__declspec(dllexport) BOOL ActivateActCtx(HANDLE handle, ULONGLONG *cookie)
{
    if (!cookie || handle == NT_INVALID_HANDLE) { SetLastError(87); return 0; }
    DWORD index = actctx_stack_index();
    if (index == TLS_OUT_OF_INDEXES) { SetLastError(8); return 0; }
    ACTCTX_FRAME_K *frame = (ACTCTX_FRAME_K *)HeapAlloc(GetProcessHeap(), 0,
                                                        sizeof(*frame));
    if (!frame) { SetLastError(8); return 0; }
    frame->Previous = (ACTCTX_FRAME_K *)TlsGetValue(index);
    frame->Context = (ACTCTX_K *)handle;
    frame->Cookie = __sync_add_and_fetch(&actctx_next_cookie, 1);
    if (handle) __sync_add_and_fetch(&frame->Context->References, 1);
    if (!TlsSetValue(index, frame)) {
        actctx_release(frame->Context);
        HeapFree(GetProcessHeap(), 0, frame);
        return 0;
    }
    *cookie = frame->Cookie;
    return 1;
}

__declspec(dllexport) BOOL DeactivateActCtx(DWORD flags, ULONGLONG cookie)
{
    if (flags || !cookie || actctx_tls_state != 2) {
        SetLastError(87);
        return 0;
    }
    DWORD index = actctx_tls_index;
    ACTCTX_FRAME_K *frame = (ACTCTX_FRAME_K *)TlsGetValue(index);
    if (!frame || frame->Cookie != cookie) { SetLastError(87); return 0; }
    if (!TlsSetValue(index, frame->Previous)) return 0;
    actctx_release(frame->Context);
    HeapFree(GetProcessHeap(), 0, frame);
    return 1;
}

__declspec(dllexport) void ReleaseActCtx(HANDLE handle)
{
    if (handle && handle != NT_INVALID_HANDLE)
        actctx_release((ACTCTX_K *)handle);
}
