/*
 * cm/Registry.c - the Configuration Manager (Cm): an in-memory registry.
 *
 * Keys form a tree rooted at an anonymous super-root whose single child is
 * "Registry" (so absolute paths read "\Registry\Machine\..."). Each key holds a
 * list of named, typed values. Keys are handed to ring 3 as Key objects (Ob
 * handles): NtCreateKey/NtOpenKey return a handle, NtSetValueKey/NtQueryValueKey
 * operate through it, and NtClose (Ob) releases it. The key tree itself persists
 * independently of handles.
 *
 * Simplifications vs. real NT: names are ASCII, there is no on-disk hive (the
 * store is built at boot and lives in memory), and user pointers are trusted.
 */
#include <svindows/Cm.h>
#include <svindows/Ob.h>
#include <svindows/Ex.h>
#include <svindows/Ke.h>
#include <svindows/Mm.h>
#include <svindows/Rtl.h>
#include <nt/Ntobject.h>

typedef struct _CM_VALUE {
    LIST_ENTRY Link;
    char       Name[64];
    UINT32     Type;
    UINT32     Size;
    UINT8     *Data;
} CM_VALUE;

typedef struct _CM_KEY {
    char       Name[64];
    struct _CM_KEY *Parent;
    LIST_ENTRY SubkeyHead;  /* list of child CM_KEYs   */
    LIST_ENTRY SubkeyLink;  /* link in Parent->SubkeyHead */
    LIST_ENTRY ValueHead;   /* list of CM_VALUEs       */
    ULONG OpenObjects;
    BOOLEAN Deleted;
    UINT64 LastWriteTime;
} CM_KEY;

/* The Ob object handed to ring 3 just points at a persistent CM_KEY. */
typedef struct _CM_KEY_OBJECT {
    CM_KEY *Key;
} CM_KEY_OBJECT;

static POBJECT_TYPE g_key_type;
static CM_KEY      *g_cm_root; /* anonymous super-root */

/* ------------------------------------------------------------------ */
/* Tree primitives                                                    */
/* ------------------------------------------------------------------ */

static int ci_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return 0;
        if (!ca)
            return 1;
    }
}

static CM_KEY *cm_alloc_key(const char *name, CM_KEY *parent)
{
    CM_KEY *k = ExAllocatePoolWithTag(NonPagedPool, sizeof(CM_KEY), 'yeKC');
    if (!k)
        return NULL;
    memset(k, 0, sizeof(*k));
    SIZE_T i = 0;
    for (; name[i] && i < sizeof(k->Name) - 1; i++)
        k->Name[i] = name[i];
    k->Name[i] = 0;
    k->Parent = parent;
    k->LastWriteTime = KeGetTickCount() * 100000ULL;
    InitializeListHead(&k->SubkeyHead);
    InitializeListHead(&k->ValueHead);
    if (parent)
        InsertTailList(&parent->SubkeyHead, &k->SubkeyLink);
    if (parent) parent->LastWriteTime = k->LastWriteTime;
    return k;
}

static CM_KEY *cm_find_subkey(CM_KEY *parent, const char *name)
{
    for (PLIST_ENTRY e = parent->SubkeyHead.Flink; e != &parent->SubkeyHead;
         e = e->Flink) {
        CM_KEY *k = CONTAINING_RECORD(e, CM_KEY, SubkeyLink);
        if (ci_eq(k->Name, name))
            return k;
    }
    return NULL;
}

/* Walk a (possibly multi-component, '\'-separated) path from `start`, creating
 * missing keys when `create` is set. Empty components are skipped, so a leading
 * '\' and the absolute "\Registry\..." form both work. */
