/* No native wait-chain traversal service is exposed yet. */
__declspec(dllexport) HANDLE OpenThreadWaitChainSession(DWORD flags, void *callback)
{
    (void)flags; (void)callback;
    SetLastError(50); return 0;
}
__declspec(dllexport) BOOL_K GetThreadWaitChain(HANDLE session, ULONGLONG context,
    DWORD flags, DWORD thread, DWORD *count, void *nodes, BOOL_K *cycle)
{
    (void)session; (void)context; (void)flags; (void)thread; (void)nodes;
    if (count) *count=0;
    if (cycle) *cycle=FALSE_K;
    SetLastError(50); return FALSE_K;
}
__declspec(dllexport) void CloseThreadWaitChainSession(HANDLE session)
{ (void)session; SetLastError(6); }
