/*
 * user/w/advapi32/Advapi32.c - the registry slice of advapi32.dll.
 *
 * Implements the classic Win32 Reg* API on top of the native Nt*Key services,
 * building the real OBJECT_ATTRIBUTES / UNICODE_STRING / KEY_VALUE_PARTIAL_-
 * INFORMATION structures exactly as advapi32 does on Windows. Predefined roots
 * (HKEY_LOCAL_MACHINE, ...) map to absolute registry paths opened from the root;
 * a real key handle passes through as the OBJECT_ATTRIBUTES RootDirectory.
 */

#include <stdarg.h>

typedef void              *HANDLE;
typedef void              *HKEY;
typedef unsigned char      BYTE;
typedef unsigned short     WCHAR;
typedef unsigned long      DWORD;
typedef unsigned long long ULONGLONG;
typedef long               LONG;
typedef long               NTSTATUS;

#define ERROR_SUCCESS         0L
#define ERROR_FILE_NOT_FOUND  2L
#define KEY_READ              0x20019
#define NT_SUCCESS(s)         ((NTSTATUS)(s) >= 0)
#define KeyValuePartialInformation 2

#define HKEY_CURRENT_USER  ((HKEY)(unsigned long long)0x80000001ULL)
#define HKEY_LOCAL_MACHINE ((HKEY)(unsigned long long)0x80000002ULL)
#define HKEY_USERS         ((HKEY)(unsigned long long)0x80000003ULL)

typedef struct _UNICODE_STRING { WCHAR Length, MaximumLength; WCHAR *Buffer; }
    UNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    DWORD           Length;
    HANDLE          RootDirectory;
    UNICODE_STRING *ObjectName;
    DWORD           Attributes;
    void           *SecurityDescriptor;
    void           *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;

typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
    DWORD TitleIndex;
    DWORD Type;
    DWORD DataLength;
    BYTE  Data[1];
} KEY_VALUE_PARTIAL_INFORMATION;

/* Native registry services (real signatures). */
extern NTSTATUS NtOpenKey(HANDLE *KeyHandle, DWORD DesiredAccess,
                          OBJECT_ATTRIBUTES *ObjectAttributes);
extern NTSTATUS NtCreateKey(HANDLE *KeyHandle, DWORD DesiredAccess,
                            OBJECT_ATTRIBUTES *ObjectAttributes, DWORD TitleIndex,
                            void *Class, DWORD CreateOptions, DWORD *Disposition);
extern NTSTATUS NtSetValueKey(HANDLE KeyHandle, UNICODE_STRING *ValueName,
                              DWORD TitleIndex, DWORD Type, const void *Data,
                              DWORD DataSize);
extern NTSTATUS NtQueryValueKey(HANDLE KeyHandle, UNICODE_STRING *ValueName,
                                DWORD InfoClass, void *Info, DWORD Length,
                                DWORD *ResultLength);
extern NTSTATUS NtDeleteKey(HANDLE KeyHandle);
extern DWORD RtlNtStatusToDosError(NTSTATUS status);
extern long     NtClose(HANDLE h);

/* Manifest ETW providers live in ntdll's process-local registry. Advapi32's
 * public entry points share the same handles and filtering state. */
extern DWORD EtwEventRegister(const void *guid, void *callback, void *context,
                              ULONGLONG *handle);
extern DWORD EtwEventUnregister(ULONGLONG handle);
extern DWORD EtwEventWrite(ULONGLONG handle, const void *descriptor,
                           DWORD count, const void *data);
extern DWORD EtwEventWriteTransfer(ULONGLONG handle, const void *descriptor,
                                   const void *activity, const void *related,
                                   DWORD count, const void *data);
extern BYTE EtwEventEnabled(ULONGLONG handle, const void *descriptor);
extern DWORD EtwRegisterTraceGuidsW(void *callback, void *context,
                                    const void *guid, DWORD count,
                                    void *classes, const WCHAR *mof_path,
                                    const WCHAR *mof_name,
                                    ULONGLONG *handle);
