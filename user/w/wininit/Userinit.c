#include "InitRuntime.h"
int main(void)
{
    HANDLE singleton=0,ready=0,shell_ready=0,shutdown=0;
    INIT_PROCESSINFO shell={0}; DWORD result=0,failures=0;
    WCHAR profile[260],shell_command[768];
    if(!InitSingleton(L"Local\\Svindows.Userinit",&singleton)) return (int)GetLastError();
    HANDLE station=OpenWindowStationW(L"WinSta0",0,WINSTA_ALL_ACCESS);
    if(!station||!SetProcessWindowStation(station)) { result=GetLastError(); goto done; }
    HANDLE desktop=OpenDesktopW(L"Default",0,0,DESKTOP_ALL_ACCESS);
    if(!desktop||!SetThreadDesktop(desktop)||!SwitchDesktop(desktop)) { result=GetLastError(); goto done; }
    if(!InitProfile(profile)) { result=GetLastError(); InitLog(L"userinit",L"profile initialization failed",result); goto done; }
    InitLog(L"userinit",L"profile environment and shell folders prepared",1);
    ready=CreateEventW(0,1,0,INIT_READY_EVENT);
    shell_ready=CreateEventW(0,1,0,INIT_SHELL_READY_EVENT);
    shutdown=CreateEventW(0,1,0,INIT_SHUTDOWN_EVENT);
    if(!ready||!shell_ready||!shutdown) { result=GetLastError(); goto done; }
    for(;;) {
        if(WaitForSingleObject(shutdown,0)==0) break;
        if(!InitReadString(INIT_HKLM,INIT_WINLOGON_KEY,L"Shell",L"C:\\WINDOWS\\EXPLORER.EXE",shell_command,768)||
           !ResetEvent(ready)||!ResetEvent(shell_ready)||!InitSessionState(1,0,0)) { result=GetLastError(); break; }
        DWORD started=GetTickCount();
        if(!InitSpawn(0,shell_command,profile,L"WinSta0\\Default",&shell)) { result=GetLastError(); break; }
        InitLog(L"userinit",L"shell process started",shell.pid);
        HANDLE startup_wait[3]={shell_ready,shell.process,shutdown};
        DWORD wait=WaitForMultipleObjects(3,startup_wait,0,30000);
        if(wait==0) {
            DWORD exit_code=0;
            if(!GetExitCodeProcess(shell.process,&exit_code)||exit_code!=259||
               !InitSessionState(2,shell.pid,0)||!SetEvent(ready)) { result=GetLastError(); break; }
            InitLog(L"userinit",L"shell desktop and taskbar ready",shell.pid);
            HANDLE running_wait[2]={shell.process,shutdown};
            wait=WaitForMultipleObjects(2,running_wait,0,INIT_INFINITE);
            if(wait==1) { InitStopChild(&shell,0); break; }
            if(wait!=0) { result=GetLastError(); break; }
        } else if(wait==2) { InitStopChild(&shell,0); break; }
        else if(wait!=1) { result=wait==0x102?1460:GetLastError(); InitStopChild(&shell,result); break; }
        DWORD exit_code=0; GetExitCodeProcess(shell.process,&exit_code);
        InitLog(L"userinit",L"shell process exited",exit_code);
        ResetEvent(ready); InitSessionState(3,0,exit_code); InitCloseProcess(&shell);
        if(!InitReadDword(INIT_HKLM,INIT_WINLOGON_KEY,L"AutoRestartShell",1)) { result=exit_code; break; }
        if(GetTickCount()-started<5000) failures++; else failures=0;
        DWORD limit=InitReadDword(INIT_HKLM,INIT_WINLOGON_KEY,L"ShellRestartLimit",5);
        if(failures>=limit) { result=1067; break; }
        DWORD delay=InitReadDword(INIT_HKLM,INIT_WINLOGON_KEY,L"ShellRestartDelay",250);
        if(delay>30000) delay=30000;
        if(WaitForSingleObject(shutdown,delay)==0) break;
    }
done:
    if(shell.process) InitStopChild(&shell,result);
    InitCloseProcess(&shell); InitSessionState(result?4:0,0,result);
    if(result) InitLog(L"userinit",L"interactive session failed",result);
    if(ready) { ResetEvent(ready); CloseHandle(ready); }
    if(shell_ready) CloseHandle(shell_ready);
    if(shutdown) CloseHandle(shutdown);
    if(singleton) { ReleaseMutex(singleton); CloseHandle(singleton); }
    return (int)result;
}
