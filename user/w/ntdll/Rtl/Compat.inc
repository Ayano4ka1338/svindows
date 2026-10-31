/* Activation-context, CSR, environment and USER compatibility support.
 * Included by user/w/ntdll/Ntdllrtl.c. */
/* ------------------------------------------------------------------ */
/* Activation contexts: report "not found" so callers take their       */
/* fallback paths instead of treating garbage as a valid context.      */
/* ------------------------------------------------------------------ */

/*
 * There is no side-by-side store in this system, so no name ever resolves to
 * a manifest section. The canonical answer real ntdll gives for that case is
 * STATUS_SXS_SECTION_NOT_FOUND: user32's class-name capture accepts exactly
 * this status (or a fully successful lookup) -- any other negative status is
 * treated as a broken activation context and fails RegisterClass silently.
 */
#define STATUS_SXS_SECTION_NOT_FOUND 0xC0150008

NTSTATUS RtlFindActivationContextSectionString(ULONG flags, const void *guid,
                                               ULONG section, const void *name,
                                               void *data)
{
    (void)flags; (void)guid; (void)section; (void)name; (void)data;
    return STATUS_SXS_SECTION_NOT_FOUND;
}

NTSTATUS RtlActivateActivationContextUnsafeFast(void *cookie, void *ctx)
{
    (void)cookie; (void)ctx;
    return STATUS_SUCCESS;
}

NTSTATUS RtlDeactivateActivationContextUnsafeFast(void *cookie, void *frame)
{
    (void)cookie; (void)frame;
    return STATUS_SUCCESS;
}

NTSTATUS RtlReleaseActivationContext(void *ctx)
{
    (void)ctx;
    return STATUS_SUCCESS;
}

NTSTATUS RtlQueryInformationActiveActivationContext(ULONG cls, void *buf,
                                                    ULONG size, ULONG *ret)
{
    (void)cls; (void)buf; (void)size;
    if (ret)
        *ret = 0;
    return STATUS_NOT_FOUND;
}

NTSTATUS LdrQueryImageFileExecutionOptions(const UNICODE_STRING_NT *image,
                                           PCWSTR name, ULONG type,
                                           void *data, ULONG size,
                                           ULONG *ret_size)
{
    (void)image; (void)name; (void)type; (void)data; (void)size;
    (void)ret_size;
    return STATUS_NOT_FOUND;
}

void LdrFlushAlternateResourceModules(void)
{
}

/* ------------------------------------------------------------------ */
/* CSR / WNF / telemetry compatibility                                  */
/* ------------------------------------------------------------------ */

void *CsrAllocateCaptureBuffer(ULONG count, ULONG size)
{
    return RtlAllocateHeap(0, 0, size ? size : 16);
}

void CsrFreeCaptureBuffer(void *buffer)
{
    RtlFreeHeap(0, 0, buffer);
}

NTSTATUS CsrAllocateMessagePointer(void *buffer, ULONG size, void **message)
{
    (void)buffer;
    *message = 0;
    return STATUS_SUCCESS;
}

NTSTATUS CsrClientCallServer(void *message, void *capture, ULONG code,
                             ULONG size)
{
    (void)message; (void)capture; (void)code; (void)size;
    return STATUS_NOT_FOUND;
}

#include "Etw.inc"

NTSTATUS NtQueryWnfStateData(void *id, void *scope, void *context,
                             ULONG *stamp, void *buffer, ULONG *size)
{
    (void)id; (void)scope; (void)context; (void)stamp; (void)buffer;
    if (size)
        *size = 0;
    return STATUS_NOT_FOUND;
}

NTSTATUS RtlQueryWnfStateData(void *id, void *scope, void *context,
                              ULONG *stamp, void *buffer, ULONG *size)
{
    return NtQueryWnfStateData(id, scope, context, stamp, buffer, size);
}

NTSTATUS RtlPublishWnfStateData(void *id, void *scope, void *context)
{
    (void)id; (void)scope; (void)context;
    return STATUS_SUCCESS;
}

NTSTATUS RtlSubscribeWnfStateChangeNotification(void **subscription,
                                                void *id, ULONG state,
                                                void *callback, void *ctx,
                                                void *stamp, ULONG flags,
                                                void *blob)
{
    (void)id; (void)state; (void)callback; (void)ctx; (void)stamp;
    (void)flags; (void)blob;
    if (subscription)
        *subscription = 0;
    return STATUS_SUCCESS;
}

NTSTATUS RtlUnsubscribeWnfNotificationWaitForCompletion(void *subscription)
{
    (void)subscription;
    return STATUS_SUCCESS;
}

ULONG WinSqmAddToStream(const void *event, ULONG size, ULONG flags)
{
    (void)event; (void)size; (void)flags;
    return 0;
}

