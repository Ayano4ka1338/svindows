/* Association/launch contract tests use the production shell code and real
 * user heap engine. Registry/file/process endpoints are observable fixtures. */
#include "UserRuntimeHost.h"
#include "../../user/w/ntdll/Rtl/CriticalSections.inc"
#include "../../user/w/ntdll/Rtl/Heap.inc"
#define NT_INVALID_HANDLE ((HANDLE)-1LL)
#define HEAP_ZERO_MEMORY 8u
#include "../../user/w/kernel32/HeapApi.inc"
#include "../../user/w/kernel32/GlobalMemory.inc"
#include "../../user/w/kernel32/LocalMemory.inc"
#define MAX_PATH 260
static DWORD GetLastError(void){return host_last_error;}
typedef struct {
    DWORD cbSize,fMask;void *hwnd;
    const WCHAR *lpVerb,*lpFile,*lpParameters,*lpDirectory;
    int nShow;HANDLE hInstApp;void *lpIDList;const WCHAR *lpClass;HANDLE hkeyClass;
    DWORD dwHotKey;union{HANDLE hIcon;HANDLE hMonitor;}DUMMYUNIONNAME;HANDLE hProcess;
} SHELLEXECUTEINFOW;
typedef void *HWND;
#include "../../user/w/shell32/CommandLine.inc"
static unsigned allocation_attempts,allocation_fail_at;
static void *shell_heap_alloc(HANDLE heap,DWORD flags,SIZE_T bytes)
{if(++allocation_attempts==allocation_fail_at){SetLastError(8);return 0;}return HeapAlloc(heap,flags,bytes);}
static void *shell_heap_realloc(HANDLE heap,DWORD flags,void *memory,SIZE_T bytes)
{if(++allocation_attempts==allocation_fail_at){SetLastError(8);return 0;}return HeapReAlloc(heap,flags,memory,bytes);}
#define HeapAlloc shell_heap_alloc
#define HeapReAlloc shell_heap_realloc
#include "../../user/w/shell32/ShellExecute.inc"
#undef HeapAlloc
#undef HeapReAlloc
typedef NTSTATUS HRESULT;
#define S_OK ((HRESULT)0)
static WCHAR *copy_path(WCHAR *out,const WCHAR *path)
{unsigned i=0;while(path[i]&&i+1<MAX_PATH){out[i]=path[i];i++;}out[i]=0;return path[i]?0:out;}
typedef struct {DWORD Data1;WORD Data2,Data3;BYTE Data4[8];} SHELL_GUID;
HRESULT SHCreateItemFromParsingName(const WCHAR *,void *,const SHELL_GUID *,void **);
BOOL CreateDirectoryW(const WCHAR *,void *);
#include "../../user/w/shell32/SpecialFolders.inc"
#include "../../user/w/shell32/FilesystemNamespace.inc"

