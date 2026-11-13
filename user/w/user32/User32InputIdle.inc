__declspec(dllimport) UINT64 SvNtUserWaitForInputIdle(HANDLE process,DWORD timeout);
USER32_API DWORD WaitForInputIdle(HANDLE process,DWORD milliseconds)
{
    return (DWORD)SvNtUserWaitForInputIdle(process,milliseconds);
}