ULONG WinSqmAddToStreamEx(const void *event, ULONG size, ULONG flags,
                          void *extra)
{
    (void)event; (void)size; (void)flags; (void)extra;
    return 0;
}

ULONG WinSqmIncrementDWORD(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    return 0;
}

ULONG WinSqmSetDWORD(void *a, ULONG b, ULONG c)
{
    (void)a; (void)b; (void)c;
    return 0;
}

void ShipAssert(const char *file, ULONG line, ULONG code)
{
    (void)file; (void)line; (void)code;
}

/* ------------------------------------------------------------------ */
/* A minimal static environment block backing %VAR% expansion.         */
/* ------------------------------------------------------------------ */

static const struct { const WCHAR *name; const WCHAR *value; } g_env_vars[] = {
    { L"SystemDrive",   L"C:" },
    { L"SystemRoot",    L"C:\\Windows" },
    { L"windir",        L"C:\\Windows" },
    { L"TEMP",          L"C:\\Temp" },
    { L"TMP",           L"C:\\Temp" },
    { L"USERPROFILE",   L"C:\\Users\\User" },
    { L"HOMEDRIVE",     L"C:" },
    { L"HOMEPATH",      L"\\Users\\User" },
    { L"APPDATA",       L"C:\\Users\\User\\AppData\\Roaming" },
    { L"LOCALAPPDATA",  L"C:\\Users\\User\\AppData\\Local" },
    { L"ProgramFiles",  L"C:\\Program Files" },
    { L"ProgramData",   L"C:\\ProgramData" },
    { L"COMPUTERNAME",  L"SVINDOWS" },
};

static const WCHAR *env_lookup(const WCHAR *name)
{
    for (UINT i = 0; i < sizeof(g_env_vars) / sizeof(g_env_vars[0]); i++) {
        const WCHAR *a = g_env_vars[i].name, *b = name;
        for (;;) {
            WCHAR ca = *a++, cb = *b++;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (!cb)
                return ca ? 0 : g_env_vars[i].value;
            if (ca != cb)
                break;
        }
    }
    return 0;
}

NTSTATUS RtlCreateEnvironment(ULONG flags, void **environment)
{
    (void)flags;
    if (environment)
        *environment = (void *)1;
    return STATUS_SUCCESS;
}

NTSTATUS RtlDestroyEnvironment(void *environment)
{
    (void)environment;
    return STATUS_SUCCESS;
}

NTSTATUS RtlQueryEnvironmentVariable_U(void *environment,
                                       UNICODE_STRING_NT *name,
                                       UNICODE_STRING_NT *value,
                                       ULONG *value_length)
{
    (void)environment;
    if (!name || !value)
        return STATUS_INVALID_PARAMETER;
    WCHAR stack[32];
    UINT chars = name->Length / 2;
    if (chars > 31)
        return STATUS_NOT_FOUND;
    for (UINT i = 0; i < chars; i++)
        stack[i] = name->Buffer[i];
    stack[chars] = 0;

    const WCHAR *found = env_lookup(stack);
    if (!found)
        return STATUS_NOT_FOUND;

    UINT len = 0;
    while (found[len])
        len++;
    if (value->MaximumLength < (len + 1) * sizeof(WCHAR)) {
        if (value_length)
            *value_length = (len + 1) * sizeof(WCHAR);
        return 0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }
    memcpy(value->Buffer, found, (len + 1) * sizeof(WCHAR));
    value->Length = (WORD)(len * sizeof(WCHAR));
    if (value_length)
        *value_length = (len) * sizeof(WCHAR);
    return STATUS_SUCCESS;
}

NTSTATUS RtlSetEnvironmentVariable(void **environment,
                                   UNICODE_STRING_NT *name,
                                   UNICODE_STRING_NT *value)
{
    (void)environment; (void)name; (void)value;
    return STATUS_SUCCESS;
}

NTSTATUS RtlSetCurrentEnvironment(void *environment, void **previous)
{
    (void)environment;
    if (previous)
        *previous = 0;
    return STATUS_SUCCESS;
}