static size_t host_wlength(const WCHAR *text){size_t n=0;if(text)while(text[n])n++;return n;}
static int host_wcompare(const WCHAR *a,const WCHAR *b)
{
    while(*a && *b){WCHAR x=*a++,y=*b++;if(x>='A' && x<='Z')x+=32;if(y>='A' && y<='Z')y+=32;if(x!=y)return x<y?-1:1;}
    return *a?1:*b?-1:0;
}
static WCHAR *host_wcopy(const WCHAR *text)
{size_t n=host_wlength(text)+1;WCHAR *out=malloc(n*sizeof(*out));assert(out);memcpy(out,text,n*sizeof(*out));return out;}
int CompareStringOrdinal(const WCHAR *a,int an,const WCHAR *b,int bn,BOOL ignore)
{
    assert(an==-1 && bn==-1 && ignore);int result=host_wcompare(a,b);return result<0?1:result>0?3:2;
}
typedef struct {const WCHAR *Name,*Value;} HOST_ENV;
static const HOST_ENV host_environment[]={
    {L"APPS",L"C:\\Registered Apps"},{L"PATH",L"C:\\Tools;C:\\WINDOWS\\SYSTEM32"},
    {L"COMSPEC",L"C:\\CMD.EXE"},{L"USERPROFILE",L"C:\\Users\\Alex"},{L"EMPTY",L""},
    {L"PUBLIC",L"C:\\Users\\Public"},{L"PROGRAMDATA",L"C:\\ProgramData"}
};
static const WCHAR *host_env_value(const WCHAR *name)
{for(unsigned i=0;i<sizeof(host_environment)/sizeof(*host_environment);++i)if(!host_wcompare(name,host_environment[i].Name))return host_environment[i].Value;return 0;}
DWORD GetEnvironmentVariableW(const WCHAR *name,WCHAR *out,DWORD capacity)
{
    const WCHAR *value=host_env_value(name);if(!value){SetLastError(203);return 0;}
    size_t n=host_wlength(value);SetLastError(0);if(!n){if(out && capacity)out[0]=0;return 0;}
    if(!out || capacity<=n)return (DWORD)n+1;memcpy(out,value,(n+1)*sizeof(*out));return (DWORD)n;
}
static unsigned env_allocated,env_freed;
WCHAR *GetEnvironmentStringsW(void)
{
    size_t n=1;for(unsigned i=0;i<sizeof(host_environment)/sizeof(*host_environment);++i)n+=host_wlength(host_environment[i].Name)+host_wlength(host_environment[i].Value)+2;
    WCHAR *out=calloc(n,sizeof(*out));assert(out);size_t pos=0;
    for(unsigned i=0;i<sizeof(host_environment)/sizeof(*host_environment);++i){
        const HOST_ENV *env=host_environment+i;size_t a=host_wlength(env->Name),b=host_wlength(env->Value);
        memcpy(out+pos,env->Name,a*sizeof(*out));pos+=a;out[pos++]='=';
        memcpy(out+pos,env->Value,b*sizeof(*out));pos+=b+1;
    }
    env_allocated++;return out;
}
BOOL FreeEnvironmentStringsW(WCHAR *source){free(source);env_freed++;return TRUE;}
DWORD ExpandEnvironmentStringsW(const WCHAR *input,WCHAR *out,DWORD capacity)
{
    WCHAR buffer[8192];size_t pos=0;
    for(size_t i=0;input[i];++i) {
        if(input[i]=='%') {
            size_t end=i+1;while(input[end] && input[end]!='%')end++;
            if(input[end]=='%') {
                WCHAR name[256];size_t length=end-i-1;assert(length<256);
                memcpy(name,input+i+1,length*sizeof(*name));name[length]=0;
                const WCHAR *value=host_env_value(name);
                if(value){size_t n=host_wlength(value);assert(pos+n<8192);memcpy(buffer+pos,value,n*sizeof(*buffer));pos+=n;i=end;continue;}
            }
        }
        assert(pos+1<8192);buffer[pos++]=input[i];
    }
    buffer[pos]=0;if(out && capacity>pos)memcpy(out,buffer,(pos+1)*sizeof(*out));return (DWORD)pos+1;
}
DWORD GetCurrentDirectoryW(DWORD capacity,WCHAR *out)
{const WCHAR *cwd=L"C:\\Work";size_t n=host_wlength(cwd);if(!out || capacity<=n)return (DWORD)n+1;memcpy(out,cwd,(n+1)*sizeof(*out));return (DWORD)n;}
DWORD GetModuleFileNameW(HANDLE module,WCHAR *out,DWORD capacity)
{assert(!module);const WCHAR *path=L"C:\\Tests\\ShellExecuteChecks.exe";size_t n=host_wlength(path);if(capacity<=n)return capacity;memcpy(out,path,(n+1)*sizeof(*out));return (DWORD)n;}
DWORD GetFullPathNameW(const WCHAR *input,DWORD capacity,WCHAR *out,WCHAR **part)
{
    WCHAR buffer[1024];size_t pos=0;
    if(input[1]!=':'){const WCHAR *prefix=L"C:\\Work\\";pos=host_wlength(prefix);memcpy(buffer,prefix,pos*sizeof(*buffer));}
    size_t n=host_wlength(input);assert(pos+n<1024);memcpy(buffer+pos,input,(n+1)*sizeof(*buffer));pos+=n;
    if(!out || capacity<=pos)return (DWORD)pos+1;
    memcpy(out,buffer,(pos+1)*sizeof(*out));if(part){*part=out;for(size_t i=0;i<pos;++i)if(out[i]=='\\' || out[i]=='/')*part=out+i+1;}return (DWORD)pos;
}
typedef struct {const WCHAR *Path;DWORD Attributes;} HOST_FILE;
static const HOST_FILE host_files[]={
    {L"C:\\Work",0x10},{L"C:\\Documents",0x10},{L"C:\\Apps",0x10},
    {L"C:\\Registered Apps\\Editor.exe",0x20},{L"C:\\Tools\\Print.exe",0x20},
    {L"C:\\Tools\\Other.exe",0x20},{L"C:\\Apps\\Tool.exe",0x20},{L"C:\\CMD.EXE",0x20},
    {L"C:\\Documents\\note.txt",0x20},{L"C:\\Documents\\\u041f\u0438\u0441\u044c\u043c\u043e 100%.txt",0x20},
    {L"C:\\Documents\\custom.cfg",0x20},{L"C:\\Documents\\bad.bad",0x20},{L"C:\\Documents\\unknown.zzz",0x20},
    {L"C:\\UserTools\\Special.exe",0x20},{L"C:\\Machine\\Special.exe",0x20},
    {L"C:\\Work\\batch script.bat",0x20},{L"C:\\Work\\Percent %APPS%.txt",0x20},
    {L"C:\\Users\\Alex",0x10},{L"C:\\Users\\Alex\\Desktop",0x10},
    {L"C:\\Users\\Alex\\Documents",0x10},{L"C:\\Users\\Alex\\RedirectedDocs",0x10},
    {L"C:\\WINDOWS",0x10},{L"C:\\WINDOWS\\SYSTEM32",0x10},{L"C:\\WINDOWS\\EXPLORER.EXE",0x20}
};
DWORD GetFileAttributesW(const WCHAR *path)
{
    for(unsigned i=0;i<sizeof(host_files)/sizeof(*host_files);++i)if(!host_wcompare(path,host_files[i].Path))return host_files[i].Attributes;
    SetLastError(2);return 0xffffffffu;
}
DWORD SearchPathW(const WCHAR *directory,const WCHAR *file,const WCHAR *extension,DWORD capacity,WCHAR *out,WCHAR **part)
{
    const WCHAR *paths=directory ? directory : host_env_value(L"PATH");
    for(size_t pos=0;paths[pos];) {
        WCHAR candidate[1024];size_t n=0;
        while(paths[pos] && paths[pos]!=';')candidate[n++]=paths[pos++];if(paths[pos])pos++;
        if(n && candidate[n-1]!='\\')candidate[n++]='\\';
        size_t f=host_wlength(file);memcpy(candidate+n,file,f*sizeof(*candidate));n+=f;
        if(extension){size_t e=host_wlength(extension);memcpy(candidate+n,extension,e*sizeof(*candidate));n+=e;}
        candidate[n]=0;if(GetFileAttributesW(candidate)==0xffffffffu)continue;
        if(!out || capacity<=n)return (DWORD)n+1;memcpy(out,candidate,(n+1)*sizeof(*out));if(part)*part=out;return (DWORD)n;
    }
    SetLastError(2);return 0;
}

