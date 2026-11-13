#include "Common.h"

HINSTANCE ShellInstance;
WCHAR ShellDesktopPath[SHELL_PATH_CAPACITY];
WCHAR ShellDocumentsPath[SHELL_PATH_CAPACITY];
WCHAR ShellExecutablePath[SHELL_PATH_CAPACITY];
BOOL ShellStandalone;

SIZE_T ShellLength(const WCHAR *s)
{ SIZE_T n=0; if(s) while(s[n]) ++n; return n; }
int ShellCompare(const WCHAR *a,const WCHAR *b)
{ return CompareStringOrdinal(a,-1,b,-1,TRUE)-2; }
BOOL ShellCopy(WCHAR *out,SIZE_T capacity,const WCHAR *text)
{
    SIZE_T n=ShellLength(text);
    if(!out||!capacity||n>=capacity) return FALSE;
    for(SIZE_T i=0;i<=n;++i) out[i]=text?text[i]:0;
    return TRUE;
}
void ShellZero(void *p,SIZE_T bytes)
{ BYTE *b=p; while(bytes--) *b++=0; }
void *ShellAlloc(SIZE_T bytes)
{ return HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,bytes); }
void ShellFree(void *p)
{ if(p) HeapFree(GetProcessHeap(),0,p); }
BOOL ShellReserve(void **memory,SIZE_T *capacity,SIZE_T wanted,SIZE_T element)
{
    if(wanted<=*capacity) return TRUE;
    SIZE_T cap=*capacity?*capacity:8;
    while(cap<wanted) { if(cap>((SIZE_T)-1)/2) return FALSE; cap*=2; }
    if(!element||cap>((SIZE_T)-1)/element) return FALSE;
    void *fresh=*memory?HeapReAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,*memory,cap*element):ShellAlloc(cap*element);
    if(!fresh) return FALSE;
    *memory=fresh; *capacity=cap; return TRUE;
}
WCHAR *ShellDuplicate(const WCHAR *text)
{
    SIZE_T n=ShellLength(text);
    if(n>((SIZE_T)-1)/sizeof(WCHAR)-1) return NULL;
    WCHAR *out=ShellAlloc((n+1)*sizeof(WCHAR));
    if(out) ShellCopy(out,n+1,text); return out;
}
const WCHAR *ShellLeaf(const WCHAR *path)
{
    const WCHAR *leaf=path;
    for(const WCHAR *p=path;p&&*p;++p) if(*p=='\\'||*p=='/') leaf=p+1;
    return leaf;
}
const WCHAR *ShellExtension(const WCHAR *path)
{
    const WCHAR *leaf=ShellLeaf(path),*extension=L"";
    for(const WCHAR *p=leaf;p&&*p;++p) if(*p=='.') extension=p;
    return extension;
}
BOOL ShellJoin(WCHAR *out,SIZE_T cap,const WCHAR *directory,const WCHAR *leaf)
{
    SIZE_T n=ShellLength(directory),m=ShellLength(leaf);
    BOOL separator=n&&directory[n-1]!='\\'&&directory[n-1]!='/';
    if(n+separator+m>=cap) return FALSE;
    for(SIZE_T i=0;i<n;++i) out[i]=directory[i];
    if(separator) out[n++]='\\';
    for(SIZE_T i=0;i<=m;++i) out[n+i]=leaf[i];
    return TRUE;
}
BOOL ShellNormalizePath(const WCHAR *input,const WCHAR *cwd,WCHAR *out,SIZE_T cap)
{
    if(!input||!*input||cap<4) return FALSE;
    WCHAR joined[SHELL_PATH_CAPACITY*2]; SIZE_T used=0;
    BOOL drive=input[0]&&input[1]==':';
    if((input[0]=='\\'||input[0]=='/')&&(input[1]=='\\'||input[1]=='/')) return FALSE;
    WCHAR letter=drive?input[0]:(cwd&&cwd[1]==':'?cwd[0]:'C');
    if(letter>='a'&&letter<='z') letter-=32;
    if(letter<'A'||letter>'Z') return FALSE;
    SIZE_T offset=drive?2:0;
    BOOL rooted=input[offset]=='\\'||input[offset]=='/';
    if(!rooted&&cwd&&cwd[1]==':'&&(cwd[0]==letter||cwd[0]==letter+32)) {
        if(!ShellCopy(joined,sizeof(joined)/sizeof(*joined),cwd)) return FALSE;
        used=ShellLength(joined);
    } else { joined[used++]=letter;joined[used++]=':';joined[used++]='\\'; }
    if(used&&joined[used-1]!='\\'&&joined[used-1]!='/') joined[used++]='\\';
    while(input[offset]=='\\'||input[offset]=='/') ++offset;
    for(;input[offset];++offset) {
        if(used+1>=sizeof(joined)/sizeof(*joined)||input[offset]<32||input[offset]==':'||input[offset]=='"') return FALSE;
        joined[used++]=input[offset]=='/'?'\\':input[offset];
    }
    joined[used]=0;
    out[0]=letter;out[1]=':';out[2]='\\';used=3;
    for(SIZE_T pos=3;joined[pos];) {
        while(joined[pos]=='\\') ++pos;
        SIZE_T begin=pos;while(joined[pos]&&joined[pos]!='\\') ++pos;
        SIZE_T size=pos-begin;if(!size) continue;
        if(size==1&&joined[begin]=='.') continue;
        if(size==2&&joined[begin]=='.'&&joined[begin+1]=='.') {
            while(used>3&&out[used-1]!='\\') --used;
            if(used>3) --used;
            continue;
        }
        if(used>3) { if(used+1>=cap) return FALSE;out[used++]='\\'; }
        if(size>=cap-used) return FALSE;
        for(SIZE_T i=0;i<size;++i) out[used++]=joined[begin+i];
    }
    out[used]=0;return TRUE;
}
BOOL ShellPathWithin(const WCHAR *path,const WCHAR *directory)
{
    SIZE_T n=ShellLength(directory),m=ShellLength(path);
    while(n>3&&directory[n-1]=='\\') --n;
    if(m<n||CompareStringOrdinal(path,(int)n,directory,(int)n,TRUE)!=2) return FALSE;
    return m==n||n==3||path[n]=='\\';
}
BOOL ShellValidName(const WCHAR *name)
{
    SIZE_T n=ShellLength(name);
    if(!n||n>255||name[n-1]==' '||name[n-1]=='.') return FALSE;
    for(SIZE_T i=0;i<n;++i) {
        WCHAR c=name[i];
        if(c<32||c=='\\'||c=='/'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') return FALSE;
        if(c>=0xd800&&c<=0xdbff) { if(++i>=n||name[i]<0xdc00||name[i]>0xdfff) return FALSE; }
        else if(c>=0xdc00&&c<=0xdfff) return FALSE;
    }
    WCHAR base[8];SIZE_T b=0;
    while(name[b]&&name[b]!='.'&&b<7) {base[b]=name[b];++b;}base[b]=0;
    if(!ShellCompare(base,L"CON")||!ShellCompare(base,L"PRN")||!ShellCompare(base,L"AUX")||!ShellCompare(base,L"NUL")) return FALSE;
    if(b==4&&((base[3]>='1'&&base[3]<='9')||base[3]==0xb9||base[3]==0xb2||base[3]==0xb3)) {
        base[3]=0;if(!ShellCompare(base,L"COM")||!ShellCompare(base,L"LPT")) return FALSE;
    }
    return TRUE;
}
void ShellUnsigned(WCHAR *out,SIZE_T cap,ULONGLONG value)
{
    WCHAR reverse[32];SIZE_T n=0;
    do { reverse[n++]=(WCHAR)('0'+value%10);value/=10; } while(value);
    if(!cap) return;
    SIZE_T used=0;while(n&&used+1<cap) out[used++]=reverse[--n];out[used]=0;
}
void ShellLog(const char *s)
{ DWORD n=0,written;while(s[n]) ++n;WriteFile(GetStdHandle((DWORD)-12),s,n,&written,NULL); }
void ShellLogPath(const char *prefix,const WCHAR *path)
{
    ShellLog(prefix);char line[512];SIZE_T n=0;
    for(SIZE_T i=0;path&&path[i]&&n+5<sizeof(line);++i) {
        UINT c=path[i];
        if(c<128) line[n++]=(char)c;
        else if(c<2048) {line[n++]=(char)(0xc0|(c>>6));line[n++]=(char)(0x80|(c&63));}
        else {line[n++]=(char)(0xe0|(c>>12));line[n++]=(char)(0x80|((c>>6)&63));line[n++]=(char)(0x80|(c&63));}
    }
    line[n++]='\r';line[n++]='\n';line[n]=0;ShellLog(line);
}
void ShellSetStatus(HWND hwnd,const WCHAR *text)
{ if(hwnd) SetWindowTextW(hwnd,text); }
HWND ShellButton(HWND parent,UINT id,const WCHAR *caption)
{ return CreateWindowExW(0,L"BUTTON",caption,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
    0,0,1,1,parent,(HMENU)(UINT_PTR)id,ShellInstance,NULL); }
HWND ShellEdit(HWND parent,UINT id,const WCHAR *text)
{ return CreateWindowExW(0,L"EDIT",text,WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,
    0,0,1,1,parent,(HMENU)(UINT_PTR)id,ShellInstance,NULL); }

/* Quote an executable using the documented CommandLineToArgvW backslash rules. */
static SIZE_T quote_path(WCHAR *out,const WCHAR *path)
{
    SIZE_T n=0;out[n++]='"';
    for(SIZE_T i=0;path[i];) {
        SIZE_T slashes=0;while(path[i]=='\\') {++slashes;++i;}
        SIZE_T copies=path[i]=='"'||!path[i]?slashes*2:slashes;
        while(copies--) out[n++]='\\';
        if(path[i]=='"') out[n++]='\\';
        if(path[i]) out[n++]=path[i++];
    }
    out[n++]='"';return n;
}
BOOL ShellLaunch(HWND owner,const WCHAR *path,const WCHAR *arguments)
{
    SIZE_T capacity=ShellLength(path)*2+ShellLength(arguments)+8;
    WCHAR *command=ShellAlloc(capacity*sizeof(WCHAR));
    if(!command) {ShellError(owner,L"Start application",path,8);return FALSE;}
    SIZE_T n=quote_path(command,path);
    if(arguments&&*arguments) {command[n++]=' ';ShellCopy(command+n,capacity-n,arguments);} else command[n]=0;
    SHELL_STARTUP_INFO startup;SHELL_PROCESS_INFO process;
    ShellZero(&startup,sizeof(startup));ShellZero(&process,sizeof(process));startup.cb=sizeof(startup);
    BOOL ok=CreateProcessW(path,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&process);
    DWORD error=GetLastError();ShellFree(command);
    if(!ok) {ShellError(owner,L"Start application",path,error);return FALSE;}
    CloseHandle(process.thread);CloseHandle(process.process);
    ShellLogPath("[explorer] launched ",path);return TRUE;
}
BOOL ShellRunCommand(HWND owner,const WCHAR *input)
{
    DWORD required=ExpandEnvironmentStringsW(input,NULL,0);
    if(!required) {ShellError(owner,L"Run",input,GetLastError());return FALSE;}
    WCHAR *command=ShellAlloc((SIZE_T)required*sizeof(WCHAR));
    if(!command) {ShellError(owner,L"Run",input,8);return FALSE;}
    if(!ExpandEnvironmentStringsW(input,command,required)) {DWORD error=GetLastError();ShellFree(command);ShellError(owner,L"Run",input,error);return FALSE;}
    WCHAR executable[SHELL_PATH_CAPACITY];SIZE_T n=0,pos=0;BOOL quoted=FALSE;
    while(command[pos]==' '||command[pos]=='\t') ++pos;
    while(command[pos]&&(quoted||(command[pos]!=' '&&command[pos]!='\t'))) {
        SIZE_T slashes=0;while(command[pos]=='\\') {++slashes;++pos;}
        if(command[pos]=='"') {
            for(SIZE_T j=0;j<slashes/2;++j) {if(n+1>=SHELL_PATH_CAPACITY) goto invalid;executable[n++]='\\';}
            if(slashes&1) {if(n+1>=SHELL_PATH_CAPACITY) goto invalid;executable[n++]='"';}
            else quoted=!quoted;
            ++pos;
        } else {
            while(slashes--) {if(n+1>=SHELL_PATH_CAPACITY) goto invalid;executable[n++]='\\';}
            if(command[pos]&&(quoted||(command[pos]!=' '&&command[pos]!='\t'))) {if(n+1>=SHELL_PATH_CAPACITY) goto invalid;executable[n++]=command[pos++];}
        }
    }
    if(quoted||!n) goto invalid;
    executable[n]=0;while(command[pos]==' '||command[pos]=='\t') ++pos;
    WCHAR resolved[SHELL_PATH_CAPACITY];
    DWORD length=SearchPathW(NULL,executable,L".exe",SHELL_PATH_CAPACITY,resolved,NULL);
    if(!length||length>=SHELL_PATH_CAPACITY) {DWORD error=GetLastError();ShellFree(command);ShellError(owner,L"Run",executable,error?error:2);return FALSE;}
    BOOL ok=ShellLaunch(owner,resolved,command+pos);ShellFree(command);return ok;
invalid:
    ShellFree(command);ShellError(owner,L"Run",input,123);return FALSE;
}
static int hex_value(WCHAR c)
{ return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; }
BOOL ShellReadShortcut(const WCHAR *path,WCHAR *target,SIZE_T capacity)
{
    WCHAR url[1024];
    DWORD n=GetPrivateProfileStringW(L"InternetShortcut",L"URL",L"",url,1024,path);
    if(!n||n>=1023) return FALSE;
    if(n<8||CompareStringOrdinal(url,8,L"file:///",8,TRUE)!=2) return ShellCopy(target,capacity,url);
    SIZE_T used=0;
    for(SIZE_T i=8;url[i];) {
        if(url[i]=='%') {
            char bytes[256];int count=0;
            while(url[i]=='%') {
                int a=hex_value(url[i+1]),b=url[i+1]?hex_value(url[i+2]):-1;
                if(a<0||b<0||count==256||!((a<<4)|b)) return FALSE;
                bytes[count++]=(char)((a<<4)|b);i+=3;
            }
            int length=MultiByteToWideChar(65001,8,bytes,count,target+used,(int)(capacity-used-1));
            if(length<=0) return FALSE;
            used+=(SIZE_T)length;
        } else {if(used+1>=capacity) return FALSE;target[used++]=url[i++];}
    }
    target[used]=0;
    WCHAR normalized[SHELL_PATH_CAPACITY];
    return ShellNormalizePath(target,L"C:\\",normalized,SHELL_PATH_CAPACITY)&&ShellCopy(target,capacity,normalized);
}
static BOOL open_resolved_path(HWND owner,const WCHAR *path)
{
    DWORD attributes=GetFileAttributesW(path);
    if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)) return ShellOpenFolder(path)!=NULL;
    if(!ShellCompare(ShellExtension(path),L".exe")) return ShellLaunch(owner,path,NULL);
    SHELL_EXECUTE_INFO info;ShellZero(&info,sizeof(info));
    info.size=sizeof(info);info.flags=0x40;info.owner=owner;info.verb=L"open";info.file=path;info.show=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&info)) {ShellError(owner,L"Open",path,GetLastError());return FALSE;}
    if(info.process) CloseHandle(info.process);
    ShellLogPath("[explorer] opened ",path);return TRUE;
}
BOOL ShellOpenPath(HWND owner,const WCHAR *path)
{
    WCHAR *current=ShellDuplicate(path),**visited=NULL;SIZE_T count=0,capacity=0;
    DWORD error=current?0:8;BOOL ok=FALSE;
    while(current&&!ShellCompare(ShellExtension(current),L".url")) {
        for(SIZE_T i=0;i<count;++i)if(!ShellCompare(current,visited[i])){error=13;goto done;}
        if(!ShellReserve((void **)&visited,&capacity,count+1,sizeof(*visited))) {error=8;goto done;}
        visited[count]=ShellDuplicate(current);if(!visited[count]){error=8;goto done;}++count;
        WCHAR target[SHELL_PATH_CAPACITY];
        if(!ShellReadShortcut(current,target,SHELL_PATH_CAPACITY)){error=13;goto done;}
        WCHAR *next=ShellDuplicate(target);if(!next){error=8;goto done;}
        ShellFree(current);current=next;
    }
    if(current)ok=open_resolved_path(owner,current);
done:
    if(error)ShellError(owner,L"Open shortcut",path,error);
    ShellFree(current);for(SIZE_T i=0;i<count;++i)ShellFree(visited[i]);ShellFree(visited);return ok;
}
void ShellPaintFill(HDC dc,const RECT *rect,COLORREF color)
{ HBRUSH brush=CreateSolidBrush(color);if(brush) {FillRect(dc,rect,brush);DeleteObject(brush);} }
void ShellPaintIcon(HDC dc,int x,int y,int size,BOOL folder,BOOL application,BOOL shortcut)
{
    RECT body={x+size/8,y+size/4,x+size-size/8,y+size-size/8};
    if(folder) {
        RECT tab={x+size/8,y+size/8,x+size/2,y+size/3};ShellPaintFill(dc,&tab,RGB(227,163,28));
        ShellPaintFill(dc,&body,RGB(244,194,70));
        RECT front={body.left,body.top+size/8,body.right,body.bottom};ShellPaintFill(dc,&front,RGB(255,218,109));
    } else {
        ShellPaintFill(dc,&body,application?RGB(35,115,176):RGB(222,232,243));
        RECT inner={body.left+3,body.top+size/5,body.right-3,body.bottom-3};ShellPaintFill(dc,&inner,RGB(255,255,255));
        HPEN pen=CreatePen(PS_SOLID,1,RGB(111,133,154));HGDIOBJ old=pen?SelectObject(dc,pen):NULL;
        for(int row=0;row<3;++row) {MoveToEx(dc,inner.left+3,inner.top+5+row*5,NULL);LineTo(dc,inner.right-3,inner.top+5+row*5);}
        if(pen) {SelectObject(dc,old);DeleteObject(pen);}
    }
    if(shortcut) {RECT badge={x,y+size-13,x+14,y+size+1};ShellPaintFill(dc,&badge,RGB(255,255,255));
        HPEN pen=CreatePen(PS_SOLID,2,RGB(24,54,83));HGDIOBJ old=pen?SelectObject(dc,pen):NULL;
        MoveToEx(dc,x+3,y+size-3,NULL);LineTo(dc,x+10,y+size-10);LineTo(dc,x+10,y+size-4);
        MoveToEx(dc,x+10,y+size-10,NULL);LineTo(dc,x+4,y+size-10);if(pen){SelectObject(dc,old);DeleteObject(pen);}}
}
