/* Filesystem PIDLs are opaque ITEMIDLIST chains. Each component has a valid
 * SHITEMID byte length, captured attributes and a terminated UTF-16 name. */
__declspec(dllimport) void *CoTaskMemAlloc(unsigned long long);
__declspec(dllimport) void CoTaskMemFree(void *);
__declspec(dllimport) DWORD GetCurrentProcessId(void);
__declspec(dllimport) HANDLE CreateEventW(void *,BOOL,BOOL,const WCHAR *);
__declspec(dllimport) DWORD WaitForMultipleObjects(DWORD,const HANDLE *,BOOL,DWORD);
__declspec(dllimport) BOOL GetExitCodeProcess(HANDLE,DWORD *);
__declspec(dllexport) HRESULT SHGetFolderPathW(HWND,int,HANDLE,DWORD,WCHAR *);
#define SEX_PIDL_HEADER 8u
#define SEX_PIDL_SIGNATURE 0x53u
#define SEX_PIDL_ROOT 1u
#define SEX_PIDL_CHILD 2u
#define SEX_PIDL_MAX_BYTES 262144u
static unsigned sex_pidl_word(const unsigned char *bytes)
{return bytes[0]|((unsigned)bytes[1]<<8);}
static void sex_pidl_store_word(unsigned char *bytes,unsigned value)
{bytes[0]=(unsigned char)value;bytes[1]=(unsigned char)(value>>8);}
static unsigned sex_pidl_size(const void *id)
{
    if(!id)return 0;const unsigned char *bytes=id;unsigned used=0;
    while(used+2<=SEX_PIDL_MAX_BYTES) {
        unsigned size=sex_pidl_word(bytes+used);
        if(!size)return used+2;
        if(size<2 || size>SEX_PIDL_MAX_BYTES-used)return 0;
        used+=size;
    }
    return 0;
}
static void sex_pidl_component(unsigned char *out,const WCHAR *name,SEX_SIZE length,
    unsigned kind,DWORD attributes)
{
    unsigned size=SEX_PIDL_HEADER+(unsigned)(length+1)*2;
    sex_pidl_store_word(out,size);out[2]=SEX_PIDL_SIGNATURE;out[3]=(unsigned char)kind;
    for(unsigned i=0;i<4;++i)out[4+i]=(unsigned char)(attributes>>(i*8));
    for(SEX_SIZE i=0;i<length;++i)sex_pidl_store_word(out+SEX_PIDL_HEADER+i*2,name[i]);
    sex_pidl_store_word(out+SEX_PIDL_HEADER+length*2,0);
}
static HRESULT sex_filesystem_parse(const WCHAR *name,void **out,unsigned requested,
    unsigned *attributes)
{
    if(!out)return (HRESULT)0x80070057UL;*out=0;
    if(attributes)*attributes=0;if(!name || !*name)return (HRESULT)0x80070057UL;
    WCHAR *path=sex_full_path(name,0);if(!path)return (HRESULT)(0x80070000UL|(GetLastError()&0xffff));
    DWORD file_attributes=GetFileAttributesW(path);
    if(file_attributes==0xffffffffUL){DWORD error=GetLastError();sex_free(path);return (HRESULT)(0x80070000UL|(error&0xffff));}
    SEX_SIZE root=0;
    if(path[0] && path[1]==':' && (path[2]=='\\' || path[2]=='/'))root=2;
    else if(path[0]=='\\' && path[1]=='\\') {
        root=2;while(path[root] && path[root]!='\\')root++;
        if(path[root])root++;while(path[root] && path[root]!='\\')root++;
    }
    if(!root){sex_free(path);return (HRESULT)0x80070057UL;}
    SEX_SIZE total=SEX_PIDL_HEADER+(root+1)*2+2;
    for(SEX_SIZE i=root;path[i];) {
        while(path[i]=='\\' || path[i]=='/')i++;if(!path[i])break;
        SEX_SIZE start=i;while(path[i] && path[i]!='\\' && path[i]!='/')i++;
        SEX_SIZE bytes=SEX_PIDL_HEADER+(i-start+1)*2;
        if(bytes>65535 || total>SEX_PIDL_MAX_BYTES-bytes){sex_free(path);return (HRESULT)0x800700ceUL;}
        total+=bytes;
    }
    unsigned char *pidl=CoTaskMemAlloc(total);if(!pidl){sex_free(path);return (HRESULT)0x8007000eUL;}
    sex_pidl_component(pidl,path,root,SEX_PIDL_ROOT,0x10);
    SEX_SIZE offset=sex_pidl_word(pidl);
    for(SEX_SIZE i=root;path[i];) {
        while(path[i]=='\\' || path[i]=='/')i++;if(!path[i])break;
        SEX_SIZE start=i;while(path[i] && path[i]!='\\' && path[i]!='/')i++;
        SEX_SIZE next=i;while(path[next]=='\\' || path[next]=='/')next++;
        sex_pidl_component(pidl+offset,path+start,i-start,SEX_PIDL_CHILD,path[next]?0x10:file_attributes);
        offset+=sex_pidl_word(pidl+offset);
    }
    sex_pidl_store_word(pidl+offset,0);sex_free(path);*out=pidl;
    unsigned capabilities=0x40000000u|1u|2u|4u|0x10u|0x20u;
    if(file_attributes&0x10)capabilities|=0x20000000u|0x08000000u;
    if(file_attributes&1)capabilities|=0x00040000u;
    if(attributes)*attributes=capabilities&requested;
    return S_OK;
}
__declspec(dllexport) HRESULT SHParseDisplayName(const WCHAR *name,void *context,
    void **pidl,unsigned attributes,unsigned *result_attributes)
{
    (void)context;return sex_filesystem_parse(name,pidl,attributes,result_attributes);
}
static WCHAR *sex_filesystem_path(const void *id)
{
    unsigned size=sex_pidl_size(id);if(!size){SetLastError(13);return 0;}
    const unsigned char *bytes=id;
    if(!sex_pidl_word(bytes)) {
        WCHAR desktop[MAX_PATH];HRESULT status=SHGetFolderPathW(0,0x10,0,0,desktop);
        if(status<0)return 0;return sex_duplicate(desktop);
    }
    SEX_STRING path={0};unsigned used=0;BOOL first=1;
    while(sex_pidl_word(bytes+used)) {
        unsigned item=sex_pidl_word(bytes+used);unsigned kind=bytes[used+3];
        if(item<SEX_PIDL_HEADER+2 || (item&1) || bytes[used+2]!=SEX_PIDL_SIGNATURE)goto invalid;
        if((first && kind!=SEX_PIDL_ROOT) || (!first && kind!=SEX_PIDL_CHILD))goto invalid;
        unsigned words=(item-SEX_PIDL_HEADER)/2;
        if(!first && path.Length && path.Text[path.Length-1]!='\\' && !sex_character(&path,'\\'))goto failed;
        unsigned copied=0;BOOL terminated=0;
        for(unsigned i=0;i<words;++i) {
            WCHAR ch=(WCHAR)sex_pidl_word(bytes+used+SEX_PIDL_HEADER+i*2);if(!ch){terminated=1;break;}
            if(!first && (ch=='\\' || ch=='/' || ch==':'))goto invalid;
            if(!sex_character(&path,ch))goto failed;copied++;
        }
        if(!copied || !terminated)goto invalid;
        if(first && path.Text[path.Length-1]!='\\' && !sex_character(&path,'\\'))goto failed;
        first=0;used+=item;
    }
    return path.Text;
invalid:SetLastError(13);
failed:sex_free(path.Text);return 0;
}
__declspec(dllexport) BOOL SHGetPathFromIDListW(const void *id,WCHAR *out)
{
    if(!out){SetLastError(87);return 0;}out[0]=0;
    WCHAR *path=sex_filesystem_path(id);if(!path)return 0;
    if(sex_length(path)>=MAX_PATH){sex_free(path);SetLastError(206);return 0;}
    BOOL copied=copy_path(out,path)!=0;sex_free(path);return copied;
}
__declspec(dllexport) BOOL SHGetPathFromIDListEx(const void *id,WCHAR *out,DWORD capacity,DWORD flags)
{
    if(!out || !capacity || flags){SetLastError(87);return 0;}out[0]=0;
    WCHAR *path=sex_filesystem_path(id);if(!path)return 0;SEX_SIZE length=sex_length(path);
    if(length>=capacity){sex_free(path);SetLastError(122);return 0;}
    for(SEX_SIZE i=0;i<=length;++i)out[i]=path[i];sex_free(path);return 1;
}
__declspec(dllexport) unsigned ILGetSize(const void *id)
{return sex_pidl_size(id);}
__declspec(dllexport) BOOL ILIsEmpty(const void *id)
{return !id || !sex_pidl_word(id);}
__declspec(dllexport) void *ILClone(const void *id)
{
    unsigned size=sex_pidl_size(id);if(!size){SetLastError(13);return 0;}
    unsigned char *copy=CoTaskMemAlloc(size);if(!copy){SetLastError(8);return 0;}
    const unsigned char *bytes=id;for(unsigned i=0;i<size;++i)copy[i]=bytes[i];return copy;
}
__declspec(dllexport) void *ILCloneFull(const void *id)
{return ILClone(id);}
__declspec(dllexport) void ILFree(void *id)
{CoTaskMemFree(id);}
__declspec(dllexport) void *ILFindLastID(const void *id)
{
    unsigned size=sex_pidl_size(id);if(!size)return 0;
    const unsigned char *bytes=id;unsigned offset=0,last=0;
    while(sex_pidl_word(bytes+offset)){last=offset;offset+=sex_pidl_word(bytes+offset);}
    return (void *)(bytes+last);
}
__declspec(dllexport) BOOL ILRemoveLastID(void *id)
{
    void *last=ILFindLastID(id);if(!last || !sex_pidl_word(last))return 0;
    sex_pidl_store_word(last,0);return 1;
}
__declspec(dllexport) void *ILCombine(const void *parent,const void *child)
{
    if(!parent)return child ? ILClone(child) : 0;if(!child)return ILClone(parent);
    unsigned a=sex_pidl_size(parent),b=sex_pidl_size(child);
    if(!a || !b || a>SEX_PIDL_MAX_BYTES-b+2){SetLastError(13);return 0;}
    unsigned char *out=CoTaskMemAlloc((SEX_SIZE)a+b-2);if(!out){SetLastError(8);return 0;}
    const unsigned char *first=parent,*second=child;
    for(unsigned i=0;i<a-2;++i)out[i]=first[i];for(unsigned i=0;i<b;++i)out[a-2+i]=second[i];return out;
}
__declspec(dllexport) BOOL ILIsEqual(const void *first,const void *second)
{
    if(ILIsEmpty(first) && ILIsEmpty(second))return 1;
    unsigned a=sex_pidl_size(first),b=sex_pidl_size(second);if(!a || a!=b)return 0;
    const unsigned char *x=first,*y=second;for(unsigned i=0;i<a;++i)if(x[i]!=y[i])return 0;return 1;
}
__declspec(dllexport) void *ILCreateFromPathW(const WCHAR *path)
{
    void *pidl=0;HRESULT status=sex_filesystem_parse(path,&pidl,0,0);
    if(status<0)SetLastError((DWORD)status&0xffff);return pidl;
}
__declspec(dllexport) void *ILCreateFromPathA(const char *path)
{
    WCHAR *wide=sex_ansi(path);if(!wide)return 0;void *pidl=ILCreateFromPathW(wide);sex_free(wide);return pidl;
}
__declspec(dllexport) HRESULT SHCreateItemFromIDList(const void *pidl,const SHELL_GUID *iid,void **out)
{
    WCHAR *path=sex_filesystem_path(pidl);if(!path){if(out)*out=0;return (HRESULT)0x80070057UL;}
    HRESULT status=SHCreateItemFromParsingName(path,0,iid,out);sex_free(path);return status;
}
static volatile DWORD sex_selection_sequence;
static BOOL sex_selection_number(SEX_STRING *text,DWORD value)
{
    WCHAR digits[10];unsigned count=0;do{digits[count++]=(WCHAR)('0'+value%10);value/=10;}while(value);
    while(count)if(!sex_character(text,digits[--count]))return 0;return 1;
}
__declspec(dllexport) HRESULT SHOpenFolderAndSelectItems(const void *folder,unsigned count,
    const void *const *children,DWORD flags)
{
    if(!folder || (count&&!children) || (flags&~3UL))return (HRESULT)0x80070057UL;
    WCHAR *directory=0;WCHAR **paths=0;WCHAR *windows=0;SEX_STRING application={0},command={0},event_name={0};
    HANDLE ready=0;SEX_PROCESS_INFO process={0};DWORD error=0;
    unsigned selected=count?count:1;
    paths=HeapAlloc(GetProcessHeap(),8,(SEX_SIZE)selected*sizeof(*paths));if(!paths){error=8;goto done;}
    if(!count) {
        paths[0]=sex_filesystem_path(folder);if(!paths[0]){error=13;goto done;}
        void *parent=ILClone(folder);if(!parent){error=8;goto done;}
        if(!ILRemoveLastID(parent)){ILFree(parent);error=87;goto done;}
        directory=sex_filesystem_path(parent);ILFree(parent);
    } else {
        directory=sex_filesystem_path(folder);
        for(unsigned i=0;directory&&i<count;++i) {
            if(!children[i]){error=87;goto done;}
            const unsigned char *child=children[i];
            if(sex_pidl_word(child)>=SEX_PIDL_HEADER && child[3]==SEX_PIDL_ROOT)paths[i]=sex_filesystem_path(child);
            else {void *combined=ILCombine(folder,child);if(!combined){error=13;goto done;}paths[i]=sex_filesystem_path(combined);ILFree(combined);}
            if(!paths[i]){error=13;goto done;}
        }
    }
    if(!directory){error=13;goto done;}
    DWORD attributes=GetFileAttributesW(directory);if(attributes==0xffffffffUL || !(attributes&0x10)){error=267;goto done;}
    windows=sex_system_directory(FALSE);if(!windows){error=GetLastError();goto done;}
    if(!sex_append(&application,windows) || !sex_append(&application,L"\\EXPLORER.EXE")){error=8;goto done;}
    for(;;) {
        event_name.Length=0;if(event_name.Text)event_name.Text[0]=0;
        DWORD sequence=__atomic_add_fetch(&sex_selection_sequence,1,__ATOMIC_RELAXED);
        if(!sex_append(&event_name,L"Local\\Svindows.FolderSelect.") ||
           !sex_selection_number(&event_name,GetCurrentProcessId()) || !sex_character(&event_name,'.') ||
           !sex_selection_number(&event_name,sequence)){error=8;goto done;}
        ready=CreateEventW(0,TRUE,FALSE,event_name.Text);if(!ready){error=GetLastError();goto done;}
        if(GetLastError()!=183)break;CloseHandle(ready);ready=0;
    }
    if(!sex_quote(&command,application.Text,FALSE,FALSE) || !sex_append(&command,L" /folder ") ||
       !sex_quote(&command,directory,FALSE,FALSE) || !sex_append(&command,L" /select")){error=8;goto done;}
    for(unsigned i=0;i<selected;++i)
        if(!sex_character(&command,' ') || !sex_quote(&command,paths[i],FALSE,FALSE)){error=8;goto done;}
    if((flags&1)&&selected==1&&!sex_append(&command,L" /edit")){error=8;goto done;}
    if(!sex_append(&command,L" /ready-event ") || !sex_quote(&command,event_name.Text,FALSE,FALSE)){error=8;goto done;}
    SEX_STARTUP_INFO startup={0};startup.Size=sizeof(startup);startup.Flags=1;startup.Show=1;
    if(!CreateProcessW(application.Text,command.Text,0,0,FALSE,0,0,directory,&startup,&process)){error=GetLastError();goto done;}
    if(process.Thread){CloseHandle(process.Thread);process.Thread=0;}
    HANDLE waits[2]={ready,process.Process};DWORD waited=WaitForMultipleObjects(2,waits,FALSE,30000);
    if(waited==1){DWORD code=0;GetExitCodeProcess(process.Process,&code);error=code?code:1067;}
    else if(waited==0x102)error=1460;
    else if(waited!=(DWORD)0){error=GetLastError();if(!error)error=31;}
done:
    if(process.Thread)CloseHandle(process.Thread);if(process.Process)CloseHandle(process.Process);if(ready)CloseHandle(ready);
    if(paths){for(unsigned i=0;i<selected;++i)sex_free(paths[i]);sex_free(paths);}
    sex_free(directory);sex_free(windows);sex_free(application.Text);sex_free(command.Text);sex_free(event_name.Text);
    if(error){SetLastError(error);return (HRESULT)(0x80070000UL|(error&0xffff));}return S_OK;
}