typedef struct {HANDLE Root;const WCHAR *Key,*Name,*Value;DWORD Type;} HOST_REG_VALUE;
static const HOST_REG_VALUE host_reg_values[]={
    {SEX_HKCR,L".txt",L"",L"txtfile",1},{SEX_HKCR,L"txtfile\\shell",L"",L"open",1},
    {SEX_HKCR,L"txtfile\\shell\\open\\command",L"",L"\"editor.exe\" \"%1\" %*",1},
    {SEX_HKCU,L"Software\\Classes\\txtfile\\shell\\print\\command",L"",L"\"Print.exe\" \"%1\" %2 \"%3\"",1},
    {SEX_HKCU,L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.cfg\\UserChoice",L"ProgId",L"customcfg",1},
    {SEX_HKCR,L"customcfg\\shell\\open\\command",L"",L"\"Other.exe\" \"%L\"",1},
    {SEX_HKLM,L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\editor.exe",L"",L"%APPS%\\Editor.exe",2},
    {SEX_HKLM,L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\editor.exe",L"Path",L"%APPS%\\Plugins",2},
    {SEX_HKCU,L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Special.exe",L"",L"C:\\UserTools\\Special.exe",1},
    {SEX_HKLM,L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Special.exe",L"",L"C:\\Machine\\Special.exe",1},
    {SEX_HKCR,L"Directory\\shell\\open\\command",L"",L"\"C:\\Apps\\Tool.exe\" /folder \"%1\"",1},
    {SEX_HKCR,L"https\\shell\\open\\command",L"",L"\"C:\\Apps\\Tool.exe\" /url \"%1\"",1},
    {SEX_HKCR,L".bad",L"",L"badfile",1},{SEX_HKCR,L"badfile\\shell\\open\\command",L"",L"",1},
    {SEX_HKCU,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",L"Personal",L"%USERPROFILE%\\RedirectedDocs",2}
};
typedef struct HOST_KEY {struct HOST_KEY *Next;HANDLE Root;WCHAR *Path;} HOST_KEY;
static HOST_KEY *host_keys;static unsigned keys_opened,keys_closed;
static BOOL host_resolve_key(HANDLE input,HANDLE *root,const WCHAR **path)
{
    if(input==SEX_HKCR || input==SEX_HKCU || input==SEX_HKLM){*root=input;*path=L"";return TRUE;}
    for(HOST_KEY *key=host_keys;key;key=key->Next)if((HANDLE)key==input){*root=key->Root;*path=key->Path;return TRUE;}return FALSE;
}
static BOOL host_reg_key_exists(HANDLE root,const WCHAR *path)
{
    size_t n=host_wlength(path);
    for(unsigned i=0;i<sizeof(host_reg_values)/sizeof(*host_reg_values);++i){
        const HOST_REG_VALUE *value=host_reg_values+i;if(value->Root!=root)continue;
        if(!host_wcompare(value->Key,path))return TRUE;
        if(host_wlength(value->Key)>n && value->Key[n]=='\\') {
            WCHAR prefix[1024];memcpy(prefix,value->Key,n*sizeof(*prefix));prefix[n]=0;
            if(!host_wcompare(prefix,path))return TRUE;
        }
    }
    return FALSE;
}
LONG RegOpenKeyExW(HANDLE input,const WCHAR *name,DWORD options,DWORD access,HANDLE *out)
{
    assert(!options && access==SEX_KEY_READ);HANDLE root;const WCHAR *parent;
    if(!host_resolve_key(input,&root,&parent))return 6;
    size_t a=host_wlength(parent),b=host_wlength(name);WCHAR *path=calloc(a+b+2,sizeof(*path));assert(path);
    memcpy(path,parent,a*sizeof(*path));if(a && b)path[a++]='\\';memcpy(path+a,name,(b+1)*sizeof(*path));
    if(!host_reg_key_exists(root,path)){free(path);return 2;}
    HOST_KEY *key=malloc(sizeof(*key));assert(key);*key=(HOST_KEY){host_keys,root,path};host_keys=key;keys_opened++;*out=key;return 0;
}
LONG RegQueryValueExW(HANDLE input,const WCHAR *name,DWORD *reserved,DWORD *type,unsigned char *data,DWORD *bytes)
{
    assert(!reserved);HANDLE root;const WCHAR *path;if(!host_resolve_key(input,&root,&path))return 6;
    for(unsigned i=0;i<sizeof(host_reg_values)/sizeof(*host_reg_values);++i) {
        const HOST_REG_VALUE *value=host_reg_values+i;
        if(value->Root!=root || host_wcompare(value->Key,path) || host_wcompare(value->Name,name ? name:L""))continue;
        DWORD needed=(DWORD)((host_wlength(value->Value)+1)*sizeof(WCHAR));if(type)*type=value->Type;
        if(!data){*bytes=needed;return 0;}if(*bytes<needed){*bytes=needed;return 234;}
        memcpy(data,value->Value,needed);*bytes=needed;return 0;
    }
    return 2;
}
LONG RegEnumKeyExW(HANDLE input,DWORD index,WCHAR *name,DWORD *chars,DWORD *reserved,WCHAR *class_name,DWORD *class_chars,void *time)
{
    (void)input;(void)index;(void)name;(void)chars;(void)reserved;(void)class_name;(void)class_chars;(void)time;return 259;
}
LONG RegCloseKey(HANDLE input)
{
    HOST_KEY **link=&host_keys;while(*link && (HANDLE)*link!=input)link=&(*link)->Next;
    assert(*link);HOST_KEY *key=*link;*link=key->Next;free(key->Path);free(key);keys_closed++;return 0;
}

static WCHAR *launched_application,*launched_command,*launched_directory,*launched_environment;
static DWORD launched_flags;static unsigned launches,closed_threads,closed_processes;
static DWORD launch_failure;
static void clear_launch(void)
{free(launched_application);free(launched_command);free(launched_directory);free(launched_environment);
 launched_application=launched_command=launched_directory=launched_environment=0;}
BOOL CreateProcessW(const WCHAR *application,WCHAR *command,void *ps,void *ts,BOOL inherit,DWORD flags,
    void *environment,const WCHAR *directory,SEX_STARTUP_INFO *startup,SEX_PROCESS_INFO *process)
{
    assert(!ps && !ts && !inherit && startup->Size==104 && (startup->Flags&1));
    if(launch_failure){SetLastError(launch_failure);return FALSE;}
    assert(GetFileAttributesW(application)!=0xffffffffu);
    clear_launch();launched_application=host_wcopy(application);launched_command=host_wcopy(command);
    if(directory)launched_directory=host_wcopy(directory);
    if(environment) {
        const WCHAR *block=environment;size_t n=0;while(block[n] || block[n+1])n++;n+=2;
        launched_environment=malloc(n*sizeof(*block));assert(launched_environment);memcpy(launched_environment,block,n*sizeof(*block));
    }
    launched_flags=flags;*process=(SEX_PROCESS_INFO){(HANDLE)0x7001,(HANDLE)0x7002,500,501};launches++;return TRUE;
}
BOOL CloseHandle(HANDLE handle)
{if(handle==(HANDLE)0x7001)closed_processes++;else if(handle==(HANDLE)0x7002)closed_threads++;else assert(handle==(HANDLE)0x7003);return TRUE;}
DWORD WaitForInputIdle(HANDLE handle,DWORD timeout){assert(handle==(HANDLE)0x7001 && timeout==60000);return 0;}
HANDLE GetCurrentProcess(void){return (HANDLE)-1LL;}
BOOL OpenProcessToken(HANDLE process,DWORD access,HANDLE *out){(void)process;(void)access;(void)out;SetLastError(6);return FALSE;}
BOOL GetTokenInformation(HANDLE token,int kind,void *data,DWORD bytes,DWORD *needed){(void)token;(void)kind;(void)data;(void)bytes;if(needed)*needed=0;SetLastError(6);return FALSE;}
BOOL ConvertSidToStringSidW(const void *sid,WCHAR **text){(void)sid;(void)text;SetLastError(6);return FALSE;}
DWORD GetWindowsDirectoryW(WCHAR *out,DWORD count){const WCHAR *path=L"C:\\WINDOWS";DWORD n=(DWORD)host_wlength(path);if(count<=n)return n+1;memcpy(out,path,(n+1)*sizeof(*out));return n;}
DWORD GetSystemDirectoryW(WCHAR *out,DWORD count){const WCHAR *path=L"C:\\WINDOWS\\SYSTEM32";DWORD n=(DWORD)host_wlength(path);if(count<=n)return n+1;memcpy(out,path,(n+1)*sizeof(*out));return n;}
BOOL CreateDirectoryW(const WCHAR *path,void *security){(void)path;(void)security;SetLastError(5);return FALSE;}
void *CoTaskMemAlloc(unsigned long long bytes){return HeapAlloc(GetProcessHeap(),0,bytes);}
void CoTaskMemFree(void *memory){if(memory)HeapFree(GetProcessHeap(),0,memory);}
DWORD GetCurrentProcessId(void){return 1000;}
static DWORD selection_wait_result;
HANDLE CreateEventW(void *attributes,BOOL manual,BOOL initial,const WCHAR *name){(void)attributes;assert(manual&&!initial&&name);SetLastError(0);return (HANDLE)0x7003;}
DWORD WaitForMultipleObjects(DWORD count,const HANDLE *objects,BOOL all,DWORD timeout){assert(count==2&&!all&&timeout==30000&&objects[0]==(HANDLE)0x7003);return selection_wait_result;}
BOOL GetExitCodeProcess(HANDLE process,DWORD *code){assert(process==(HANDLE)0x7001);*code=2;return TRUE;}
int MultiByteToWideChar(unsigned cp,DWORD flags,const char *text,int bytes,WCHAR *out,int count)
{
    assert(!cp && !flags);size_t n=bytes==-1?strlen(text)+1:(size_t)bytes;if(!out)return (int)n;
    if(count<(int)n)return 0;for(size_t i=0;i<n;++i)out[i]=(unsigned char)text[i];return (int)n;
}
static unsigned live_heap_blocks(void)
{
    RTL_HEAP_STATE *heap=GetProcessHeap();unsigned count=0;
    for(RTL_HEAP_SEGMENT *segment=&heap->FirstSegment;segment;segment=segment->Next)
        for(BYTE *position=segment->First;position<segment->Base+segment->Committed;) {
            RTL_HEAP_BLOCK *block=(RTL_HEAP_BLOCK *)position;
            if((block->Flags&RTL_BLOCK_BUSY) && !(block->Flags&RTL_BLOCK_INTERNAL))count++;
            position=(BYTE *)(block+1)+block->Span;
        }
    return count;
}
static SHELLEXECUTEINFOW request(const WCHAR *file,const WCHAR *parameters)
{return (SHELLEXECUTEINFOW){.cbSize=112,.fMask=SEX_NOCLOSEPROCESS|SEX_NO_UI,.lpFile=file,.lpParameters=parameters,.nShow=1};}
static void check_no_leaks(void)
{assert(!host_keys && keys_opened==keys_closed && env_allocated==env_freed && !live_heap_blocks());}

int main(void)
{
    _Static_assert(sizeof(SHELLEXECUTEINFOW)==112,"ShellExecute layout");
    SHELLEXECUTEINFOW info=request(L"C:\\Apps\\Tool.exe",L"--name \"a b\"");
    assert(ShellExecuteExW(&info));assert(info.hProcess==(HANDLE)0x7001 && (uintptr_t)info.hInstApp>32);
    assert(!host_wcompare(launched_application,L"C:\\Apps\\Tool.exe"));
    assert(!host_wcompare(launched_command,L"\"C:\\Apps\\Tool.exe\" --name \"a b\""));
    assert(closed_threads==1 && !closed_processes);CloseHandle(info.hProcess);check_no_leaks();
    info=request(L"C:\\Documents\\\u041f\u0438\u0441\u044c\u043c\u043e 100%.txt",L"--readonly");
    assert(ShellExecuteExW(&info));assert(!host_wcompare(launched_application,L"C:\\Registered Apps\\Editor.exe"));
    assert(!host_wcompare(launched_command,L"\"editor.exe\" \"C:\\Documents\\\u041f\u0438\u0441\u044c\u043c\u043e 100%.txt\" --readonly"));
    assert(launched_environment && (launched_flags&0x400));
    const WCHAR *path=launched_environment;while(*path && host_wcompare(path,L"PATH=C:\\Tools;C:\\WINDOWS\\SYSTEM32;C:\\Registered Apps\\Plugins"))path+=host_wlength(path)+1;
    assert(*path);CloseHandle(info.hProcess);check_no_leaks();
    info=request(L"C:\\Documents\\note.txt",L"\"Laser Printer\" \"x y\"");info.lpVerb=L"print";
    assert(ShellExecuteExW(&info));assert(!host_wcompare(launched_application,L"C:\\Tools\\Print.exe"));
    assert(!host_wcompare(launched_command,L"\"Print.exe\" \"C:\\Documents\\note.txt\" \"Laser Printer\" \"x y\""));
    CloseHandle(info.hProcess);check_no_leaks();
    info=request(L"C:\\Documents\\custom.cfg",0);assert(ShellExecuteExW(&info));
    assert(!host_wcompare(launched_application,L"C:\\Tools\\Other.exe"));CloseHandle(info.hProcess);
    info=request(L"Special.exe",0);assert(ShellExecuteExW(&info));assert(!host_wcompare(launched_application,L"C:\\UserTools\\Special.exe"));CloseHandle(info.hProcess);
    info=request(L"C:\\Documents",0);assert(ShellExecuteExW(&info));assert(!host_wcompare(launched_command,L"\"C:\\Apps\\Tool.exe\" /folder \"C:\\Documents\""));CloseHandle(info.hProcess);
    info=request(L"https://example.test/a%20b",0);assert(ShellExecuteExW(&info));assert(!host_wcompare(launched_command,L"\"C:\\Apps\\Tool.exe\" /url \"https://example.test/a%20b\""));CloseHandle(info.hProcess);
    info=request(L"C:\\Work\\batch script.bat",L"hello");assert(ShellExecuteExW(&info));assert(!host_wcompare(launched_application,L"C:\\CMD.EXE"));
    assert(!host_wcompare(launched_command,L"\"C:\\CMD.EXE\" /c \"C:\\Work\\batch script.bat\" hello"));CloseHandle(info.hProcess);
    check_no_leaks();
    unsigned before=launches;info=request(L"C:\\Documents\\unknown.zzz",0);assert(!ShellExecuteExW(&info) && GetLastError()==SEX_NO_ASSOC && !info.hProcess && (uintptr_t)info.hInstApp==31);
    info=request(L"C:\\Documents\\bad.bad",0);assert(!ShellExecuteExW(&info) && !info.hProcess && launches==before);
    launch_failure=5;info=request(L"C:\\Apps\\Tool.exe",0);assert(!ShellExecuteExW(&info) && GetLastError()==5 && launches==before);launch_failure=0;
    HANDLE class_key;assert(!RegOpenKeyExW(SEX_HKCR,L"txtfile",0,SEX_KEY_READ,&class_key));
    info=request(L"C:\\Documents\\unknown.zzz",0);info.fMask|=SEX_CLASSKEY;info.hkeyClass=class_key;
    assert(ShellExecuteExW(&info));CloseHandle(info.hProcess);assert(!RegCloseKey(class_key));check_no_leaks();
    SHELLEXECUTEINFOA ansi={.cbSize=112,.fMask=SEX_NO_UI,.lpFile="C:\\Apps\\Tool.exe",.nShow=1};
    assert(ShellExecuteExA(&ansi) && !ansi.hProcess);check_no_leaks();
    WCHAR folder_path[MAX_PATH];
    assert(SHGetFolderPathW(0,0x10,0,0,folder_path)==S_OK&&!host_wcompare(folder_path,L"C:\\Users\\Alex\\Desktop"));
    assert(SHGetFolderPathW(0,5,0,0,folder_path)==S_OK&&!host_wcompare(folder_path,L"C:\\Users\\Alex\\RedirectedDocs"));
    assert(SHGetFolderPathW(0,5,0,1,folder_path)==S_OK&&!host_wcompare(folder_path,L"C:\\Users\\Alex\\Documents"));
    assert(SHGetFolderPathW(0,0x11,0,0,folder_path)<0);
    void *pidl=0;unsigned attributes=0;
    assert(SHParseDisplayName(L"C:\\Documents\\note.txt",0,&pidl,0x60000000u,&attributes)==S_OK);
    assert(pidl&&attributes==0x40000000u&&ILGetSize(pidl)>2);
    assert(SHGetPathFromIDListW(pidl,folder_path)&&!host_wcompare(folder_path,L"C:\\Documents\\note.txt"));
    void *parent=ILCloneFull(pidl),*last=ILClone(ILFindLastID(pidl));assert(parent&&last);
    assert(ILRemoveLastID(parent)&&SHGetPathFromIDListW(parent,folder_path)&&!host_wcompare(folder_path,L"C:\\Documents"));
    void *combined=ILCombine(parent,last);assert(combined&&ILIsEqual(combined,pidl));
    unsigned char generic[]={3,0,0xab,0,0};void *generic_copy=ILClone(generic);
    assert(generic_copy&&ILGetSize(generic_copy)==5&&!SHGetPathFromIDListW(generic_copy,folder_path));ILFree(generic_copy);
    assert(SHOpenFolderAndSelectItems(pidl,0,0,0)==S_OK);
    assert(!host_wcompare(launched_application,L"C:\\WINDOWS\\EXPLORER.EXE"));
    assert(launched_command&&host_wlength(launched_command)>40);
    selection_wait_result=1;assert(SHOpenFolderAndSelectItems(pidl,0,0,0)<0&&GetLastError()==2);
    selection_wait_result=0x102;assert(SHOpenFolderAndSelectItems(pidl,0,0,0)<0&&GetLastError()==1460);selection_wait_result=0;
    ILFree(parent);ILFree(last);ILFree(combined);ILFree(pidl);check_no_leaks();
    for(unsigned fail=1;fail<=24;++fail) {
        allocation_attempts=0;allocation_fail_at=fail;info=request(L"C:\\Documents\\note.txt",L"--readonly");
        if(ShellExecuteExW(&info))CloseHandle(info.hProcess);else assert(!info.hProcess);
        allocation_fail_at=0;check_no_leaks();
    }
    clear_launch();HANDLE heap=GetProcessHeap();assert(!RtlDestroyHeap(heap));host_peb.ProcessHeap=0;host_runtime_cleanup();
    puts("PASS ShellExecute: associations, verbs, UserChoice, App Paths, Unicode, quoting, env, directories, URIs, batch and handle/OOM ownership");
    return 0;
}
