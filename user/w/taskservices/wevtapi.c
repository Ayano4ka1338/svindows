typedef unsigned long DWORD;
__declspec(dllimport) void SetLastError(DWORD);
__declspec(dllexport) void *EvtSubscribe(void *session, void *event,
    const unsigned short *channel, const unsigned short *query, void *bookmark,
    void *context, void *callback, DWORD flags)
{
    (void)session; (void)event; (void)channel; (void)query; (void)bookmark;
    (void)context; (void)callback; (void)flags;
    SetLastError(120); /* No event-log subscription service is installed. */
    return 0;
}
__declspec(dllexport) int EvtClose(void *handle)
{
    (void)handle;
    SetLastError(6); /* No event-log handles have been issued. */
    return 0;
}