static CM_KEY *cm_walk(CM_KEY *start, const char *path, BOOLEAN create)
{
    CM_KEY *cur = start;
    const char *p = path;

    while (*p) {
        while (*p == '\\' || *p == '/')
            p++;
        if (!*p)
            break;

        char comp[64];
        int n = 0;
        while (*p && *p != '\\' && *p != '/' && n < (int)sizeof(comp) - 1)
            comp[n++] = *p++;
        comp[n] = 0;
        while (*p && *p != '\\' && *p != '/') /* skip an over-long component tail */
            p++;

        CM_KEY *child = cm_find_subkey(cur, comp);
        if (!child) {
            if (!create)
                return NULL;
            child = cm_alloc_key(comp, cur);
            if (!child)
                return NULL;
        }
        cur = child;
    }
    return cur;
}

static CM_VALUE *cm_find_value(CM_KEY *k, const char *name)
{
    for (PLIST_ENTRY e = k->ValueHead.Flink; e != &k->ValueHead; e = e->Flink) {
        CM_VALUE *v = CONTAINING_RECORD(e, CM_VALUE, Link);
        if (ci_eq(v->Name, name))
            return v;
    }
    return NULL;
}

static NTSTATUS cm_set_value(CM_KEY *k, const char *name, UINT32 type,
                             const void *data, UINT32 size)
{
    UINT8 *copy = NULL;
    if (size) {
        copy = ExAllocatePoolWithTag(NonPagedPool, size, 'laVC');
        if (!copy)
            return STATUS_NO_MEMORY;
        memcpy(copy, data, size);
    }

    CM_VALUE *v = cm_find_value(k, name);
    if (v) {
        if (v->Data)
            ExFreePool(v->Data);
    } else {
        v = ExAllocatePoolWithTag(NonPagedPool, sizeof(CM_VALUE), 'laVC');
        if (!v) {
            if (copy)
                ExFreePool(copy);
            return STATUS_NO_MEMORY;
        }
        memset(v, 0, sizeof(*v));
        SIZE_T i = 0;
        for (; name[i] && i < sizeof(v->Name) - 1; i++)
            v->Name[i] = name[i];
        v->Name[i] = 0;
        InsertTailList(&k->ValueHead, &v->Link);
    }
    v->Type = type;
    v->Size = size;
    v->Data = copy;
    k->LastWriteTime = KeGetTickCount() * 100000ULL;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Key objects / handles                                              */
/* ------------------------------------------------------------------ */

static void CmpKeyDelete(POBJECT object)
{
    CM_KEY *key = ((CM_KEY_OBJECT *)object)->Key;
    if (--key->OpenObjects == 0 && key->Deleted) {
        while (!IsListEmpty(&key->ValueHead)) {
            CM_VALUE *value = CONTAINING_RECORD(RemoveHeadList(&key->ValueHead),
                                                CM_VALUE, Link);
            if (value->Data) ExFreePool(value->Data);
            ExFreePool(value);
        }
        ExFreePool(key);
    }
}

static UINT64 cm_make_handle(CM_KEY *k)
{
    POBJECT obj;
    if (!NT_SUCCESS(ObCreateObject(g_key_type, sizeof(CM_KEY_OBJECT), &obj)))
        return 0;
    ((CM_KEY_OBJECT *)obj)->Key = k;
    k->OpenObjects++;

    HANDLE h;
    NTSTATUS st = ObCreateHandle(obj, GENERIC_ALL, &h);
    ObDereferenceObject(obj); /* the handle keeps the object alive */
    if (!NT_SUCCESS(st))
        return 0;
    return (UINT64)(ULONG_PTR)h;
}

/* Resolve a key handle to its CM_KEY. Handle 0 means the root (so absolute
 * paths can be opened without first opening a root key). */
static CM_KEY *cm_key_from_handle(UINT64 handle)
{
    if (handle == 0)
        return g_cm_root;
    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)handle, 0,
                                              g_key_type, &obj)))
        return NULL;
    CM_KEY *k = ((CM_KEY_OBJECT *)obj)->Key;
    if (k->Deleted) k = NULL;
    ObDereferenceObject(obj);
    return k;
}

/* ------------------------------------------------------------------ */
/* Nt* services                                                       */
/* ------------------------------------------------------------------ */