extern DWORD EtwUnregisterTraceGuids(ULONGLONG handle);
extern DWORD EtwGetTraceEnableLevel(ULONGLONG handle);
extern DWORD EtwGetTraceEnableFlags(ULONGLONG handle);
extern ULONGLONG EtwGetTraceLoggerHandle(const void *buffer);
extern DWORD EtwTraceMessageVa(ULONGLONG logger, DWORD flags,
                               const void *guid, unsigned short number,
                               va_list args);
extern DWORD EtwStartTraceW(ULONGLONG *trace, const WCHAR *name,
                            void *properties);
extern DWORD EtwStopTraceW(ULONGLONG trace, const WCHAR *name,
                           void *properties);
extern DWORD EtwEnableTraceEx(const void *provider, const void *source,
                              ULONGLONG trace, DWORD enabled, BYTE level,
                              ULONGLONG match_any, ULONGLONG match_all,
                              DWORD property, const void *filter);

__declspec(dllexport) DWORD EventRegister(const void *guid, void *callback,
                                          void *context, ULONGLONG *handle)
{ return EtwEventRegister(guid, callback, context, handle); }

__declspec(dllexport) DWORD EventUnregister(ULONGLONG handle)
{ return EtwEventUnregister(handle); }

__declspec(dllexport) DWORD EventWrite(ULONGLONG handle,
                                       const void *descriptor, DWORD count,
                                       const void *data)
{ return EtwEventWrite(handle, descriptor, count, data); }

__declspec(dllexport) DWORD EventWriteTransfer(ULONGLONG handle,
                                               const void *descriptor,
                                               const void *activity,
                                               const void *related,
                                               DWORD count, const void *data)
{
    return EtwEventWriteTransfer(handle, descriptor, activity, related,
                                 count, data);
}

__declspec(dllexport) BYTE EventEnabled(ULONGLONG handle,
                                        const void *descriptor)
{ return EtwEventEnabled(handle, descriptor); }

__declspec(dllexport) DWORD RegisterTraceGuidsW(void *callback, void *context,
                                                const void *guid, DWORD count,
                                                void *classes,
                                                const WCHAR *mof_path,
                                                const WCHAR *mof_name,
                                                ULONGLONG *handle)
{
    return EtwRegisterTraceGuidsW(callback, context, guid, count, classes,
                                  mof_path, mof_name, handle);
}

__declspec(dllexport) DWORD UnregisterTraceGuids(ULONGLONG handle)
{ return EtwUnregisterTraceGuids(handle); }

__declspec(dllexport) DWORD GetTraceEnableLevel(ULONGLONG handle)
{ return EtwGetTraceEnableLevel(handle); }

__declspec(dllexport) DWORD GetTraceEnableFlags(ULONGLONG handle)
{ return EtwGetTraceEnableFlags(handle); }

__declspec(dllexport) ULONGLONG GetTraceLoggerHandle(const void *buffer)
{ return EtwGetTraceLoggerHandle(buffer); }

__declspec(dllexport) DWORD TraceMessageVa(ULONGLONG logger, DWORD flags,
                                           const void *guid,
                                           unsigned short number,
                                           va_list args)
{ return EtwTraceMessageVa(logger, flags, guid, number, args); }

__declspec(dllexport) DWORD StartTraceW(ULONGLONG *trace,
                                        const WCHAR *name, void *properties)
{ return EtwStartTraceW(trace, name, properties); }

__declspec(dllexport) DWORD StopTraceW(ULONGLONG trace,
                                       const WCHAR *name, void *properties)
{ return EtwStopTraceW(trace, name, properties); }

__declspec(dllexport) DWORD EnableTraceEx(const void *provider,
                                          const void *source,
                                          ULONGLONG trace, DWORD enabled,
                                          BYTE level, ULONGLONG match_any,
                                          ULONGLONG match_all,
                                          DWORD property, const void *filter)
{
    return EtwEnableTraceEx(provider, source, trace, enabled, level,
                            match_any, match_all, property, filter);
}

__declspec(dllexport) DWORD TraceMessage(ULONGLONG logger, DWORD flags,
                                         const void *guid,
                                         DWORD number, ...)
{
    va_list args;
    va_start(args, number);
    DWORD status = EtwTraceMessageVa(logger, flags, guid,
                                     (unsigned short)number, args);
    va_end(args);
    return status;
}

