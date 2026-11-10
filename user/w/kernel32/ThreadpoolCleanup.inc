/* Cleanup group handles used by thread-pool clients.  Submitted callbacks
 * are tracked by their work objects; this table owns the public group handle
 * and rejects use after CloseThreadpoolCleanupGroup. */
typedef struct _THREADPOOL_CLEANUP_GROUP_LOCAL {
    volatile LONG Used;
    ULONGLONG Generation;
} THREADPOOL_CLEANUP_GROUP_LOCAL;

static THREADPOOL_CLEANUP_GROUP_LOCAL g_threadpool_cleanup_groups[32];

__declspec(dllexport) void *CreateThreadpoolCleanupGroup(void)
{
    for (DWORD i = 0; i < 32; ++i) {
        THREADPOOL_CLEANUP_GROUP_LOCAL *group = &g_threadpool_cleanup_groups[i];
        if (__sync_bool_compare_and_swap(&group->Used, 0, 1)) {
            ++group->Generation;
            if (!group->Generation)
                ++group->Generation;
            SetLastError(0);
            return group;
        }
    }
    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return 0;
}

__declspec(dllexport) void CloseThreadpoolCleanupGroup(void *handle)
{
    THREADPOOL_CLEANUP_GROUP_LOCAL *group = handle;
    if (!group || group < g_threadpool_cleanup_groups ||
        group >= g_threadpool_cleanup_groups + 32 || !group->Used) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return;
    }
    __sync_lock_test_and_set(&group->Used, 0);
    SetLastError(0);
}

__declspec(dllexport) void CloseThreadpoolCleanupGroupMembers(
    void *handle, BOOL cancel_pending, void *cleanup_context)
{
    (void)cancel_pending;
    (void)cleanup_context;
    THREADPOOL_CLEANUP_GROUP_LOCAL *group = handle;
    if (!group || group < g_threadpool_cleanup_groups ||
        group >= g_threadpool_cleanup_groups + 32 || !group->Used) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return;
    }
    /* No callback can belong to a group until the callback environment API is
     * wired to work submission.  Do not disturb unrelated global pool work. */
    SetLastError(0);
}
