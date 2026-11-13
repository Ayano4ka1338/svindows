#include "InitRuntime.h"
static void wait_for_userinit_shutdown(INIT_PROCESSINFO *userinit)
{
    if(userinit->process&&WaitForSingleObject(userinit->process,5000)!=0)
        InitStopChild(userinit,0);
}
int main(void)
{
    HANDLE singleton=0,ready=0,shutdown=0;
    INIT_PROCESSINFO userinit={0}; DWORD result=0;
    if(!InitSingleton(L"Global\\Svindows.Wininit",&singleton)) return (int)GetLastError();
    if(!InitSystem()) { result=GetLastError(); InitLog(L"wininit",L"system preparation failed",result); goto done; }
    InitLog(L"wininit",L"system directories, environment and registry initialized",1);
    HANDLE station=CreateWindowStationW(L"SvindowsServices",0,WINSTA_ALL_ACCESS,0);
    if(!station||!SetProcessWindowStation(station)) { result=GetLastError(); goto done; }
    HANDLE desktop=CreateDesktopW(L"Services",0,0,0,DESKTOP_ALL_ACCESS,0);
    if(!desktop||!SetThreadDesktop(desktop)) { result=GetLastError(); goto done; }
    InitLog(L"wininit",L"noninteractive service station attached",1);
    ready=CreateEventW(0,1,0,INIT_READY_EVENT);
    shutdown=CreateEventW(0,1,0,INIT_SHUTDOWN_EVENT);
    if(!ready||!shutdown||!ResetEvent(ready)||!ResetEvent(shutdown)) { result=GetLastError(); goto done; }
    if(!InitRegistryDword(INIT_HKLM,INIT_SESSION_KEY,L"State",1,1)||
       !InitSpawn(L"C:\\WINDOWS\\SYSTEM32\\USERINIT.EXE",L"\"C:\\WINDOWS\\SYSTEM32\\USERINIT.EXE\"",
            L"C:\\WINDOWS",L"WinSta0\\Default",&userinit)) { result=GetLastError(); goto done; }
    InitLog(L"wininit",L"interactive userinit started",userinit.pid);
    HANDLE startup_wait[3]={ready,userinit.process,shutdown};
    DWORD wait=WaitForMultipleObjects(3,startup_wait,0,30000);
    if(wait==0) {
        InitLog(L"wininit",L"interactive session ready",1);
        HANDLE running_wait[2]={userinit.process,shutdown};
        wait=WaitForMultipleObjects(2,running_wait,0,INIT_INFINITE);
        if(wait==1) { InitLog(L"wininit",L"session shutdown requested",1); wait_for_userinit_shutdown(&userinit); }
        else if(wait==0) GetExitCodeProcess(userinit.process,&result);
        else result=GetLastError();
    } else if(wait==1) GetExitCodeProcess(userinit.process,&result);
    else if(wait==2) wait_for_userinit_shutdown(&userinit);
    else { result=wait==0x102?1460:GetLastError(); InitStopChild(&userinit,result); }
done:
    if(result) InitLog(L"wininit",L"session coordinator failed",result);
    if(ready) { ResetEvent(ready); CloseHandle(ready); }
    if(shutdown) CloseHandle(shutdown);
    InitCloseProcess(&userinit);
    if(singleton) { ReleaseMutex(singleton); CloseHandle(singleton); }
    return (int)result;
}