NTSTATUS RtlExpandEnvironmentStrings_U(void *environment,
                                       const UNICODE_STRING_NT *source,
                                       UNICODE_STRING_NT *destination,
                                       ULONG *destination_length)
{
    (void)environment;
    if (!source || !destination)
        return STATUS_INVALID_PARAMETER;

    UINT n = source->Length / 2;
    UINT out = 0;
    BOOLEAN overflow = FALSE;
    for (UINT i = 0; i < n; i++) {
        if (source->Buffer[i] != '%') {
            if (out + 1 < destination->MaximumLength / 2)
                destination->Buffer[out] = source->Buffer[i];
            else
                overflow = TRUE;
            out++;
            continue;
        }
        UINT j = i + 1;
        while (j < n && source->Buffer[j] != '%')
            j++;
        if (j >= n) {
            if (out + 1 < destination->MaximumLength / 2)
                destination->Buffer[out] = '%';
            out++;
            continue;
        }
        WCHAR name[32];
        UINT name_len = j - i - 1;
        if (name_len > 31)
            name_len = 31;
        for (UINT k = 0; k < name_len; k++)
            name[k] = source->Buffer[i + 1 + k];
        name[name_len] = 0;
        const WCHAR *value = env_lookup(name);
        if (!value)
            value = L"";
        for (; *value; value++) {
            if (out + 1 < destination->MaximumLength / 2)
                destination->Buffer[out] = *value;
            else
                overflow = TRUE;
            out++;
        }
        i = j;
    }
    if (out + 1 < destination->MaximumLength / 2 && !overflow) {
        destination->Buffer[out] = 0;
        destination->Length = (WORD)(out * sizeof(WCHAR));
        if (destination_length)
            *destination_length = (out + 1) * sizeof(WCHAR);
        return STATUS_SUCCESS;
    }
    if (destination_length)
        *destination_length = (out + 1) * sizeof(WCHAR);
    return 0x80000005L; /* STATUS_BUFFER_OVERFLOW */
}

/* ------------------------------------------------------------------ */
/* VDM backchannel: user32's UserRegisterWowHandlers gates its work on */
/* NtVdmControl(VDM_CHECK = 14) reporting "allowed" through a flag.     */
/* ------------------------------------------------------------------ */