static int is_predefined(HKEY h)
{
    return ((unsigned long long)h & 0x80000000ULL) != 0;
}

static const char *root_prefix(HKEY h)
{
    /* Callers pass sign-extended handle-sized values (0xFFFFFFFF8xxxxxxx);
     * normalize to the 32-bit predefined-root code before matching. */
    unsigned long long v = (unsigned long long)h;
    if (v & 0xFFFFFFFF00000000ULL)
        v &= 0xFFFFFFFFULL;
    switch (v) {
    case 0x80000002ULL: return "\\Registry\\Machine";
    case 0x80000001ULL: return "\\Registry\\User";
    case 0x80000003ULL: return "\\Registry\\User";
    default:            return "\\Registry";
    }
}

/* Widen an ASCII string into `w`, returning its length in characters. */
static int widen(WCHAR *w, const char *s, int cap)
{
    int n = 0;
    while (*s && n < cap - 1)
        w[n++] = (WCHAR)(unsigned char)*s++;
    w[n] = 0;
    return n;
}

static void init_ustr(UNICODE_STRING *u, WCHAR *buf, int chars)
{
    u->Length = (WCHAR)(chars * 2);
    u->MaximumLength = (WCHAR)(chars * 2 + 2);
    u->Buffer = buf;
}

static void init_oa(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *name, HANDLE root)
{
    oa->Length = sizeof(*oa);
    oa->RootDirectory = root;
    oa->ObjectName = name;
    oa->Attributes = 0;
    oa->SecurityDescriptor = 0;
    oa->SecurityQualityOfService = 0;
}

/* Build the (name, RootDirectory) pair for a Reg* call: a predefined root
 * becomes an absolute path opened from the registry root; a real handle passes
 * through with the relative subkey. */
static void build_key_name(HKEY hKey, const char *subKey, WCHAR *wbuf,
                           UNICODE_STRING *name, HANDLE *root)
{
    if (is_predefined(hKey)) {
        char path[256];
        int i = 0;
        const char *pre = root_prefix(hKey);
        while (*pre)
            path[i++] = *pre++;
        if (subKey && *subKey) {
            path[i++] = '\\';
            while (*subKey)
                path[i++] = *subKey++;
        }
        path[i] = 0;
        init_ustr(name, wbuf, widen(wbuf, path, 256));
        *root = 0;
    } else {
        init_ustr(name, wbuf, widen(wbuf, subKey ? subKey : "", 256));
        *root = (HANDLE)hKey;
    }
}

static int wide_len(const WCHAR *s)
{
    int n = 0;
    while (s && s[n])
        n++;
    return n;
}

static void build_key_name_w(HKEY hKey, const WCHAR *subKey, WCHAR *wbuf,
                             UNICODE_STRING *name, HANDLE *root)
{
    int n = 0;
    if (is_predefined(hKey)) {
        const char *pre = root_prefix(hKey);
        while (*pre && n < 255)
            wbuf[n++] = (WCHAR)(unsigned char)*pre++;
        if (subKey && *subKey && n < 255)
            wbuf[n++] = '\\';
        while (subKey && *subKey && n < 255)
            wbuf[n++] = *subKey++;
        *root = 0;
    } else {
        while (subKey && *subKey && n < 255)
            wbuf[n++] = *subKey++;
        *root = (HANDLE)hKey;
    }
    wbuf[n] = 0;
    init_ustr(name, wbuf, n);
}

