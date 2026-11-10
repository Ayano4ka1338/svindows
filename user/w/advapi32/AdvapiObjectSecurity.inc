/* The object manager does not store or enforce per-object ACLs yet.
 * Report the unavailable operation instead of returning an invented ACL. */
#define SECURITY_NOT_SUPPORTED 50u

__declspec(dllexport) DWORD GetSecurityInfo(HANDLE object, DWORD object_type,
    DWORD information, void **owner, void **group, void **dacl, void **sacl,
    void **descriptor)
{
    (void)object;
    (void)object_type;
    (void)information;
    if (owner) *owner = 0;
    if (group) *group = 0;
    if (dacl) *dacl = 0;
    if (sacl) *sacl = 0;
    if (descriptor) *descriptor = 0;
    return SECURITY_NOT_SUPPORTED;
}

__declspec(dllexport) DWORD GetNamedSecurityInfoW(const WCHAR *name,
    DWORD object_type, DWORD information, void **owner, void **group,
    void **dacl, void **sacl, void **descriptor)
{
    (void)name;
    return GetSecurityInfo(0, object_type, information, owner, group, dacl,
                           sacl, descriptor);
}

__declspec(dllexport) DWORD SetSecurityInfo(HANDLE object, DWORD object_type,
    DWORD information, void *owner, void *group, void *dacl, void *sacl)
{
    (void)object;
    (void)object_type;
    (void)information;
    (void)owner;
    (void)group;
    (void)dacl;
    (void)sacl;
    return SECURITY_NOT_SUPPORTED;
}

__declspec(dllexport) DWORD SetNamedSecurityInfoW(const WCHAR *name,
    DWORD object_type, DWORD information, void *owner, void *group,
    void *dacl, void *sacl)
{
    (void)name;
    return SetSecurityInfo(0, object_type, information, owner, group, dacl, sacl);
}

__declspec(dllexport) LONG RegSetKeySecurity(HKEY key, DWORD information,
    const void *descriptor)
{
    (void)key;
    (void)information;
    (void)descriptor;
    return SECURITY_NOT_SUPPORTED;
}

typedef struct {
    DWORD Read, Write, Execute, All;
} GENERIC_MAPPING;

__declspec(dllexport) void MapGenericMask(DWORD *mask, const GENERIC_MAPPING *map)
{
    DWORD input = *mask;
    DWORD mapped = input & 0x0fffffffu;
    if (input & 0x80000000u) mapped |= map->Read;
    if (input & 0x40000000u) mapped |= map->Write;
    if (input & 0x20000000u) mapped |= map->Execute;
    if (input & 0x10000000u) mapped |= map->All;
    *mask = mapped & 0x0fffffffu;
}

__declspec(dllexport) DWORD GetInheritanceSourceW(const WCHAR *name,
    DWORD object_type, DWORD information, BOOL_K container,
    const void *class_guids, DWORD guid_count, const void *acl,
    const void *object_functions, const GENERIC_MAPPING *map, void *sources)
{
    (void)name;
    (void)object_type;
    (void)information;
    (void)container;
    (void)class_guids;
    (void)guid_count;
    (void)acl;
    (void)object_functions;
    (void)map;
    (void)sources;
    return SECURITY_NOT_SUPPORTED;
}
