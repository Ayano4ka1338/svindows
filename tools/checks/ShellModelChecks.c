/* Actual shell path/command/shortcut code; OS boundary fixtures make the
 * observable launch and error contracts independent from the implementation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/w/explorer/Common.h"

static unsigned allocations,frees,launched,closed;
static DWORD last_error,reported_error;
static BOOL launch_success=TRUE;
static WCHAR launched_path[260],launched_command[1024];
HANDLE GetProcessHeap(void){return (HANDLE)1;}
void *HeapAlloc(HANDLE heap,DWORD flags,SIZE_T bytes){(void)heap;(void)flags;++allocations;return calloc(1,(size_t)bytes);}
void *HeapReAlloc(HANDLE heap,DWORD flags,void *memory,SIZE_T bytes){(void)heap;(void)flags;return realloc(memory,(size_t)bytes);}
BOOL HeapFree(HANDLE heap,DWORD flags,void *memory){(void)heap;(void)flags;if(memory){++frees;free(memory);}return TRUE;}
static WCHAR upper(WCHAR c){return c>='a'&&c<='z'?c-32:c;}
int CompareStringOrdinal(const WCHAR *a,int an,const WCHAR *b,int bn,BOOL ignore)
{
    if(an<0){an=0;while(a[an])++an;}if(bn<0){bn=0;while(b[bn])++bn;}
    for(int i=0;i<an&&i<bn;++i){WCHAR ac=ignore?upper(a[i]):a[i],bc=ignore?upper(b[i]):b[i];if(ac<bc)return 1;if(ac>bc)return 3;}
    return an==bn?2:an<bn?1:3;
}
DWORD GetLastError(void){return last_error;}
void SetLastError(DWORD error){last_error=error;}
HANDLE GetStdHandle(DWORD which){(void)which;return (HANDLE)1;}
BOOL WriteFile(HANDLE handle,const void *data,DWORD bytes,DWORD *written,void *overlapped){(void)handle;(void)data;(void)overlapped;*written=bytes;return TRUE;}
BOOL CloseHandle(HANDLE handle){assert(handle==(HANDLE)2||handle==(HANDLE)3);++closed;return TRUE;}
BOOL CreateProcessW(const WCHAR *path,WCHAR *command,void *pa,void *ta,BOOL inherit,DWORD flags,void *env,const WCHAR *cwd,SHELL_STARTUP_INFO *startup,SHELL_PROCESS_INFO *process)
{
    (void)pa;(void)ta;(void)env;(void)cwd;assert(!inherit&&!flags&&startup->cb==sizeof(*startup));
    ShellCopy(launched_path,260,path);ShellCopy(launched_command,1024,command);++launched;
    if(!launch_success){last_error=126;return FALSE;}process->process=(HANDLE)2;process->thread=(HANDLE)3;return TRUE;
}
DWORD ExpandEnvironmentStringsW(const WCHAR *source,WCHAR *out,DWORD capacity)
{DWORD need=(DWORD)ShellLength(source)+1;if(out&&capacity>=need)ShellCopy(out,capacity,source);return need;}
DWORD SearchPathW(const WCHAR *paths,const WCHAR *file,const WCHAR *ext,DWORD capacity,WCHAR *out,WCHAR **part)
{(void)paths;(void)ext;(void)part;assert(ShellCopy(out,capacity,file));return (DWORD)ShellLength(file);}
void ShellError(HWND owner,const WCHAR *action,const WCHAR *path,DWORD error){(void)owner;(void)action;(void)path;reported_error=error;}
HWND ShellOpenFolder(const WCHAR *path){(void)path;return (HWND)4;}
DWORD GetFileAttributesW(const WCHAR *path){(void)path;return FILE_ATTRIBUTE_NORMAL;}
BOOL ShellExecuteExW(SHELL_EXECUTE_INFO *info){assert(info->size==sizeof(*info)&&!ShellCompare(info->verb,L"open"));info->process=(HANDLE)2;return TRUE;}
DWORD GetPrivateProfileStringW(const WCHAR *section,const WCHAR *key,const WCHAR *fallback,WCHAR *out,DWORD capacity,const WCHAR *path)
{
    (void)fallback;assert(!ShellCompare(section,L"InternetShortcut")&&!ShellCompare(key,L"URL"));const WCHAR *value;
    if(!ShellCompare(path,L"C:\\one.url"))value=L"file:///C:/Program%20Files/App/app.exe";
    else if(!ShellCompare(path,L"C:\\cycle-a.url"))value=L"file:///C:/cycle-b.url";
    else if(!ShellCompare(path,L"C:\\cycle-b.url"))value=L"file:///C:/cycle-a.url";
    else if(!ShellCompare(path,L"C:\\nul.url"))value=L"file:///C:/bad%00name.exe";
    else value=L"";
    assert(ShellCopy(out,capacity,value));return (DWORD)ShellLength(value);
}
int MultiByteToWideChar(UINT page,DWORD flags,const char *source,int length,WCHAR *out,int capacity)
{assert(page==65001&&flags==8);if(length>capacity)return 0;for(int i=0;i<length;++i){assert((unsigned char)source[i]<128);out[i]=(BYTE)source[i];}return length;}
static void path(const WCHAR *input,const WCHAR *directory,const WCHAR *expected)
{WCHAR normalized[260];assert(ShellNormalizePath(input,directory,normalized,260));assert(!ShellCompare(normalized,expected));}
int main(void)
{
    path(L"..\\Notes\\.\\test.txt",L"C:\\Users\\User\\Documents",L"C:\\Users\\User\\Notes\\test.txt");
    path(L"\\Windows7\\notepad.exe",L"C:\\Users\\User",L"C:\\Windows7\\notepad.exe");
    path(L"C:note.txt",L"C:\\Users\\User",L"C:\\Users\\User\\note.txt");
    path(L"..\\..\\..\\..",L"C:\\Users",L"C:\\");
    path(L"C:/Users//User/Documents/../Desktop",L"C:\\",L"C:\\Users\\User\\Desktop");
    WCHAR out[260];assert(!ShellNormalizePath(L"\\\\server\\share",L"C:\\",out,260));
    assert(!ShellNormalizePath(L"C:\\bad:name",L"C:\\",out,260));
    assert(!ShellNormalizePath(L"C:\\length",L"C:\\",out,5));
    assert(ShellPathWithin(L"C:\\Documents\\child",L"c:\\documents"));
    assert(ShellPathWithin(L"C:\\Documents",L"C:\\Documents"));
    assert(!ShellPathWithin(L"C:\\Documents2",L"C:\\Documents"));
    assert(!ShellPathWithin(L"C:\\Document",L"C:\\Documents"));
    assert(ShellValidName(L"report — текст.txt"));
    assert(!ShellValidName(L"NUL.txt")&&!ShellValidName(L"com2")&&!ShellValidName(L"LPT².txt"));
    assert(!ShellValidName(L"..")&&!ShellValidName(L"trailing.")&&!ShellValidName(L"trailing "));
    WCHAR bad_surrogate[]={0xd800,'x',0};assert(!ShellValidName(bad_surrogate));
    WCHAR valid_pair[]={0xd83d,0xde00,'.','t','x','t',0};assert(ShellValidName(valid_pair));
    assert(ShellLaunch(NULL,L"C:\\Program Files\\Tool\\app.exe",L"--name \"x y\""));
    assert(!ShellCompare(launched_path,L"C:\\Program Files\\Tool\\app.exe"));
    assert(!ShellCompare(launched_command,L"\"C:\\Program Files\\Tool\\app.exe\" --name \"x y\""));
    assert(launched==1&&closed==2);
    assert(ShellRunCommand(NULL,L"\"C:\\Program Files\\Tool\\app.exe\" --value 7"));
    assert(!ShellCompare(launched_command,L"\"C:\\Program Files\\Tool\\app.exe\" --value 7"));
    assert(!ShellRunCommand(NULL,L"\"unclosed executable"));assert(reported_error==123&&launched==2);
    assert(ShellReadShortcut(L"C:\\one.url",out,260));assert(!ShellCompare(out,L"C:\\Program Files\\App\\app.exe"));
    assert(!ShellReadShortcut(L"C:\\nul.url",out,260));
    assert(!ShellOpenPath(NULL,L"C:\\cycle-a.url"));assert(reported_error==13&&launched==2);
    assert(ShellOpenPath(NULL,L"C:\\one.url"));assert(launched==3);
    launch_success=FALSE;assert(!ShellLaunch(NULL,L"C:\\missing.exe",NULL));assert(reported_error==126);
    assert(allocations==frees);
    puts("Shell path, reserved-name, command quoting, shortcut cycle and launch cleanup checks passed");
    return 0;
}