__declspec(dllexport) LONG RegOpenKeyExA(HKEY hKey, const char *subKey,
                                         DWORD options, DWORD sam, HKEY *result)
{
    (void)options; (void)sam;
    WCHAR wbuf[256];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE root, out = 0;
    build_key_name(hKey, subKey, wbuf, &name, &root);
    init_oa(&oa, &name, root);
    NTSTATUS st = NtOpenKey(&out, KEY_READ, &oa);
    *result = (HKEY)out;
    return NT_SUCCESS(st) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegOpenKeyExW(HKEY hKey, const WCHAR *subKey,
                                         DWORD options, DWORD sam, HKEY *result)
{
    (void)options; (void)sam;
    WCHAR wbuf[256];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE root, out = 0;
    build_key_name_w(hKey, subKey, wbuf, &name, &root);
    init_oa(&oa, &name, root);
    NTSTATUS st = NtOpenKey(&out, KEY_READ, &oa);
    *result = (HKEY)out;
    return NT_SUCCESS(st) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegOpenKeyW(HKEY hKey, const WCHAR *subKey,
                                       HKEY *result)
{
    if (!result) return 87;
    return RegOpenKeyExW(hKey, subKey, 0, KEY_READ, result);
}

__declspec(dllexport) LONG RegCreateKeyExA(HKEY hKey, const char *subKey,
                                           DWORD res, char *cls, DWORD options,
                                           DWORD sam, void *sa, HKEY *result,
                                           DWORD *disposition)
{
    (void)res; (void)cls; (void)options; (void)sam; (void)sa;
    WCHAR wbuf[256];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE root, out = 0;
    build_key_name(hKey, subKey, wbuf, &name, &root);
    init_oa(&oa, &name, root);
    NTSTATUS st = NtCreateKey(&out, KEY_READ, &oa, 0, 0, 0, disposition);
    *result = (HKEY)out;
    return NT_SUCCESS(st) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegCreateKeyA(HKEY hKey, const char *subKey,
                                          HKEY *result)
{
    DWORD disposition = 0;
    return RegCreateKeyExA(hKey, subKey, 0, 0, 0, KEY_READ, 0,
                           result, &disposition);
}

/* Account names are not exposed until the logon subsystem owns identity. */
__declspec(dllexport) int GetUserNameW(WCHAR *buffer, DWORD *size)
{
    (void)buffer;
    if (size) *size = 0;
    return 0;
}

__declspec(dllexport) LONG RegCreateKeyExW(HKEY hKey, const WCHAR *subKey,
                                           DWORD res, WCHAR *cls, DWORD options,
                                           DWORD sam, void *sa, HKEY *result,
                                           DWORD *disposition)
{
    (void)res; (void)cls; (void)options; (void)sam; (void)sa;
    WCHAR wbuf[256];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE root, out = 0;
    build_key_name_w(hKey, subKey, wbuf, &name, &root);
    init_oa(&oa, &name, root);
    NTSTATUS st = NtCreateKey(&out, KEY_READ, &oa, 0, 0, 0, disposition);
    *result = (HKEY)out;
    return NT_SUCCESS(st) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegSetValueExA(HKEY hKey, const char *valueName,
                                          DWORD res, DWORD type,
                                          const BYTE *data, DWORD cbData)
{
    (void)res;
    WCHAR wbuf[128];
    UNICODE_STRING name;
    init_ustr(&name, wbuf, widen(wbuf, valueName ? valueName : "", 128));
    NTSTATUS st = NtSetValueKey((HANDLE)hKey, &name, 0, type, data, cbData);
    return NT_SUCCESS(st) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegSetValueExW(HKEY hKey, const WCHAR *valueName,
                                          DWORD res, DWORD type,
                                          const BYTE *data, DWORD cbData)
{
    (void)res;
    UNICODE_STRING name;
    init_ustr(&name, (WCHAR *)(valueName ? valueName : L""),
              wide_len(valueName));
    NTSTATUS st = NtSetValueKey((HANDLE)hKey, &name, 0, type, data, cbData);
    return NT_SUCCESS(st) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegQueryValueExA(HKEY hKey, const char *valueName,
                                            DWORD *res, DWORD *type,
                                            BYTE *data, DWORD *cbData)
{
    (void)res;
    WCHAR wbuf[128];
    UNICODE_STRING name;
    init_ustr(&name, wbuf, widen(wbuf, valueName ? valueName : "", 128));

    /* KEY_VALUE_PARTIAL_INFORMATION header + up to a page of data. */
    BYTE buf[512];
    DWORD resultLen = 0;
    NTSTATUS st = NtQueryValueKey((HANDLE)hKey, &name, KeyValuePartialInformation,
                                  buf, sizeof(buf), &resultLen);
    if (!NT_SUCCESS(st))
        return ERROR_FILE_NOT_FOUND;

    KEY_VALUE_PARTIAL_INFORMATION *kvpi = (KEY_VALUE_PARTIAL_INFORMATION *)buf;
    if (type)
        *type = kvpi->Type;
    if (data && cbData) {
        DWORD copy = *cbData < kvpi->DataLength ? *cbData : kvpi->DataLength;
        for (DWORD i = 0; i < copy; i++)
            data[i] = kvpi->Data[i];
    }
    if (cbData)
        *cbData = kvpi->DataLength;
    return ERROR_SUCCESS;
}

__declspec(dllexport) LONG RegQueryValueExW(HKEY hKey, const WCHAR *valueName,
                                            DWORD *res, DWORD *type,
                                            BYTE *data, DWORD *cbData)
{
    (void)res;
    UNICODE_STRING name;
    init_ustr(&name, (WCHAR *)(valueName ? valueName : L""),
              wide_len(valueName));

    BYTE buf[512];
    DWORD resultLen = 0;
    NTSTATUS st = NtQueryValueKey((HANDLE)hKey, &name, KeyValuePartialInformation,
                                  buf, sizeof(buf), &resultLen);
    if (!NT_SUCCESS(st))
        return ERROR_FILE_NOT_FOUND;

    KEY_VALUE_PARTIAL_INFORMATION *kvpi = (KEY_VALUE_PARTIAL_INFORMATION *)buf;
    if (type)
        *type = kvpi->Type;
    if (data && cbData) {
        DWORD copy = *cbData < kvpi->DataLength ? *cbData : kvpi->DataLength;
        for (DWORD i = 0; i < copy; i++)
            data[i] = kvpi->Data[i];
    }
    if (cbData)
        *cbData = kvpi->DataLength;
    return ERROR_SUCCESS;
}

__declspec(dllexport) LONG RegCloseKey(HKEY hKey)
{
    if (is_predefined(hKey))
        return ERROR_SUCCESS;
    NtClose((HANDLE)hKey);
    return ERROR_SUCCESS;
}

__declspec(dllexport) LONG RegDeleteKeyExW(HKEY hKey, const WCHAR *subKey,
                                           DWORD samDesired, DWORD reserved)
{
    (void)samDesired; /* This system has one 64-bit registry view. */
    if (!subKey || !subKey[0] || reserved) return 87;
    int prefix = 0;
    if (is_predefined(hKey)) {
        const char *p = root_prefix(hKey);
        while (p[prefix]) prefix++;
        prefix++; /* separator */
    }
    int n = 0;
    while (subKey[n] && n + prefix < 255) n++;
    if (subKey[n]) return 206; /* ERROR_FILENAME_EXCED_RANGE */
    WCHAR wbuf[256];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE root, key = 0;
    build_key_name_w(hKey, subKey, wbuf, &name, &root);
    init_oa(&oa, &name, root);
    NTSTATUS st = NtOpenKey(&key, 0x00010000 /* DELETE */, &oa);
    if (!NT_SUCCESS(st)) return (LONG)RtlNtStatusToDosError(st);
    st = NtDeleteKey(key);
    NtClose(key);
    return NT_SUCCESS(st) ? ERROR_SUCCESS : (LONG)RtlNtStatusToDosError(st);
}

__declspec(dllexport) LONG RegDeleteKeyW(HKEY hKey, const WCHAR *subKey)
{ return RegDeleteKeyExW(hKey, subKey, 0, 0); }

/* ------------------------------------------------------------------ */
/* Tokens and SIDs                                                     */
/* ------------------------------------------------------------------ */

typedef int BOOL_K;
#define TRUE_K  1
#define FALSE_K 0

extern NTSTATUS NtAllocateVirtualMemory(HANDLE Process, void **BaseAddress,
                                        unsigned long long ZeroBits,
                                        unsigned long long *RegionSize,
                                        DWORD AllocationType, DWORD Protect);

static void *sid_alloc(DWORD bytes)
{
    void *base = 0;
    unsigned long long size = bytes;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1ULL, &base, 0, &size,
                                          0x3000 /* MEM_COMMIT|MEM_RESERVE */,
                                          0x04 /* PAGE_READWRITE */);
    return NT_SUCCESS(st) ? base : 0;
}

extern NTSTATUS NtFreeVirtualMemory(HANDLE process, void **base,
                                    unsigned long long *size, DWORD type);

__declspec(dllexport) BOOL_K AllocateAndInitializeSid(
    const BYTE *identifier_authority, BYTE sub_authority_count,
    DWORD s0, DWORD s1, DWORD s2, DWORD s3, DWORD s4, DWORD s5, DWORD s6,
    DWORD s7, void **sid)
{
    if (!identifier_authority || !sub_authority_count ||
        sub_authority_count > 8 || !sid)
        return FALSE_K;
    BYTE *raw = sid_alloc(8 + 4ull * sub_authority_count);
    if (!raw)
        return FALSE_K;
    raw[0] = 1; /* revision */
    raw[1] = sub_authority_count;
    for (int i = 0; i < 6; i++)
        raw[2 + i] = identifier_authority[i];
    DWORD *authorities = (DWORD *)(raw + 8);
    authorities[0] = s0;
    if (sub_authority_count > 1) authorities[1] = s1;
    if (sub_authority_count > 2) authorities[2] = s2;
    if (sub_authority_count > 3) authorities[3] = s3;
    if (sub_authority_count > 4) authorities[4] = s4;
    if (sub_authority_count > 5) authorities[5] = s5;
    if (sub_authority_count > 6) authorities[6] = s6;
    if (sub_authority_count > 7) authorities[7] = s7;
    *sid = raw;
    return TRUE_K;
}

__declspec(dllexport) void *FreeSid(void *sid)
{
    if (!sid) return 0;
    void *base = sid;
    unsigned long long size = 0;
    return NT_SUCCESS(NtFreeVirtualMemory((HANDLE)-1ULL, &base, &size, 0x8000))
               ? 0 : sid;
}

__declspec(dllexport) LONG RegOpenCurrentUser(DWORD desired_access, HKEY *key)
{
    (void)desired_access;
    if (!key)
        return ERROR_FILE_NOT_FOUND;
    *key = HKEY_CURRENT_USER;
    return ERROR_SUCCESS;
}

__declspec(dllexport) LONG RegCreateKeyW(HKEY hKey, const WCHAR *subKey,
                                         HKEY *result)
{
    return RegCreateKeyExW(hKey, subKey, 0, 0, 0, 0, 0, result, 0);
}

__declspec(dllexport) LONG RegSetValueW(HKEY key, const WCHAR *subkey,
    DWORD type, const WCHAR *data, DWORD bytes)
{
    const DWORD reg_sz = 1;
    const DWORD key_set_value = 2;
    (void)bytes;
    if (!data || type != reg_sz) return 87; /* ERROR_INVALID_PARAMETER */

    HKEY target = key;
    LONG status = ERROR_SUCCESS;
    if (subkey && *subkey)
        status = RegCreateKeyExW(key, subkey, 0, 0, 0, key_set_value, 0, &target, 0);
    else if (is_predefined(key))
        status = RegOpenKeyExW(key, 0, 0, key_set_value, &target);
    if (status != ERROR_SUCCESS) return status;

    DWORD size = ((DWORD)wide_len(data) + 1) * sizeof(*data);
    status = RegSetValueExW(target, 0, 0, type, (const BYTE *)data, size);
    if (target != key) NtClose(target);
    return status;
}

__declspec(dllexport) LONG RegQueryValueW(HKEY hKey, const WCHAR *subKey,
                                          void *data, LONG *data_size)
{
    DWORD type = 0;
    DWORD size = data_size ? (DWORD)*data_size : 0;
    LONG status = RegQueryValueExW(hKey, subKey, 0, &type, data,
                                   data_size ? &size : 0);
    if (data_size)
        *data_size = (LONG)size;
    return status;
}

__declspec(dllexport) LONG RegGetValueW(HKEY hKey, const WCHAR *subKey,
                                        const WCHAR *value, DWORD flags,
                                        DWORD *type, void *data,
                                        DWORD *data_size)
{
    (void)flags;
    HKEY opened = hKey;
    LONG status = ERROR_SUCCESS;
    if (subKey && subKey[0])
        status = RegOpenKeyExW(hKey, subKey, 0, KEY_READ, &opened);
    if (status != ERROR_SUCCESS)
        return status;
    status = RegQueryValueExW(opened, value, 0, type, data, data_size);
    if (opened != hKey)
        NtClose(opened);
    return status;
}

#include "AdvapiSid.inc"
#include "AdvapiToken.inc"
#include "AdvapiRegistryHive.inc"
/* Encoding detection is a heuristic for unmarked files. Sample pairs of
 * bytes directly so the buffer need not be aligned as WCHAR. */
static int unicode_control(unsigned ch)
{
    return ch == '\r' || ch == '\n' || ch == '\t' ||
           ch == ' ' || ch == 0x3000;
}

__declspec(dllexport) int IsTextUnicode(const void *buffer, int size, int *tests)
{
    const unsigned requested = tests ? (unsigned)*tests : ~0u;
    if (!buffer || size < 2) {
        if (tests) *tests = 0;
        return 0;
    }

    enum {
        ASCII16 = 1, STATISTICS = 2, CONTROLS = 4, SIGNATURE = 8,
        REVERSE = 4, ILLEGAL_CHARS = 0x100, ODD_LENGTH = 0x200,
        NULL_BYTES = 0x1000
    };
    const BYTE *bytes = buffer;
    unsigned found = size & 1 ? ODD_LENGTH : 0;
    unsigned words = (unsigned)size / 2;
    if (words > 256) words = 256;
    unsigned ascii = 0, reverse_ascii = 0, low_change = 0, high_change = 0;
    for (unsigned i = 0; i < words; ++i) {
        unsigned low = bytes[i * 2], high = bytes[i * 2 + 1];
        unsigned word = low | (high << 8);
        if (i == 0 && word == 0xfeff) found |= SIGNATURE;
        if (i == 0 && word == 0xfffe) found |= SIGNATURE << REVERSE;
        if ((!word && i + 1 != (unsigned)size / 2) ||
            (word == 0xfffe && i) || word == 0xffff || word == 0x0a0d)
            found |= ILLEGAL_CHARS;
        if ((!low || !high) && word) found |= NULL_BYTES;
        if (unicode_control(word)) found |= CONTROLS;
        if (unicode_control(high | (low << 8))) found |= CONTROLS << REVERSE;
        if (!high && ((low >= 32 && low < 127) || unicode_control(low))) ++ascii;
        if (!low && ((high >= 32 && high < 127) || unicode_control(high)))
            ++reverse_ascii;
        if (i) {
            int dl = (int)low - bytes[(i - 1) * 2];
            int dh = (int)high - bytes[(i - 1) * 2 + 1];
            low_change += (unsigned)(dl < 0 ? -dl : dl);
            high_change += (unsigned)(dh < 0 ? -dh : dh);
        }
    }
    if (ascii == words) found |= ASCII16;
    if (reverse_ascii == words) found |= ASCII16 << REVERSE;
    if (ascii > words / 2 || (words >= 3 && low_change > 4 * high_change))
        found |= STATISTICS;
    if (reverse_ascii > words / 2 || (words >= 3 && high_change > 4 * low_change))
        found |= STATISTICS << REVERSE;

    /* A BOM supplies stronger byte-order evidence than the heuristics. */
    if (found & SIGNATURE)
        found &= ~0xf0u;
    else if (found & (SIGNATURE << REVERSE))
        found &= ~0x0fu;
    found &= requested;
    if (tests) *tests = (int)found;
    return !(found & 0x0ff0) && (found & 0xf00f) != 0;
}

#include "AdvapiRegistryEnum.inc"
#include "AdvapiService.inc"
#include "AdvapiSecurityDescriptor.inc"
#include "AdvapiObjectSecurity.inc"
#include "AdvapiLsa.inc"
#include "AdvapiCrypto.inc"
