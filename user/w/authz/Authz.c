/* Authorization services are optional. Native token/DACL evaluation is not
 * implemented yet; applications receive failure and never an access grant. */
typedef unsigned long DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef unsigned short WCHAR;
typedef struct { DWORD Low; long High; } LUID;
__declspec(dllimport) void SetLastError(DWORD);

__declspec(dllexport) BOOL AuthzInitializeResourceManager(DWORD flags,
    void *access_check, void *compute_groups, void *free_groups,
    const WCHAR *name, HANDLE *manager)
{
    (void)flags; (void)access_check; (void)compute_groups;
    (void)free_groups; (void)name;
    if (!manager) { SetLastError(87); return 0; }
    *manager = 0;
    SetLastError(50);
    return 0;
}
__declspec(dllexport) BOOL AuthzInitializeContextFromSid(DWORD flags,
    void *sid, HANDLE manager, const long long *expires, LUID identifier,
    void *groups, HANDLE *context)
{
    (void)flags; (void)sid; (void)manager; (void)expires;
    (void)identifier; (void)groups;
    if (!context) { SetLastError(87); return 0; }
    *context = 0;
    SetLastError(50);
    return 0;
}
__declspec(dllexport) BOOL AuthzAccessCheck(DWORD flags, HANDLE context,
    void *request, HANDLE audit, void *descriptor, void **optional,
    DWORD optional_count, void *reply, HANDLE *results)
{
    (void)flags; (void)context; (void)request; (void)audit;
    (void)descriptor; (void)optional; (void)optional_count; (void)reply;
    if (results) *results = 0;
    SetLastError(50);
    return 0;
}
__declspec(dllexport) BOOL AuthzFreeResourceManager(HANDLE manager)
{ (void)manager; SetLastError(6); return 0; }
__declspec(dllexport) BOOL AuthzFreeContext(HANDLE context)
{ (void)context; SetLastError(6); return 0; }