NTSTATUS NtVdmControl(ULONG command, void *output)
{
    if (command == 14 && output) {
        ((BYTE *)output)[0x10] = 1;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_IMPLEMENTED;
}

/* ------------------------------------------------------------------ */
/* Win32k normally invokes user32!UserRegisterWowHandlers during USER  */
/* initialization, handing back the handler table whose sixth member   */
/* must be a user32-internal routine that RegisterClassW validates.    */
/* With no win32k we perform the same registration ourselves right     */
/* after the loader finishes DLL process attach.                       */
/* ------------------------------------------------------------------ */

static void wow_handler_stub(void)
{
}

typedef struct _LDR_PEB_WALK {
    BYTE Reserved[0x10];
    void *ImageBaseAddress;      /* 0x10 */
    void *Ldr;                   /* 0x18 */
} PEB_WALK;

typedef struct _PEB_LDR_WALK {
    BYTE Reserved[0x10];
    struct { void *Flink, *Blink; } InLoadOrderModuleList; /* 0x10 */
} PEB_LDR_WALK;

static int wide_name_is(const WCHAR *name, UINT length_chars,
                        const char *ascii)
{
    UINT i = 0;
    for (; i < length_chars && ascii[i]; i++) {
        WCHAR c = name[i];
        char a = ascii[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (c != (WCHAR)(BYTE)a)
            return FALSE;
    }
    return i == length_chars && ascii[i] == 0;
}

void LdrpRegisterWowHandlers(void);
__attribute__((noinline)) void LdrpRegisterWowHandlers(void)
{
    ULONGLONG peb;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
    if (!peb)
        return;
    PEB_LDR_WALK *ldr = *(PEB_LDR_WALK **)(peb + 0x18);
    if (!ldr)
        return;

    for (void *link = ldr->InLoadOrderModuleList.Flink;
         link && link != &ldr->InLoadOrderModuleList.Flink;
         link = *(void **)link) {
        BYTE *entry = (BYTE *)link;
        BYTE *dll_base = *(BYTE **)(entry + 0x30);
        WCHAR *name = *(WCHAR **)(entry + 0x60);
        WORD name_len = *(WORD *)(entry + 0x58);
        if (!dll_base || !name ||
            !wide_name_is(name, name_len / 2, "user32.dll"))
            continue;

        /* Walk user32's export table for UserRegisterWowHandlers. */
        DWORD pe_offset = *(DWORD *)(dll_base + 0x3C);
        DWORD *dd = (DWORD *)(dll_base + pe_offset + 24 + 112);
        DWORD exp_rva = dd[0], exp_size = dd[1]; /* directory entry 0 */
        if (!exp_rva || !exp_size)
            return;
        BYTE *dir = dll_base + exp_rva;
        UINT32 number_of_names = *(UINT32 *)(dir + 24);
        UINT32 names_rva = *(UINT32 *)(dir + 32);
        UINT32 funcs_rva = *(UINT32 *)(dir + 28);
        UINT16 *ordinals = (UINT16 *)(dll_base + *(UINT32 *)(dir + 36));
        UINT32 *functions = (UINT32 *)(dll_base + funcs_rva);

        for (UINT32 i = 0; i < number_of_names; i++) {
            const char *export_name =
                (const char *)(dll_base + *(UINT32 *)(dll_base +
                                                     names_rva + i * 4));
            static const char want[] = "UserRegisterWowHandlers";
            UINT j;
            for (j = 0; export_name[j] == want[j] && want[j]; j++)
                ;
            if (want[j])
                continue;

            void (*register_handlers)(const void *, BYTE *) =
                (void (*)(const void *, BYTE *))
                    (dll_base + functions[ordinals[i]]);
            void *handlers[8];
            for (UINT k = 0; k < 8; k++)
                handlers[k] = (void *)wow_handler_stub;
            /* RegisterClassW validates that this member points at
             * user32's own fnid-resolution routine (RVA 0xD1A0 in the
             * supplied build). */
            handlers[6] = dll_base + 0xD1A0;
            BYTE allowed = 0;
            register_handlers(handlers, &allowed);
            NtDisplayString("[ntdll] WowHandlers registered\r\n");
            return;
        }
        return;
    }
}

/*
 * ------------------------------------------------------------------------
 * DefWindowProc
 * ------------------------------------------------------------------------
 * USER32 exports DefWindowProcW/A as forwarders to NTDLL.NtdllDefWindowProc_W
 * /_A, so the default window procedure genuinely lives here. Returning zero
 * for everything (the previous stub) is wrong in a way that breaks window
 * creation outright: WM_NCCREATE's documented default is TRUE, and win32k
 * aborts CreateWindowEx when the class procedure -- which forwards the
 * message here -- answers FALSE. The results below are the documented
 * defaults; messages with a zero default fall through the switch.
 */
typedef unsigned long long ULONG_PTR;
typedef long long          LONG_PTR;

#define WM_CREATE                 0x0001
#define WM_DESTROY                0x0002
#define WM_SETREDRAW              0x000B
#define WM_SETTEXT                0x000C
#define WM_GETTEXT                0x000D
#define WM_GETTEXTLENGTH          0x000E
#define WM_ERASEBKGND             0x0014
#define WM_QUERYENDSESSION        0x0011
#define WM_QUERYOPEN              0x0013
#define WM_SHOWWINDOW             0x0018
#define WM_SETCURSOR              0x0020
#define WM_MOUSEACTIVATE          0x0021
#define WM_NCCREATE               0x0081
#define WM_NCDESTROY              0x0082
#define WM_NCCALCSIZE             0x0083
#define WM_NCHITTEST              0x0084
#define WM_NCACTIVATE             0x0086
#define WM_GETDLGCODE             0x0087
#define WM_NOTIFYFORMAT           0x0055
#define WM_DEVICECHANGE           0x0219
#define WM_INPUTLANGCHANGEREQUEST 0x0050
#define WM_QUERYUISTATE           0x0129
#define WM_CTLCOLORMSGBOX         0x0132
#define WM_CTLCOLORSTATIC         0x0138

#define HTCLIENT      1
#define MA_ACTIVATE   1
#define NFR_UNICODE   2

LONG_PTR NtdllDefWindowProc_W(HANDLE hwnd, UINT msg,
                                                    ULONG_PTR wparam,
                                                    LONG_PTR lparam)
{
    (void)hwnd;
    (void)wparam;
    (void)lparam;

    switch (msg) {
    /* Creation: a FALSE here cancels the window. */
    case WM_NCCREATE:
        return TRUE;

    /* Queries whose "nobody handled it" answer is affirmative. */
    case WM_NCACTIVATE:
    case WM_QUERYENDSESSION:
    case WM_QUERYOPEN:
    case WM_SETCURSOR:
    case WM_SETTEXT:
    case WM_ERASEBKGND:
    case WM_DEVICECHANGE:
    case WM_INPUTLANGCHANGEREQUEST:
        return TRUE;

    case WM_NCHITTEST:
        /* No non-client frame is tracked yet: every hit is in the client
         * area, which is the answer for a borderless window anyway. */
        return HTCLIENT;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_NOTIFYFORMAT:
        /* Every window in this system is a Unicode window. */
        return NFR_UNICODE;

    /* The CTLCOLOR family answers with a brush; no GDI stock brushes exist
     * yet, so the honest answer stays NULL rather than a fake handle. */
    case WM_CTLCOLORMSGBOX:
    case WM_CTLCOLORSTATIC:
        return 0;

    /* Everything else -- WM_CREATE, WM_DESTROY, WM_NCDESTROY, WM_NCCALCSIZE,
     * WM_PAINT, WM_SHOWWINDOW, the WINDOWPOS pair, ... -- defaults to 0. */
    default:
        return 0;
    }
}

LONG_PTR NtdllDefWindowProc_A(HANDLE hwnd, UINT msg,
                                                    ULONG_PTR wparam,
                                                    LONG_PTR lparam)
{
    return NtdllDefWindowProc_W(hwnd, msg, wparam, lparam);
}
