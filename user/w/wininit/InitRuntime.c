#include "InitRuntime.h"

static DWORD length(const WCHAR *text) { DWORD n=0; while(text&&text[n]) n++; return n; }
BOOL InitCopy(WCHAR *out,DWORD chars,const WCHAR *text)
{
    DWORD n=length(text); if(!out||!chars||n>=chars) { SetLastError(206); return 0; }
    for(DWORD i=0;i<=n;i++) out[i]=text?text[i]:0; return 1;
}
BOOL InitAppend(WCHAR *out,DWORD chars,const WCHAR *text)
{
    DWORD n=length(out); return n<chars&&InitCopy(out+n,chars-n,text);
}
void InitLog(const WCHAR *role,const WCHAR *message,DWORD value)
{
    WCHAR text[512],number[16]; DWORD used=0;
    do { number[used++]=(WCHAR)('0'+value%10); value/=10; } while(value);
    if(!InitCopy(text,512,L"[")||!InitAppend(text,512,role)||!InitAppend(text,512,L"] ")||
       !InitAppend(text,512,message)||!InitAppend(text,512,L" ")) return;
    DWORD n=length(text); while(used) text[n++]=number[--used]; text[n++]= '\n'; text[n]=0;
    OutputDebugStringW(text);
}
BOOL InitDirectory(const WCHAR *path)
{
    if(!path||!path[0]||path[1]!=':'||(path[2]!='\\'&&path[2]!='/')) { SetLastError(267); return 0; }
    WCHAR partial[260]; if(!InitCopy(partial,260,path)) return 0;
    DWORD n=length(partial);
    for(DWORD i=3;i<=n;i++) if(partial[i]=='\\'||partial[i]=='/'||!partial[i]) {
        WCHAR saved=partial[i]; partial[i]=0;
        DWORD flags=GetFileAttributesW(partial);
        if(flags==(DWORD)-1) {
            if(!CreateDirectoryW(partial,0)) return 0;
        } else if(!(flags&0x10u)) { SetLastError(267); return 0; }
        partial[i]=saved;
    }
    return 1;
}
static BOOL registry_value(HKEY root,const WCHAR *path,const WCHAR *name,
    const void *data,DWORD bytes,DWORD type,BOOL replace)
{
    HKEY key=0; DWORD disposition;
    LONG status=RegCreateKeyExW(root,path,0,0,0,0x2001Bu,0,&key,&disposition);
    if(status) { SetLastError((DWORD)status); return 0; }
    if(!replace) {
        DWORD existing_type=0,size=0;
        status=RegQueryValueExW(key,name,0,&existing_type,0,&size);
        if(!status) { RegCloseKey(key); return 1; }
        if(status!=2) { RegCloseKey(key); SetLastError((DWORD)status); return 0; }
    }
    status=RegSetValueExW(key,name,0,type,data,bytes); RegCloseKey(key);
    if(status) { SetLastError((DWORD)status); return 0; } return 1;
}
BOOL InitRegistryString(HKEY root,const WCHAR *key,const WCHAR *name,const WCHAR *value,BOOL replace,DWORD type)
{ return registry_value(root,key,name,value,(length(value)+1)*2,type,replace); }
BOOL InitRegistryDword(HKEY root,const WCHAR *key,const WCHAR *name,DWORD value,BOOL replace)
{ return registry_value(root,key,name,&value,4,4,replace); }
DWORD InitReadDword(HKEY root,const WCHAR *path,const WCHAR *name,DWORD fallback)
{
    HKEY key; DWORD value=0,type=0,bytes=4;
    if(RegOpenKeyExW(root,path,0,0x20019,&key)) return fallback;
    LONG status=RegQueryValueExW(key,name,0,&type,(BYTE *)&value,&bytes); RegCloseKey(key);
    return !status&&type==4&&bytes==4?value:fallback;
}
BOOL InitReadString(HKEY root,const WCHAR *path,const WCHAR *name,const WCHAR *fallback,WCHAR *out,DWORD chars)
{
    HKEY key; DWORD type=0,bytes=chars*2;
    LONG status=RegOpenKeyExW(root,path,0,0x20019,&key);
    if(!status) { status=RegQueryValueExW(key,name,0,&type,(BYTE *)out,&bytes); RegCloseKey(key); }
    if(status==2) return InitCopy(out,chars,fallback);
    if(status) { SetLastError((DWORD)status); return 0; }
    if((type!=1&&type!=2)||!bytes||(bytes&1)||bytes>chars*2||out[bytes/2-1]) { SetLastError(13); return 0; }
    if(type==2) {
        WCHAR expanded[260]; DWORD needed=ExpandEnvironmentStringsW(out,expanded,260);
        if(!needed||needed>260) return 0; return InitCopy(out,chars,expanded);
    }
    return 1;
}
static BOOL shortcut(const WCHAR *path,const char *target)
{
    HANDLE file=CreateFileW(path,0x40000000u,1,0,1,0x80,0); /* CREATE_NEW preserves user files */
    if(file==(HANDLE)(UINT_PTR)-1) return GetLastError()==80||GetLastError()==183;
    static const char prefix[]="[InternetShortcut]\r\nURL=";
    DWORD done=0,bytes=0; while(target[bytes]) bytes++;
    BOOL ok=WriteFile(file,prefix,sizeof(prefix)-1,&done,0)&&done==sizeof(prefix)-1;
    if(ok) ok=WriteFile(file,target,bytes,&done,0)&&done==bytes;
    if(ok) ok=WriteFile(file,"\r\n",2,&done,0)&&done==2;
    if(ok) ok=FlushFileBuffers(file);
    DWORD error=GetLastError(); CloseHandle(file); if(!ok) SetLastError(error); return ok;
}
BOOL InitSystem(void)
{
    static const WCHAR *directories[]={L"C:\\WINDOWS",L"C:\\WINDOWS\\SYSTEM32",L"C:\\WINDOWS\\TEMP",
        L"C:\\Users",L"C:\\Program Files",L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup",
        L"C:\\Users\\Public\\Desktop",L"C:\\Users\\Public\\Documents",L"C:\\Users\\Public\\Music",
        L"C:\\Users\\Public\\Pictures",L"C:\\Users\\Public\\Videos"};
    for(DWORD i=0;i<sizeof(directories)/sizeof(directories[0]);i++) if(!InitDirectory(directories[i])) return 0;
    static const struct {const WCHAR *name,*value;} environment[]={
        {L"SystemDrive",L"C:"},{L"SystemRoot",L"C:\\WINDOWS"},{L"windir",L"C:\\WINDOWS"},
        {L"ComSpec",L"C:\\CMD.EXE"},{L"ProgramFiles",L"C:\\Program Files"},
        {L"ProgramData",L"C:\\ProgramData"},{L"ALLUSERSPROFILE",L"C:\\ProgramData"},
        {L"PUBLIC",L"C:\\Users\\Public"},
        {L"TEMP",L"C:\\WINDOWS\\TEMP"},{L"TMP",L"C:\\WINDOWS\\TEMP"},
        {L"Path",L"C:\\WINDOWS\\SYSTEM32;C:\\WINDOWS;C:\\WINDOWS7;C:\\;C:\\USR\\BIN"},
        {L"PATHEXT",L".COM;.EXE;.BAT;.CMD;.ELF"},{L"OS",L"Windows_NT"}};
    for(DWORD i=0;i<sizeof(environment)/sizeof(environment[0]);i++)
        if(!SetEnvironmentVariableW(environment[i].name,environment[i].value)) return 0;
    if(!InitRegistryString(INIT_HKLM,INIT_WINLOGON_KEY,L"Shell",L"C:\\WINDOWS\\EXPLORER.EXE",0,1)||
       !InitRegistryDword(INIT_HKLM,INIT_WINLOGON_KEY,L"AutoRestartShell",1,0)||
       !InitRegistryDword(INIT_HKLM,INIT_WINLOGON_KEY,L"ShellRestartLimit",5,0)||
       !InitRegistryDword(INIT_HKLM,INIT_WINLOGON_KEY,L"ShellRestartDelay",250,0)||
       !InitRegistryString(INIT_HKLM,L"SOFTWARE\\Svindows\\Winlogon",L"UserName",L"User",0,1)) return 0;
    if(GetFileAttributesW(L"C:\\WINDOWS7\\NOTEPAD.EXE")!=(DWORD)-1) {
        static const WCHAR *extensions[]={L".txt",L".log",L".ini"};
        for(DWORD i=0;i<3;i++) if(!InitRegistryString(INIT_HKCR,extensions[i],0,L"txtfile",0,1)) return 0;
        if(!InitRegistryString(INIT_HKCR,L"txtfile",0,L"Text Document",0,1)||
           !InitRegistryString(INIT_HKCR,L"txtfile\\shell\\open\\command",0,
             L"\"C:\\WINDOWS7\\NOTEPAD.EXE\" \"%1\"",0,1)) return 0;
    }
    if(!InitRegistryString(INIT_HKCR,L"exefile\\shell\\open\\command",0,L"\"%1\" %*",0,1)||
       !InitRegistryString(INIT_HKCR,L".exe",0,L"exefile",0,1)||
       !InitRegistryString(INIT_HKCR,L".bat",0,L"batfile",0,1)||
       !InitRegistryString(INIT_HKCR,L".cmd",0,L"batfile",0,1)||
       !InitRegistryString(INIT_HKCR,L"batfile\\shell\\open\\command",0,L"\"C:\\CMD.EXE\" /c \"%1\" %*",0,1)||
       !InitRegistryString(INIT_HKCR,L"Folder\\shell\\open\\command",0,L"\"C:\\WINDOWS\\EXPLORER.EXE\" /folder \"%1\"",0,1)||
       !InitRegistryString(INIT_HKCR,L"Directory\\shell\\open\\command",0,L"\"C:\\WINDOWS\\EXPLORER.EXE\" /folder \"%1\"",0,1)) return 0;
    static const struct {const WCHAR *name,*path;} common_folders[]={
        {L"Common AppData",L"C:\\ProgramData"},
        {L"Common Desktop",L"C:\\Users\\Public\\Desktop"},
        {L"Common Documents",L"C:\\Users\\Public\\Documents"},
        {L"CommonMusic",L"C:\\Users\\Public\\Music"},
        {L"CommonPictures",L"C:\\Users\\Public\\Pictures"},
        {L"CommonVideo",L"C:\\Users\\Public\\Videos"},
        {L"Common Start Menu",L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu"},
        {L"Common Programs",L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs"},
        {L"Common Startup",L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"}};
    for(DWORD i=0;i<sizeof(common_folders)/sizeof(common_folders[0]);i++)
        if(!InitRegistryString(INIT_HKLM,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
            common_folders[i].name,common_folders[i].path,0,1)) return 0;
    return SetCurrentDirectoryW(L"C:\\WINDOWS");
}
BOOL InitProfile(WCHAR profile[260])
{
    WCHAR username[64];
    if(!InitReadString(INIT_HKLM,L"SOFTWARE\\Svindows\\Winlogon",L"UserName",L"User",username,64)) return 0;
    if(!username[0]) { SetLastError(1315); return 0; }
    for(DWORD i=0;username[i];i++)
        if(username[i]<32||username[i]=='\\'||username[i]=='/'||username[i]==':'||username[i]=='*'||
           username[i]=='?'||username[i]=='"'||username[i]=='<'||username[i]=='>'||username[i]=='|') { SetLastError(1315); return 0; }
    if(!InitCopy(profile,260,L"C:\\Users\\")||!InitAppend(profile,260,username)) return 0;
    HANDLE token=0; union {UINT_PTR alignment; BYTE bytes[256];} information;
    DWORD information_bytes=0; WCHAR *sid=0;
    if(!OpenProcessToken(GetCurrentProcess(),8,&token)) return 0;
    BOOL identity=GetTokenInformation(token,1,information.bytes,sizeof(information),&information_bytes)&&
                  ConvertSidToStringSidW(*(void **)information.bytes,&sid);
    DWORD identity_error=GetLastError(); CloseHandle(token);
    if(!identity) { SetLastError(identity_error); return 0; }
    WCHAR profile_key[256],configured[260];
    BOOL configured_profile=InitCopy(profile_key,256,L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\")&&
        InitAppend(profile_key,256,sid)&&
        InitReadString(INIT_HKLM,profile_key,L"ProfileImagePath",profile,configured,260)&&
        InitCopy(profile,260,configured)&&
        InitRegistryString(INIT_HKLM,profile_key,L"ProfileImagePath",profile,0,2)&&
        InitRegistryString(INIT_HKLM,profile_key,L"AccountName",username,0,1)&&
        InitReadString(INIT_HKLM,profile_key,L"AccountName",username,username,64);
    identity_error=GetLastError(); LocalFree((HANDLE)sid);
    if(!configured_profile) { SetLastError(identity_error); return 0; }
    if(!InitDirectory(profile)) return 0;
    if(!SetEnvironmentVariableW(L"USERNAME",username)||!SetEnvironmentVariableW(L"USERPROFILE",profile)||
       !SetEnvironmentVariableW(L"HOMEDRIVE",L"C:")||!SetEnvironmentVariableW(L"HOMEPATH",profile+2)) return 0;
    static const struct {const WCHAR *suffix,*registry,*environment;} folders[]={
        {L"\\Desktop",L"Desktop",0},{L"\\Documents",L"Personal",0},{L"\\Downloads",L"Downloads",0},
        {L"\\AppData\\Roaming",L"AppData",L"APPDATA"},{L"\\AppData\\Local",L"Local AppData",L"LOCALAPPDATA"},
        {L"\\AppData\\Local\\Temp",0,L"TEMP"},
        {L"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu",L"Start Menu",0},
        {L"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs",L"Programs",0},
        {L"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup",L"Startup",0}};
    WCHAR path[260],expanded[260];
    for(DWORD i=0;i<sizeof(folders)/sizeof(folders[0]);i++) {
        if(!InitCopy(path,260,profile)||!InitAppend(path,260,folders[i].suffix)||!InitDirectory(path)) return 0;
        if(folders[i].environment&&!SetEnvironmentVariableW(folders[i].environment,path)) return 0;
        if(folders[i].registry) {
            if(!InitCopy(expanded,260,L"%USERPROFILE%")||!InitAppend(expanded,260,folders[i].suffix)||
               !InitRegistryString(INIT_HKCU,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
                    folders[i].registry,path,1,1)||
               !InitRegistryString(INIT_HKCU,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                    folders[i].registry,expanded,1,2)) return 0;
        }
    }
    if(!GetEnvironmentVariableW(L"TEMP",path,260)||!SetEnvironmentVariableW(L"TMP",path)) return 0;
    static const struct {const WCHAR *executable,*label;const char *url;} apps[]={
        {L"C:\\WINDOWS7\\NOTEPAD.EXE",L"Notepad.url","file:///C:/WINDOWS7/NOTEPAD.EXE"},
        {L"C:\\WINDOWS7\\CALC.EXE",L"Calculator.url","file:///C:/WINDOWS7/CALC.EXE"},
        {L"C:\\WINDOWS7\\REGEDIT.EXE",L"Registry Editor.url","file:///C:/WINDOWS7/REGEDIT.EXE"}};
    for(DWORD i=0;i<3;i++) if(GetFileAttributesW(apps[i].executable)!=(DWORD)-1) {
        if(!InitCopy(path,260,profile)||!InitAppend(path,260,L"\\Desktop\\")||!InitAppend(path,260,apps[i].label)||
           !shortcut(path,apps[i].url)) return 0;
    }
    return SetCurrentDirectoryW(profile);
}
BOOL InitSpawn(const WCHAR *application,const WCHAR *command,const WCHAR *directory,
    const WCHAR *desktop,INIT_PROCESSINFO *process)
{
    INIT_STARTUPINFO startup={0}; startup.cb=sizeof(startup); startup.desktop=(WCHAR *)desktop;
    WCHAR line[768]; if(!InitCopy(line,768,command)) return 0;
    BOOL result=CreateProcessW(application,line,0,0,0,0,0,directory,&startup,process);
    if(result&&process->thread) { CloseHandle(process->thread); process->thread=0; }
    return result;
}
void InitCloseProcess(INIT_PROCESSINFO *process)
{
    if(process->thread) CloseHandle(process->thread);
    if(process->process) CloseHandle(process->process);
    process->thread=process->process=0;
}
BOOL InitStopChild(INIT_PROCESSINFO *process,DWORD code)
{
    if(!process->process) return 1;
    DWORD exit_code;
    if(!GetExitCodeProcess(process->process,&exit_code)) return 0;
    if(exit_code==259&&!TerminateProcess(process->process,code)) return 0;
    return WaitForSingleObject(process->process,5000)==0;
}
BOOL InitSessionState(DWORD state,DWORD pid,DWORD exit_code)
{
    return InitRegistryDword(INIT_HKLM,INIT_SESSION_KEY,L"State",state,1)&&
           InitRegistryDword(INIT_HKLM,INIT_SESSION_KEY,L"ShellProcessId",pid,1)&&
           InitRegistryDword(INIT_HKLM,INIT_SESSION_KEY,L"LastExitCode",exit_code,1);
}
BOOL InitSingleton(const WCHAR *name,HANDLE *mutex)
{
    *mutex=CreateMutexW(0,0,name); if(!*mutex) return 0;
    DWORD wait=WaitForSingleObject(*mutex,0);
    if(wait==0||wait==0x80) return 1;
    DWORD error=wait==0x102?183:GetLastError(); CloseHandle(*mutex); *mutex=0; SetLastError(error); return 0;
}