/* Resolve OBJECT_ATTRIBUTES (RootDirectory + captured name) to a target key.
 * Returns STATUS on failure; on success writes *out. */
static NTSTATUS cm_resolve_oa(POBJECT_ATTRIBUTES oa, BOOLEAN create,
                              CM_KEY **out)
{
    if (!MmProbeForRead((UINT64)oa, sizeof(OBJECT_ATTRIBUTES)))
        return STATUS_ACCESS_VIOLATION;

    /* RootDirectory (a key handle) anchors a relative name; NULL means the
     * name is absolute from the registry root. */
    CM_KEY *parent = cm_key_from_handle((UINT64)oa->RootDirectory);
    if (!parent)
        return STATUS_INVALID_HANDLE;

    char name[128];
    if (!MmCaptureUnicodeName((UINT64)oa->ObjectName, name, sizeof(name)))
        return STATUS_ACCESS_VIOLATION;

    CM_KEY *k = cm_walk(parent, name, create);
    if (!k) {
        KeLog("[cm]   key %s%s not found\n",
              create ? "" : "(open) ", name);
        return create ? STATUS_NO_MEMORY : STATUS_OBJECT_NAME_NOT_FOUND;
    }
    *out = k;
    return STATUS_SUCCESS;
}

/* NtCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, TitleIndex, Class,
 *             CreateOptions, PULONG Disposition). */
