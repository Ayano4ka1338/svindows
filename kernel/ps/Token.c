/* Process-owned security identity and queryable token handles. */
#include <svindows/Ps.h>
#include <svindows/Ob.h>
#include <svindows/Mm.h>
#include <svindows/Rtl.h>
#include <nt/Ntstatus.h>

#define TOKEN_QUERY 0x0008u
#define SE_GROUP_MANDATORY 0x00000001u
#define SE_GROUP_ENABLED_BY_DEFAULT 0x00000002u
#define SE_GROUP_ENABLED 0x00000004u
#define SE_GROUP_INTEGRITY 0x00000020u
#define SE_GROUP_INTEGRITY_ENABLED 0x00000040u

typedef struct { ULONG UserRid; } PS_TOKEN;
typedef struct { PVOID Sid; ULONG Attributes; ULONG Pad; } SID_ATTRIBUTES;
static POBJECT_TYPE g_token_type;

void PsInitializeTokens(void)
{
    g_token_type = ObCreateObjectType("Token", NULL);
}

UINT64 NtOpenProcessToken(UINT64 *a)
{
    HANDLE *out = (HANDLE *)a[2];
    if (!MmProbeForWrite((UINT64)out, sizeof(*out)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (((ULONG)a[1] & TOKEN_QUERY) == 0 ||
        ((ULONG)a[1] & ~(TOKEN_QUERY | 0x00020000u | 0x00100000u)))
        return (UINT64)STATUS_ACCESS_DENIED;

    PKPROCESS process = NULL;
    POBJECT reference = NULL;
    if (a[0] == (UINT64)-1) {
        PKTHREAD thread = KeGetCurrentThread();
        process = thread ? thread->Process : NULL;
    } else {
        NTSTATUS st = ObReferenceObjectByHandle((HANDLE)a[0], 0, NULL,
                                                &reference);
        if (!NT_SUCCESS(st)) return (UINT64)st;
        process = PsGetProcessFromObject(reference);
    }
    if (!process) {
        if (reference) ObDereferenceObject(reference);
        return (UINT64)STATUS_INVALID_HANDLE;
    }
    POBJECT object;
    NTSTATUS st = ObCreateObject(g_token_type, sizeof(PS_TOKEN), &object);
    if (NT_SUCCESS(st)) {
        ((PS_TOKEN *)object)->UserRid = process->SecurityUserRid;
        st = ObCreateHandle(object, (ACCESS_MASK)a[1], out);
        ObDereferenceObject(object);
    }
    if (reference) ObDereferenceObject(reference);
    return (UINT64)st;
}

UINT64 NtOpenThreadToken(UINT64 *a)
{
    NTSTATUS st = PsValidateThreadHandle((HANDLE)a[0]);
    if (!NT_SUCCESS(st)) return (UINT64)st;
    /* Threads have no impersonation token until impersonation is implemented. */
    return (UINT64)STATUS_NO_TOKEN;
}

static ULONG write_sid(UINT8 *dst, UINT8 authority, ULONG count,
                       const ULONG *sub)
{
    dst[0] = 1;
    dst[1] = (UINT8)count;
    for (ULONG i = 2; i < 7; i++) dst[i] = 0;
    dst[7] = authority;
    for (ULONG i = 0; i < count; i++)
        memcpy(dst + 8 + i * 4, &sub[i], sizeof(ULONG));
    return 8 + count * 4;
}

UINT64 NtQueryInformationToken(UINT64 *a)
{
    POBJECT object;
    NTSTATUS st = ObReferenceObjectByHandle((HANDLE)a[0], TOKEN_QUERY,
                                            g_token_type, &object);
    if (!NT_SUCCESS(st)) return (UINT64)st;
    ULONG cls = (ULONG)a[1];
    ULONG required;
    switch (cls) {
    case 1: required = 16 + 28; break; /* user */
    case 4: required = 8 + 28; break; /* owner */
    case 5: required = 8 + 16; break; /* primary group */
    case 2: required = 8 + 4 * 16 + 12 + 16 + 16 + 16; break;
    case 3: required = 4; break; /* no assigned privileges */
    case 8: case 12: case 18: case 20: required = 4; break;
    case 25: required = 16 + 16; break;
    default:
        ObDereferenceObject(object);
        return (UINT64)STATUS_INVALID_INFO_CLASS;
    }
    ULONG *length = (ULONG *)a[4];
    if (!MmProbeForWrite((UINT64)length, sizeof(*length))) {
        ObDereferenceObject(object);
        return (UINT64)STATUS_ACCESS_VIOLATION;
    }
    *length = required;
    if (a[3] < required) {
        ObDereferenceObject(object);
        return (UINT64)STATUS_BUFFER_TOO_SMALL;
    }
    UINT8 *buffer = (UINT8 *)a[2];
    if (!MmProbeForWrite((UINT64)buffer, required)) {
        ObDereferenceObject(object);
        return (UINT64)STATUS_ACCESS_VIOLATION;
    }
    memset(buffer, 0, required);
    ULONG rid = ((PS_TOKEN *)object)->UserRid;
    const ULONG user[] = {21, 1, 2, 3, rid};
    const ULONG world[] = {0};
    const ULONG authenticated[] = {11};
    const ULONG interactive[] = {4};
    const ULONG users[] = {32, 545};
    const ULONG medium[] = {8192};
    if (cls == 1) {
        ((SID_ATTRIBUTES *)buffer)->Sid = buffer + 16;
        ((SID_ATTRIBUTES *)buffer)->Attributes = SE_GROUP_ENABLED;
        write_sid(buffer + 16, 5, 5, user);
    } else if (cls == 4) {
        *(PVOID *)buffer = buffer + 8;
        write_sid(buffer + 8, 5, 5, user);
    } else if (cls == 5) {
        *(PVOID *)buffer = buffer + 8;
        write_sid(buffer + 8, 5, 2, users);
    } else if (cls == 2) {
        *(ULONG *)buffer = 4;
        SID_ATTRIBUTES *groups = (SID_ATTRIBUTES *)(buffer + 8);
        UINT8 *sid = buffer + 8 + 4 * 16;
        const ULONG *parts[] = {world, authenticated, interactive, users};
        const UINT8 authority[] = {1, 5, 5, 5};
        const ULONG counts[] = {1, 1, 1, 2};
        for (ULONG i = 0; i < 4; i++) {
            groups[i].Sid = sid;
            groups[i].Attributes = SE_GROUP_MANDATORY |
                SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED;
            sid += write_sid(sid, authority[i], counts[i], parts[i]);
        }
    } else if (cls == 25) {
        SID_ATTRIBUTES *label = (SID_ATTRIBUTES *)buffer;
        label->Sid = buffer + 16;
        label->Attributes = SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED;
        write_sid(buffer + 16, 16, 1, medium);
    } else if (cls == 8 || cls == 12 || cls == 18) {
        *(ULONG *)buffer = 1; /* primary, interactive session, default elevation */
    }
    ObDereferenceObject(object);
    return (UINT64)STATUS_SUCCESS;
}