UINT64 NtCreateKey(UINT64 *a)
{
    PHANDLE out_handle = (PHANDLE)a[0];
    UINT32 *disp = (UINT32 *)a[6];
    if (!MmProbeForWrite((UINT64)out_handle, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    CM_KEY *k;
    NTSTATUS st = cm_resolve_oa((POBJECT_ATTRIBUTES)a[2], TRUE, &k);
    if (!NT_SUCCESS(st))
        return (UINT64)st;

    UINT64 h = cm_make_handle(k);
    *out_handle = (HANDLE)(ULONG_PTR)h;
    if (disp && MmProbeForWrite((UINT64)disp, sizeof(UINT32)))
        *disp = 1; /* REG_CREATED_NEW_KEY (we don't distinguish) */
    KeLog("[cm]   NtCreateKey -> handle %p\n", (void *)h);
    return (UINT64)STATUS_SUCCESS;
}

/* NtOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES). */
UINT64 NtOpenKey(UINT64 *a)
{
    PHANDLE out_handle = (PHANDLE)a[0];
    if (!MmProbeForWrite((UINT64)out_handle, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    CM_KEY *k;
    NTSTATUS st = cm_resolve_oa((POBJECT_ATTRIBUTES)a[2], FALSE, &k);
    if (!NT_SUCCESS(st))
        return (UINT64)st;

    UINT64 h = cm_make_handle(k);
    *out_handle = (HANDLE)(ULONG_PTR)h;
    KeLog("[cm]   NtOpenKey -> handle %p\n", (void *)h);
    return (UINT64)STATUS_SUCCESS;
}

/* NtDeleteKey removes a leaf from the live tree. Existing handles hold its
 * storage until their final close, but cannot resolve the deleted key. */
UINT64 NtDeleteKey(UINT64 *a)
{
    POBJECT object;
    NTSTATUS st = ObReferenceObjectByHandle((HANDLE)a[0], 0, g_key_type,
                                            &object);
    if (!NT_SUCCESS(st)) return (UINT64)st;
    CM_KEY *key = ((CM_KEY_OBJECT *)object)->Key;
    if (key->Deleted || !key->Parent) {
        st = STATUS_INVALID_HANDLE;
    } else if (!IsListEmpty(&key->SubkeyHead)) {
        st = STATUS_CANNOT_DELETE;
    } else {
        RemoveEntryList(&key->SubkeyLink);
        key->Parent->LastWriteTime = KeGetTickCount() * 100000ULL;
        key->Deleted = TRUE;
        st = STATUS_SUCCESS;
    }
    ObDereferenceObject(object);
    return (UINT64)st;
}

UINT64 NtDeleteValueKey(UINT64 *a)
{
    CM_KEY *key = cm_key_from_handle(a[0]);
    if (!key) return (UINT64)STATUS_INVALID_HANDLE;
    char name[64];
    if (!MmCaptureUnicodeName(a[1], name, sizeof(name)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    CM_VALUE *value = cm_find_value(key, name);
    if (!value) return (UINT64)STATUS_OBJECT_NAME_NOT_FOUND;
    RemoveEntryList(&value->Link);
    key->LastWriteTime = KeGetTickCount() * 100000ULL;
    if (value->Data) ExFreePool(value->Data);
    ExFreePool(value);
    return (UINT64)STATUS_SUCCESS;
}

static NTSTATUS cm_prepare_query(UINT64 output, ULONG capacity,
                                 UINT64 length_address, ULONG required)
{
    ULONG *length = (ULONG *)length_address;
    if (!MmProbeForWrite(length_address, sizeof(*length)))
        return STATUS_ACCESS_VIOLATION;
    *length = required;
    if (capacity < required) return STATUS_BUFFER_TOO_SMALL;
    if (!MmProbeForWrite(output, required)) return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* Rename the node in place so handles and descendants stay attached. */
UINT64 NtRenameKey(UINT64 *a)
{
    char name[sizeof(((CM_KEY *)0)->Name)];
    if (!MmCaptureUnicodeName(a[1], name, sizeof(name)))
        return (UINT64)STATUS_OBJECT_NAME_INVALID;
    if (!name[0]) return (UINT64)STATUS_OBJECT_NAME_INVALID;
    for (const char *p = name; *p; ++p)
        if (*p == '\\' || *p == '/') return (UINT64)STATUS_OBJECT_NAME_INVALID;

    UINT64 flags = KiIrqSave();
    CM_KEY *key = a[0] ? cm_key_from_handle(a[0]) : NULL;
    NTSTATUS status = STATUS_SUCCESS;
    if (!key) {
        status = STATUS_INVALID_HANDLE;
    } else if (!key->Parent || !key->Parent->Parent ||
               key->Parent->Parent == g_cm_root) {
        status = STATUS_ACCESS_DENIED; /* The registry roots have fixed names. */
    } else {
        CM_KEY *existing = cm_find_subkey(key->Parent, name);
        if (existing && existing != key) {
            status = STATUS_OBJECT_NAME_COLLISION;
        } else {
            memcpy(key->Name, name, strlen(name) + 1);
            key->LastWriteTime = KeGetTickCount() * 100000ULL;
            key->Parent->LastWriteTime = key->LastWriteTime;
        }
    }
    KiIrqRestore(flags);
    return (UINT64)status;
}

/* KeyBasicInformation: LastWriteTime, TitleIndex, NameLength, Name. */
UINT64 NtEnumerateKey(UINT64 *a)
{
    CM_KEY *key = cm_key_from_handle(a[0]);
    if (!key) return (UINT64)STATUS_INVALID_HANDLE;
    if (a[2] != 0) return (UINT64)STATUS_INVALID_INFO_CLASS;
    PLIST_ENTRY link = key->SubkeyHead.Flink;
    for (ULONG i = 0; i < (ULONG)a[1] && link != &key->SubkeyHead; i++)
        link = link->Flink;
    if (link == &key->SubkeyHead) return (UINT64)STATUS_NO_MORE_ENTRIES;
    CM_KEY *child = CONTAINING_RECORD(link, CM_KEY, SubkeyLink);
    ULONG chars = (ULONG)strlen(child->Name);
    ULONG needed = 16 + chars * sizeof(UINT16);
    NTSTATUS st = cm_prepare_query(a[3], (ULONG)a[4], a[5], needed);
    if (!NT_SUCCESS(st)) return (UINT64)st;
    UINT8 *out = (UINT8 *)a[3];
    memset(out, 0, 16);
    *(ULONG *)(out + 12) = chars * sizeof(UINT16);
    UINT16 *name = (UINT16 *)(out + 16);
    for (ULONG i = 0; i < chars; i++) name[i] = (BYTE)child->Name[i];
    return (UINT64)STATUS_SUCCESS;
}

/* KeyValueFullInformation: title/type/data offset/data length/name length,
 * followed by UTF-16 name and four-byte-aligned value bytes. */
UINT64 NtEnumerateValueKey(UINT64 *a)
{
    CM_KEY *key = cm_key_from_handle(a[0]);
    if (!key) return (UINT64)STATUS_INVALID_HANDLE;
    if (a[2] != 1) return (UINT64)STATUS_INVALID_INFO_CLASS;
    PLIST_ENTRY link = key->ValueHead.Flink;
    for (ULONG i = 0; i < (ULONG)a[1] && link != &key->ValueHead; i++)
        link = link->Flink;
    if (link == &key->ValueHead) return (UINT64)STATUS_NO_MORE_ENTRIES;
    CM_VALUE *value = CONTAINING_RECORD(link, CM_VALUE, Link);
    ULONG chars = (ULONG)strlen(value->Name);
    ULONG data_offset = (20 + chars * sizeof(UINT16) + 3) & ~3u;
    if (value->Size > 0xffffffffu - data_offset)
        return (UINT64)STATUS_INTEGER_OVERFLOW;
    ULONG needed = data_offset + value->Size;
    NTSTATUS st = cm_prepare_query(a[3], (ULONG)a[4], a[5], needed);
    if (!NT_SUCCESS(st)) return (UINT64)st;
    UINT8 *out = (UINT8 *)a[3];
    memset(out, 0, data_offset);
    *(ULONG *)(out + 4) = value->Type;
    *(ULONG *)(out + 8) = data_offset;
    *(ULONG *)(out + 12) = value->Size;
    *(ULONG *)(out + 16) = chars * sizeof(UINT16);
    UINT16 *name = (UINT16 *)(out + 20);
    for (ULONG i = 0; i < chars; i++) name[i] = (BYTE)value->Name[i];
    if (value->Size) memcpy(out + data_offset, value->Data, value->Size);
    return (UINT64)STATUS_SUCCESS;
}

/* KeyFullInformation reports counts and maxima in bytes, as NtQueryKey does. */
UINT64 NtQueryKey(UINT64 *a)
{
    CM_KEY *key = cm_key_from_handle(a[0]);
    if (!key) return (UINT64)STATUS_INVALID_HANDLE;
    if (a[1] != 2) return (UINT64)STATUS_INVALID_INFO_CLASS;
    const ULONG needed = 44;
    NTSTATUS st = cm_prepare_query(a[2], (ULONG)a[3], a[4], needed);
    if (!NT_SUCCESS(st)) return (UINT64)st;
    UINT8 *out = (UINT8 *)a[2];
    memset(out, 0, needed);
    memcpy(out, &key->LastWriteTime, sizeof(key->LastWriteTime));
    *(ULONG *)(out + 12) = needed; /* empty class at end of fixed header */
    for (PLIST_ENTRY link = key->SubkeyHead.Flink;
         link != &key->SubkeyHead; link = link->Flink) {
        CM_KEY *child = CONTAINING_RECORD(link, CM_KEY, SubkeyLink);
        ULONG bytes = (ULONG)strlen(child->Name) * sizeof(UINT16);
        ++*(ULONG *)(out + 20);
        if (bytes > *(ULONG *)(out + 24)) *(ULONG *)(out + 24) = bytes;
    }
    for (PLIST_ENTRY link = key->ValueHead.Flink;
         link != &key->ValueHead; link = link->Flink) {
        CM_VALUE *value = CONTAINING_RECORD(link, CM_VALUE, Link);
        ULONG bytes = (ULONG)strlen(value->Name) * sizeof(UINT16);
        ++*(ULONG *)(out + 32);
        if (bytes > *(ULONG *)(out + 36)) *(ULONG *)(out + 36) = bytes;
        if (value->Size > *(ULONG *)(out + 40))
            *(ULONG *)(out + 40) = value->Size;
    }
    return (UINT64)STATUS_SUCCESS;
}

/* NtSetValueKey(HANDLE, PUNICODE_STRING ValueName, TitleIndex, Type, Data,
 *               DataSize). */
UINT64 NtSetValueKey(UINT64 *a)
{
    CM_KEY *k = cm_key_from_handle(a[0]);
    if (!k)
        return (UINT64)STATUS_INVALID_HANDLE;

    char vname[64];
    if (!MmCaptureUnicodeName(a[1], vname, sizeof(vname)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    UINT32 type = (UINT32)a[3];
    UINT64 data = a[4];
    UINT32 size = (UINT32)a[5];
    if (size && !MmProbeForRead(data, size))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    NTSTATUS st = cm_set_value(k, vname, type, (const void *)data, size);
    KeLog("[cm]   NtSetValueKey('%s', type=%u, %u bytes) -> 0x%08x\n",
          vname, (unsigned)type, (unsigned)size, (unsigned)st);
    return (UINT64)st;
}

/* NtQueryValueKey(HANDLE, PUNICODE_STRING ValueName, InfoClass, Info, Length,
 *                 PULONG ResultLength) - returns KEY_VALUE_PARTIAL_INFORMATION. */
UINT64 NtQueryValueKey(UINT64 *a)
{
    CM_KEY *k = cm_key_from_handle(a[0]);
    if (!k)
        return (UINT64)STATUS_INVALID_HANDLE;

    char vname[64];
    if (!MmCaptureUnicodeName(a[1], vname, sizeof(vname)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    UINT64 info = a[3];
    UINT32 length = (UINT32)a[4];
    UINT32 *result_len = (UINT32 *)a[5];

    CM_VALUE *v = cm_find_value(k, vname);
    if (!v)
        return (UINT64)STATUS_OBJECT_NAME_NOT_FOUND;

    UINT32 needed = (UINT32)(sizeof(KEY_VALUE_PARTIAL_INFORMATION) - 1 + v->Size);
    if (result_len && MmProbeForWrite((UINT64)result_len, sizeof(UINT32)))
        *result_len = needed;

    if (info == 0 || length < needed) {
        if (!info || length < sizeof(KEY_VALUE_PARTIAL_INFORMATION) - 1)
            return (UINT64)STATUS_BUFFER_TOO_SMALL;
    }
    if (!MmProbeForWrite(info, length))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    KEY_VALUE_PARTIAL_INFORMATION *kvpi = (KEY_VALUE_PARTIAL_INFORMATION *)info;
    kvpi->TitleIndex = 0;
    kvpi->Type = v->Type;
    kvpi->DataLength = v->Size;
    UINT32 avail = length - (UINT32)(sizeof(KEY_VALUE_PARTIAL_INFORMATION) - 1);
    UINT32 copy = (avail < v->Size) ? avail : v->Size;
    memcpy(kvpi->Data, v->Data, copy);

    return (UINT64)STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */

void CmInitialize(void)
{
    g_key_type = ObCreateObjectType("Key", CmpKeyDelete);

    /* Anonymous super-root whose child "Registry" anchors absolute paths. */
    g_cm_root = cm_alloc_key("", NULL);

    /* Build \Registry\Machine\Software\SVINDOWS with a couple of preset values. */
    CM_KEY *svindows = cm_walk(g_cm_root, "\\Registry\\Machine\\Software\\SVINDOWS", TRUE);
    if (svindows) {
        cm_set_value(svindows, "Version", REG_SZ, "0.6.0", 6);
        UINT32 build = 600;
        cm_set_value(svindows, "BuildNumber", REG_DWORD, &build, sizeof(build));
    }
    /* An empty \Registry\User for HKEY_CURRENT_USER to hang off of. */
    cm_walk(g_cm_root, "\\Registry\\User", TRUE);

    KeLog("[cm]   registry ready: Key type + \\Registry hive\n");
}
