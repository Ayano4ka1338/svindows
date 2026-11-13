/* Registered shell verbs and actual process creation. Association commands,
 * App Paths, environment and argv quoting are resolved before launch. */
typedef unsigned long long SEX_SIZE;
typedef long long SEX_INT64;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(HANDLE,DWORD,SEX_SIZE);
__declspec(dllimport) void *HeapReAlloc(HANDLE,DWORD,void *,SEX_SIZE);
__declspec(dllimport) BOOL HeapFree(HANDLE,DWORD,void *);
__declspec(dllimport) DWORD GetEnvironmentVariableW(const WCHAR *,WCHAR *,DWORD);
__declspec(dllimport) WCHAR *GetEnvironmentStringsW(void);
__declspec(dllimport) BOOL FreeEnvironmentStringsW(WCHAR *);
__declspec(dllimport) DWORD GetCurrentDirectoryW(DWORD,WCHAR *);
__declspec(dllimport) DWORD SearchPathW(const WCHAR *,const WCHAR *,const WCHAR *,DWORD,WCHAR *,WCHAR **);
__declspec(dllimport) DWORD GetFullPathNameW(const WCHAR *,DWORD,WCHAR *,WCHAR **);
__declspec(dllimport) DWORD GetFileAttributesW(const WCHAR *);
__declspec(dllimport) DWORD ExpandEnvironmentStringsW(const WCHAR *,WCHAR *,DWORD);
__declspec(dllimport) BOOL CloseHandle(HANDLE);
__declspec(dllimport) LONG RegOpenKeyExW(HANDLE,const WCHAR *,DWORD,DWORD,HANDLE *);
__declspec(dllimport) LONG RegQueryValueExW(HANDLE,const WCHAR *,DWORD *,DWORD *,unsigned char *,DWORD *);
__declspec(dllimport) LONG RegEnumKeyExW(HANDLE,DWORD,WCHAR *,DWORD *,DWORD *,WCHAR *,DWORD *,void *);
__declspec(dllimport) LONG RegCloseKey(HANDLE);
__declspec(dllimport) int CompareStringOrdinal(const WCHAR *,int,const WCHAR *,int,BOOL);
__declspec(dllimport) int MultiByteToWideChar(unsigned,DWORD,const char *,int,WCHAR *,int);
__declspec(dllimport) DWORD WaitForInputIdle(HANDLE,DWORD);
__declspec(dllexport) BOOL SHGetPathFromIDListW(const void *,WCHAR *);

typedef struct {
    DWORD Size;WCHAR *Reserved,*Desktop,*Title;
    DWORD X,Y,Width,Height,CharsX,CharsY,Fill,Flags;
    unsigned short Show,ReservedBytes;unsigned char *ReservedData;
    HANDLE Input,Output,Error;
} SEX_STARTUP_INFO;
typedef struct {HANDLE Process,Thread;DWORD ProcessId,ThreadId;} SEX_PROCESS_INFO;
__declspec(dllimport) BOOL CreateProcessW(const WCHAR *,WCHAR *,void *,void *,BOOL,DWORD,
    void *,const WCHAR *,SEX_STARTUP_INFO *,SEX_PROCESS_INFO *);

#define SEX_HKCR ((HANDLE)(SEX_INT64)(LONG)0x80000000UL)
#define SEX_HKCU ((HANDLE)(SEX_INT64)(LONG)0x80000001UL)
#define SEX_HKLM ((HANDLE)(SEX_INT64)(LONG)0x80000002UL)
#define SEX_KEY_READ 0x00020019UL
#define SEX_NO_ASSOC 1155UL
#define SEX_INVALID_DATA 13UL
#define SEX_MORE_DATA 234UL
#define SEX_NOCLOSEPROCESS 0x00000040UL
#define SEX_CLASSNAME 0x00000001UL
#define SEX_CLASSKEY 0x00000003UL
#define SEX_IDLIST 0x00000004UL
#define SEX_NO_UI 0x00000400UL
#define SEX_NO_CONSOLE 0x00008000UL
#define SEX_DOENVSUBST 0x00000200UL
#define SEX_WAIT_INPUT_IDLE 0x02000000UL

typedef struct {WCHAR *Text;SEX_SIZE Length,Capacity;} SEX_STRING;
static SEX_SIZE sex_length(const WCHAR *text)
{SEX_SIZE n=0;if(text)while(text[n])n++;return n;}
static BOOL sex_equal(const WCHAR *a,const WCHAR *b)
{return a && b && CompareStringOrdinal(a,-1,b,-1,1)==2;}
static WCHAR sex_lower(WCHAR value)
{return value>='A' && value<='Z' ? value+('a'-'A') : value;}
static void sex_free(void *memory)
{if(memory)HeapFree(GetProcessHeap(),0,memory);}
static BOOL sex_reserve(SEX_STRING *string,SEX_SIZE count)
{
    if(count< string->Length || count>~(SEX_SIZE)0/sizeof(WCHAR)-1){SetLastError(8);return 0;}
    SEX_SIZE wanted=count+1;if(wanted<=string->Capacity)return 1;
    SEX_SIZE capacity=string->Capacity ? string->Capacity : 64;
    while(capacity<wanted){if(capacity>~(SEX_SIZE)0/2){capacity=wanted;break;}capacity*=2;}
    if(capacity>~(SEX_SIZE)0/sizeof(WCHAR)){SetLastError(8);return 0;}
    WCHAR *memory=string->Text ? HeapReAlloc(GetProcessHeap(),0,string->Text,capacity*sizeof(WCHAR)) :
                              HeapAlloc(GetProcessHeap(),0,capacity*sizeof(WCHAR));
    if(!memory){SetLastError(8);return 0;}
    string->Text=memory;string->Capacity=capacity;string->Text[string->Length]=0;return 1;
}
static BOOL sex_append_n(SEX_STRING *string,const WCHAR *text,SEX_SIZE count)
{
    if(count>~(SEX_SIZE)0-string->Length || !sex_reserve(string,string->Length+count))return 0;
    for(SEX_SIZE i=0;i<count;++i)string->Text[string->Length+i]=text[i];
    string->Length+=count;string->Text[string->Length]=0;return 1;
}
static BOOL sex_append(SEX_STRING *string,const WCHAR *text)
{return sex_append_n(string,text,sex_length(text));}
static BOOL sex_character(SEX_STRING *string,WCHAR value)
{return sex_append_n(string,&value,1);}
static WCHAR *sex_duplicate(const WCHAR *text)
{
    SEX_STRING result={0};if(!sex_append(&result,text)){sex_free(result.Text);return 0;}return result.Text;
}
static BOOL sex_quote(SEX_STRING *string,const WCHAR *text,BOOL inside,BOOL closing_quote)
{
    if(!inside && !sex_character(string,'"'))return 0;
    const WCHAR *read=text;
    while(*read) {
        SEX_SIZE slashes=0;while(*read=='\\'){slashes++;read++;}
        SEX_SIZE emit=slashes;
        if(*read=='"')emit=slashes*2+1;
        else if(!*read && (!inside || closing_quote))emit=slashes*2;
        for(SEX_SIZE i=0;i<emit;++i)if(!sex_character(string,'\\'))return 0;
        if(*read && !sex_character(string,*read++))return 0;
    }
    return inside || sex_character(string,'"');
}
static DWORD sex_registry_string(HANDLE root,const WCHAR *path,const WCHAR *value,
    WCHAR **out,DWORD *value_type)
{
    *out=0;HANDLE key=root;BOOL close=path && *path;
    LONG status=close ? RegOpenKeyExW(root,path,0,SEX_KEY_READ,&key) : 0;
    if(status)return (DWORD)status;
    DWORD type=0,bytes=0;
    status=RegQueryValueExW(key,value,0,&type,0,&bytes);
    if(status && status!=SEX_MORE_DATA){if(close)RegCloseKey(key);return (DWORD)status;}
    if((type!=1 && type!=2) || (bytes&1)){if(close)RegCloseKey(key);return SEX_INVALID_DATA;}
    for(;;) {
        if(bytes>0xfffffffdUL){if(close)RegCloseKey(key);return 8;}
        WCHAR *data=HeapAlloc(GetProcessHeap(),8,(SEX_SIZE)bytes+sizeof(WCHAR));
        if(!data){if(close)RegCloseKey(key);return 8;}
        DWORD supplied=bytes;
        status=RegQueryValueExW(key,value,0,&type,(unsigned char *)data,&supplied);
        if(status==SEX_MORE_DATA){sex_free(data);bytes=supplied;continue;}
        if(status || (supplied&1) || (type!=1 && type!=2)) {
            sex_free(data);if(close)RegCloseKey(key);return status ? (DWORD)status : SEX_INVALID_DATA;
        }
        data[supplied/sizeof(WCHAR)]=0;
        if(close)RegCloseKey(key);*out=data;if(value_type)*value_type=type;return 0;
    }
}
static WCHAR *sex_environment(const WCHAR *name)
{
    SetLastError(0);DWORD count=GetEnvironmentVariableW(name,0,0);
    if(!count){if(GetLastError()==203)return 0;return sex_duplicate(L"");}
    for(;;) {
        WCHAR *value=HeapAlloc(GetProcessHeap(),0,(SEX_SIZE)count*sizeof(WCHAR));
        if(!value){SetLastError(8);return 0;}
        DWORD copied=GetEnvironmentVariableW(name,value,count);
        if(copied>=count){sex_free(value);count=copied+1;continue;}
        if(!copied && GetLastError()==203){sex_free(value);return 0;}
        return value;
    }
}
static WCHAR *sex_expand_environment(const WCHAR *text)
{
    DWORD count=ExpandEnvironmentStringsW(text,0,0);if(!count)return 0;
    for(;;) {
        WCHAR *result=HeapAlloc(GetProcessHeap(),0,(SEX_SIZE)count*sizeof(WCHAR));
        if(!result){SetLastError(8);return 0;}
        DWORD needed=ExpandEnvironmentStringsW(text,result,count);
        if(!needed){sex_free(result);return 0;}
        if(needed>count){sex_free(result);count=needed;continue;}
        return result;
    }
}
static WCHAR *sex_full_path(const WCHAR *path,const WCHAR *directory)
{
    SEX_STRING joined={0};BOOL absolute=path[0]=='\\' || path[0]=='/' || (path[0] && path[1]==':');
    if(!absolute && directory && *directory) {
        if(!sex_append(&joined,directory) ||
           (joined.Text[joined.Length-1]!='\\' && joined.Text[joined.Length-1]!='/' && !sex_character(&joined,'\\')))
            goto failed;
    }
    if(!sex_append(&joined,path))goto failed;
    DWORD count=GetFullPathNameW(joined.Text,0,0,0);if(!count)goto failed;
    for(;;) {
        WCHAR *result=HeapAlloc(GetProcessHeap(),0,(SEX_SIZE)count*sizeof(WCHAR));
        if(!result){SetLastError(8);goto failed;}
        DWORD needed=GetFullPathNameW(joined.Text,count,result,0);
        if(!needed){sex_free(result);goto failed;}
        if(needed>=count){sex_free(result);count=needed+1;continue;}
        sex_free(joined.Text);return result;
    }
failed:sex_free(joined.Text);return 0;
}
static BOOL sex_has_path(const WCHAR *name)
{for(SEX_SIZE i=0;name[i];++i)if(name[i]=='\\' || name[i]=='/' || name[i]==':')return 1;return 0;}
static const WCHAR *sex_extension(const WCHAR *path)
{
    const WCHAR *extension=0;
    for(SEX_SIZE i=0;path[i];++i)if(path[i]=='\\' || path[i]=='/' || path[i]==':')extension=0;
        else if(path[i]=='.')extension=path+i;
    return extension;
}
static BOOL sex_uri(const WCHAR *text,SEX_SIZE *length)
{
    if(!text || !((text[0]>='a' && text[0]<='z') || (text[0]>='A' && text[0]<='Z')))return 0;
    SEX_SIZE i=1;
    while(text[i] && ((text[i]>='a' && text[i]<='z') || (text[i]>='A' && text[i]<='Z') ||
        (text[i]>='0' && text[i]<='9') || text[i]=='+' || text[i]=='.' || text[i]=='-'))i++;
    if(text[i]!=':' || i==1)return 0;*length=i;return 1;
}
static WCHAR *sex_search(const WCHAR *file,const WCHAR *directory)
{
    DWORD count=SearchPathW(directory,file,sex_extension(file)?0:L".exe",0,0,0);
    if(!count)return 0;
    for(;;) {
        WCHAR *path=HeapAlloc(GetProcessHeap(),0,(SEX_SIZE)(count+1)*sizeof(WCHAR));
        if(!path){SetLastError(8);return 0;}
        DWORD needed=SearchPathW(directory,file,sex_extension(file)?0:L".exe",count+1,path,0);
        if(!needed){sex_free(path);return 0;}
        if(needed>count){sex_free(path);count=needed;continue;}return path;
    }
}
static WCHAR *sex_app_path(const WCHAR *file,WCHAR **extra_path)
{
    SetLastError(0);
    *extra_path=0;SEX_STRING key={0};
    if(!sex_append(&key,L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") || !sex_append(&key,file) ||
       (!sex_extension(file) && !sex_append(&key,L".exe")))goto failed;
    const HANDLE roots[]={SEX_HKCU,SEX_HKLM};
    for(unsigned i=0;i<2;++i) {
        WCHAR *registered=0;DWORD type=0;
        DWORD status=sex_registry_string(roots[i],key.Text,L"",&registered,&type);
        if(status==2 || status==3)continue;
        if(status){SetLastError(status);goto failed;}
        WCHAR *path=type==2 ? sex_expand_environment(registered) : sex_duplicate(registered);
        sex_free(registered);if(!path)goto failed;
        if(GetFileAttributesW(path)==0xffffffffUL){sex_free(path);continue;}
        WCHAR *extra=0;
        DWORD extra_status=sex_registry_string(roots[i],key.Text,L"Path",&extra,&type);
        if(!extra_status) {
            *extra_path=type==2 ? sex_expand_environment(extra) : sex_duplicate(extra);
            sex_free(extra);
            if(!*extra_path){sex_free(path);goto failed;}
        } else if(extra_status!=2 && extra_status!=3){sex_free(path);SetLastError(extra_status);goto failed;}
        sex_free(key.Text);return path;
    }
failed:sex_free(key.Text);return 0;
}
static WCHAR *sex_resolve_executable(const WCHAR *file,const WCHAR *directory,WCHAR **extra)
{
    *extra=0;
    if(sex_has_path(file))return sex_full_path(file,directory);
    WCHAR *registered=sex_app_path(file,extra);if(registered)return registered;
    if(GetLastError()==8 || GetLastError()==SEX_INVALID_DATA)return 0;
    WCHAR *path=directory && *directory ? sex_search(file,directory) : 0;
    return path ? path : sex_search(file,0);
}
static DWORD sex_class_value(const WCHAR *class_name,const WCHAR *tail,const WCHAR *value,WCHAR **out)
{
    SEX_STRING path={0};DWORD status=8;
    if(!sex_append(&path,L"Software\\Classes\\") || !sex_append(&path,class_name) ||
       (tail && *tail && (!sex_character(&path,'\\') || !sex_append(&path,tail))))goto done;
    status=sex_registry_string(SEX_HKCU,path.Text,value,out,0);
    if(status==2 || status==3) {
        sex_free(path.Text);path=(SEX_STRING){0};
        if(!sex_append(&path,class_name) ||
           (tail && *tail && (!sex_character(&path,'\\') || !sex_append(&path,tail)))){status=8;goto done;}
        status=sex_registry_string(SEX_HKCR,path.Text,value,out,0);
    }
done:sex_free(path.Text);return status;
}
static WCHAR *sex_association_class(const WCHAR *extension)
{
    SetLastError(0);
    SEX_STRING key={0};WCHAR *class_name=0;
    if(!sex_append(&key,L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\") ||
       !sex_append(&key,extension) || !sex_append(&key,L"\\UserChoice"))goto done;
    DWORD status=sex_registry_string(SEX_HKCU,key.Text,L"ProgId",&class_name,0);
    if(!status && *class_name)goto done;
    if(status && status!=2 && status!=3){SetLastError(status);goto done;}
    sex_free(class_name);class_name=0;
    status=sex_class_value(extension,0,L"",&class_name);
    if(status || !class_name || !*class_name){sex_free(class_name);class_name=0;if(status!=2 && status!=3)SetLastError(status);}
done:sex_free(key.Text);return class_name;
}
static DWORD sex_verb_command(const WCHAR *class_name,HANDLE class_key,const WCHAR *requested,WCHAR **command)
{
    WCHAR *verb=0;DWORD status=0;SEX_STRING tail={0};
    if(requested && *requested)verb=sex_duplicate(requested);
    else {
        status=class_key ? sex_registry_string(class_key,L"shell",L"",&verb,0) :
                          sex_class_value(class_name,L"shell",L"",&verb);
        if(status && status!=2 && status!=3)return status;
        if(status || !verb || !*verb){sex_free(verb);verb=sex_duplicate(L"open");}
    }
    if(!verb)return 8;
    if(!sex_append(&tail,L"shell\\") || !sex_append(&tail,verb) || !sex_append(&tail,L"\\command")){status=8;goto done;}
    status=class_key ? sex_registry_string(class_key,tail.Text,L"",command,0) :
                      sex_class_value(class_name,tail.Text,L"",command);
    if(status && (!requested || !*requested) && sex_equal(verb,L"open")) {
        /* A class can publish a default verb without naming it in shell's
         * default value. Enumerate that class's actual registered children. */
        const HANDLE roots[]={SEX_HKCU,SEX_HKCR};
        for(unsigned root=0;root<2;++root) {
            SEX_STRING key={0};HANDLE opened=0;
            if(root==0 && !sex_append(&key,L"Software\\Classes\\")){status=8;break;}
            if(!sex_append(&key,class_name) || !sex_append(&key,L"\\shell")){sex_free(key.Text);status=8;break;}
            LONG error=class_key ? RegOpenKeyExW(class_key,L"shell",0,SEX_KEY_READ,&opened) :
                                 RegOpenKeyExW(roots[root],key.Text,0,SEX_KEY_READ,&opened);
            sex_free(key.Text);if(error){if(class_key)break;continue;}
            WCHAR name[256];DWORD chars=256;
            error=RegEnumKeyExW(opened,0,name,&chars,0,0,0,0);RegCloseKey(opened);
            if(error){if(class_key)break;continue;}
            sex_free(tail.Text);tail=(SEX_STRING){0};
            if(!sex_append(&tail,L"shell\\") || !sex_append(&tail,name) || !sex_append(&tail,L"\\command")){status=8;break;}
            status=class_key ? sex_registry_string(class_key,tail.Text,L"",command,0) :
                              sex_class_value(class_name,tail.Text,L"",command);
            if(!status || class_key)break;
        }
    }
done:sex_free(verb);sex_free(tail.Text);return status;
}
static BOOL sex_build_command(const WCHAR *format,const WCHAR *file,const WCHAR *parameters,
    const WCHAR *directory,SEX_STRING *command)
{
    BOOL quoted=0;WCHAR **arguments=0;int argument_count=0;SEX_STRING parameter_line={0};
    for(SEX_SIZE i=0;format[i];++i) {
        WCHAR value=format[i];
        if(value=='%') {
            WCHAR token=format[i+1];
            if(token=='1' || token=='l' || token=='L' || token=='v' || token=='V') {
                if(!sex_quote(command,file,quoted,format[i+2]=='"'))goto failed;i++;continue;
            }
            if(token=='d' || token=='D') {
                if(!sex_quote(command,directory ? directory : L"",quoted,format[i+2]=='"'))goto failed;i++;continue;
            }
            if(token=='*'){if(!sex_append(command,parameters ? parameters : L""))goto failed;i++;continue;}
            if(token>='2' && token<='9') {
                if(!arguments) {
                    if(!sex_append(&parameter_line,L"shell ") || !sex_append(&parameter_line,parameters ? parameters : L""))goto failed;
                    arguments=CommandLineToArgvW(parameter_line.Text,&argument_count);if(!arguments)goto failed;
                }
                unsigned index=(unsigned)(token-'1');
                if(index<(unsigned)argument_count && !sex_quote(command,arguments[index],quoted,format[i+2]=='"'))goto failed;
                i++;continue;
            }
            SEX_SIZE end=i+1;while(format[end] && format[end]!='%')end++;
            if(format[end]=='%' && end>i+1) {
                SEX_STRING name={0};if(!sex_append_n(&name,format+i+1,end-i-1))goto failed;
                WCHAR *environment=sex_environment(name.Text);sex_free(name.Text);
                if(!environment && GetLastError()==8)goto failed;
                if(environment){BOOL appended=sex_append(command,environment);sex_free(environment);if(!appended)goto failed;i=end;continue;}
            }
        }
        if(value=='"') {
            SEX_SIZE slashes=0,previous=i;while(previous && format[previous-1]=='\\'){slashes++;previous--;}
            if(!(slashes&1))quoted=!quoted;
        }
        if(!sex_character(command,value))goto failed;
    }
    if(arguments)LocalFree(arguments);sex_free(parameter_line.Text);return command->Length!=0;
failed:if(arguments)LocalFree(arguments);sex_free(parameter_line.Text);return 0;
}
static WCHAR *sex_child_environment(const WCHAR *extra)
{
    if(!extra || !*extra)return 0;
    WCHAR *source=GetEnvironmentStringsW();if(!source)return 0;
    SEX_STRING result={0};BOOL replaced=0;
    for(const WCHAR *entry=source;*entry;) {
        SEX_SIZE length=sex_length(entry);
        BOOL path=length>=5 && sex_lower(entry[0])=='p' && sex_lower(entry[1])=='a' &&
                  sex_lower(entry[2])=='t' && sex_lower(entry[3])=='h' && entry[4]=='=';
        if(path) {
            if(!sex_append(&result,L"PATH=") || !sex_append(&result,entry+5) ||
               (entry[5] && !sex_character(&result,';')) || !sex_append(&result,extra))goto failed;
            replaced=1;
        } else if(!sex_append_n(&result,entry,length))goto failed;
        if(!sex_character(&result,0))goto failed;entry+=length+1;
    }
    if(!replaced && (!sex_append(&result,L"PATH=") || !sex_append(&result,extra) || !sex_character(&result,0)))goto failed;
    if(!sex_character(&result,0))goto failed;
    FreeEnvironmentStringsW(source);return result.Text;
failed:FreeEnvironmentStringsW(source);sex_free(result.Text);return 0;
}
static HANDLE sex_failure_instance(DWORD error)
{
    DWORD legacy=error==2 || error==3 || error==5 || error==8 ? error :
        error==193 || error==11 ? 11 : error==32 ? 26 : error==SEX_NO_ASSOC ? 31 :
        error==1157 ? 32 : error==1156 ? 29 : 27;
    return (HANDLE)(unsigned long long)legacy;
}

__declspec(dllexport) BOOL ShellExecuteExW(SHELLEXECUTEINFOW *info)
{
    if(!info || info->cbSize<sizeof(*info)){SetLastError(87);return 0;}
    info->hProcess=0;info->hInstApp=0;
    DWORD error=0,attributes=0xffffffffUL;BOOL direct=0;
    WCHAR *input=0,*file=0,*directory=0,*extra=0,*class_name=0,*format=0,*environment=0;
    WCHAR *application=0,*application_extra=0;WCHAR **arguments=0;int argument_count=0;
    SEX_STRING command={0};SEX_PROCESS_INFO process={0};
    if(info->lpDirectory && *info->lpDirectory) {
        WCHAR *expanded=info->fMask&SEX_DOENVSUBST ? sex_expand_environment(info->lpDirectory) : sex_duplicate(info->lpDirectory);
        if(!expanded)goto memory_failure;
        directory=sex_full_path(expanded,0);sex_free(expanded);
        if(!directory){error=GetLastError();goto done;}
        DWORD attr=GetFileAttributesW(directory);if(attr==0xffffffffUL || !(attr&0x10)){error=3;goto done;}
    }
    if(info->lpFile && *info->lpFile)input=info->fMask&SEX_DOENVSUBST ? sex_expand_environment(info->lpFile) : sex_duplicate(info->lpFile);
    else if((info->fMask&SEX_IDLIST) && info->lpIDList) {
        WCHAR path[MAX_PATH];if(!SHGetPathFromIDListW(info->lpIDList,path)){error=87;goto done;}input=sex_duplicate(path);
    } else {error=87;goto done;}
    if(!input)goto memory_failure;
    SEX_SIZE length=sex_length(input);
    if(length>=2 && input[0]=='"' && input[length-1]=='"') {
        for(SEX_SIZE i=0;i+2<length;++i)input[i]=input[i+1];input[length-2]=0;
    }
    if(!*input){error=87;goto done;}
    SEX_SIZE scheme_length=0;BOOL uri=sex_uri(input,&scheme_length);
    if(uri){file=sex_duplicate(input);if(!file)goto memory_failure;SEX_STRING scheme={0};if(!sex_append_n(&scheme,input,scheme_length))goto memory_failure;class_name=scheme.Text;}
    else {
        file=sex_full_path(input,directory);
        if(file)attributes=GetFileAttributesW(file);
        if(attributes==0xffffffffUL && !sex_has_path(input)) {
            sex_free(file);file=sex_resolve_executable(input,directory,&extra);
            if(file)attributes=GetFileAttributesW(file);
        }
        if(!file || attributes==0xffffffffUL){error=GetLastError();if(!error)error=2;goto done;}
        const WCHAR *extension=sex_extension(file);
        direct=!(attributes&0x10) && (sex_equal(extension,L".exe") || sex_equal(extension,L".com") ||
                                    sex_equal(extension,L".bat") || sex_equal(extension,L".cmd"));
        if(attributes&0x10)class_name=sex_duplicate(L"Directory");
        else if(extension)class_name=sex_association_class(extension);
        if(!class_name && GetLastError()==8){error=8;goto done;}
    }
    HANDLE class_key=0;
    if((info->fMask&SEX_CLASSKEY)==SEX_CLASSKEY) {
        if(!info->hkeyClass){error=6;goto done;}class_key=info->hkeyClass;
    } else if(info->fMask&SEX_CLASSNAME) {
        if(!info->lpClass || !*info->lpClass){error=87;goto done;}
        sex_free(class_name);class_name=sex_duplicate(info->lpClass);if(!class_name)goto memory_failure;
    }
    if(class_key || class_name) {
        DWORD status=sex_verb_command(class_name,class_key,info->lpVerb,&format);
        if(status && !((status==2 || status==3) && direct && !(info->fMask&SEX_CLASSNAME) &&
            (!info->lpVerb || !*info->lpVerb || sex_equal(info->lpVerb,L"open")))) {
            error=status==2 || status==3 ? SEX_NO_ASSOC : status;goto done;
        }
    }
    if(format) {
        if(!*format){error=SEX_INVALID_DATA;goto done;}
        if(!sex_build_command(format,file,info->lpParameters,directory,&command))goto memory_failure;
    } else if(direct && (!info->lpVerb || !*info->lpVerb || sex_equal(info->lpVerb,L"open"))) {
        if(!sex_quote(&command,file,0,0) ||
           (info->lpParameters && *info->lpParameters && (!sex_character(&command,' ') || !sex_append(&command,info->lpParameters))))goto memory_failure;
    } else {error=SEX_NO_ASSOC;goto done;}
    arguments=CommandLineToArgvW(command.Text,&argument_count);
    if(!arguments || !argument_count || !*arguments[0]){error=87;goto done;}
    application=sex_resolve_executable(arguments[0],directory,&application_extra);
    if(!application || GetFileAttributesW(application)==0xffffffffUL){error=GetLastError();if(!error)error=2;goto done;}
    const WCHAR *app_extension=sex_extension(application);
    if(sex_equal(app_extension,L".bat") || sex_equal(app_extension,L".cmd")) {
        WCHAR *processor_extra=0;
        WCHAR *processor=sex_environment(L"COMSPEC");if(!processor)processor=sex_resolve_executable(L"cmd.exe",directory,&processor_extra);
        if(processor_extra){sex_free(application_extra);application_extra=processor_extra;}
        if(!processor){error=2;goto done;}
        SEX_STRING batch={0};
        BOOL built=sex_quote(&batch,processor,0,0) && sex_append(&batch,L" /c ") && sex_append(&batch,command.Text);
        sex_free(application);application=processor;
        if(!built){sex_free(batch.Text);goto memory_failure;}
        sex_free(command.Text);command=batch;
    }
    const WCHAR *child_path=application_extra && *application_extra ? application_extra : extra;
    if(child_path && *child_path){environment=sex_child_environment(child_path);if(!environment)goto memory_failure;}
    SEX_STARTUP_INFO startup={0};startup.Size=sizeof(startup);startup.Flags=1;startup.Show=(unsigned short)info->nShow;
    DWORD flags=environment ? 0x400UL : 0;
    if(info->fMask&SEX_NO_CONSOLE)flags|=0x08000000UL;
    if(!CreateProcessW(application,command.Text,0,0,0,flags,environment,directory,&startup,&process)) {
        error=GetLastError();if(!error)error=31;goto done;
    }
    if(!process.Process){if(process.Thread)CloseHandle(process.Thread);error=6;goto done;}
    if(process.Thread)CloseHandle(process.Thread);
    if(info->fMask&SEX_WAIT_INPUT_IDLE)WaitForInputIdle(process.Process,60000);
    if(info->fMask&SEX_NOCLOSEPROCESS)info->hProcess=process.Process;
    else if(process.Process)CloseHandle(process.Process);
    /* hInstApp is the documented legacy status result, not a process handle. */
    info->hInstApp=(HANDLE)(unsigned long long)33;
    goto done;
memory_failure:error=GetLastError();if(!error || error==203)error=8;
done:
    if(arguments)LocalFree(arguments);sex_free(command.Text);sex_free(input);sex_free(file);sex_free(directory);
    sex_free(extra);sex_free(class_name);sex_free(format);sex_free(environment);sex_free(application);sex_free(application_extra);
    if(error){info->hInstApp=sex_failure_instance(error);SetLastError(error);return 0;}
    return info->hInstApp!=0;
}

typedef struct {
    DWORD cbSize,fMask;HWND hwnd;
    const char *lpVerb,*lpFile,*lpParameters,*lpDirectory;
    int nShow;HANDLE hInstApp;void *lpIDList;const char *lpClass;HANDLE hkeyClass;
    DWORD dwHotKey;union{HANDLE hIcon;HANDLE hMonitor;}DUMMYUNIONNAME;HANDLE hProcess;
} SHELLEXECUTEINFOA;
static WCHAR *sex_ansi(const char *text)
{
    if(!text)return 0;int count=MultiByteToWideChar(0,0,text,-1,0,0);if(!count)return 0;
    WCHAR *wide=HeapAlloc(GetProcessHeap(),0,(SEX_SIZE)count*sizeof(WCHAR));
    if(!wide){SetLastError(8);return 0;}
    if(!MultiByteToWideChar(0,0,text,-1,wide,count)){sex_free(wide);return 0;}return wide;
}
__declspec(dllexport) BOOL ShellExecuteExA(SHELLEXECUTEINFOA *info)
{
    if(!info || info->cbSize<sizeof(*info)){SetLastError(87);return 0;}
    WCHAR *verb=sex_ansi(info->lpVerb),*file=sex_ansi(info->lpFile),*parameters=sex_ansi(info->lpParameters);
    WCHAR *directory=sex_ansi(info->lpDirectory),*class_name=sex_ansi(info->lpClass);
    if((info->lpVerb && !verb) || (info->lpFile && !file) || (info->lpParameters && !parameters) ||
       (info->lpDirectory && !directory) || (info->lpClass && !class_name)) {
        sex_free(verb);sex_free(file);sex_free(parameters);sex_free(directory);sex_free(class_name);return 0;
    }
    SHELLEXECUTEINFOW wide={0};wide.cbSize=sizeof(wide);wide.fMask=info->fMask;wide.hwnd=info->hwnd;
    wide.lpVerb=verb;wide.lpFile=file;wide.lpParameters=parameters;wide.lpDirectory=directory;wide.nShow=info->nShow;
    wide.lpIDList=info->lpIDList;wide.lpClass=class_name;wide.hkeyClass=info->hkeyClass;wide.dwHotKey=info->dwHotKey;
    wide.DUMMYUNIONNAME.hIcon=info->DUMMYUNIONNAME.hIcon;
    BOOL result=ShellExecuteExW(&wide);DWORD error=GetLastError();info->hInstApp=wide.hInstApp;info->hProcess=wide.hProcess;
    sex_free(verb);sex_free(file);sex_free(parameters);sex_free(directory);sex_free(class_name);SetLastError(error);return result;
}
__declspec(dllexport) HANDLE ShellExecuteW(HWND owner,const WCHAR *verb,const WCHAR *file,
    const WCHAR *parameters,const WCHAR *directory,int show)
{
    SHELLEXECUTEINFOW info={0};info.cbSize=sizeof(info);info.hwnd=owner;info.lpVerb=verb;info.lpFile=file;
    info.lpParameters=parameters;info.lpDirectory=directory;info.nShow=show;ShellExecuteExW(&info);return info.hInstApp;
}
__declspec(dllexport) HANDLE ShellExecuteA(HWND owner,const char *verb,const char *file,
    const char *parameters,const char *directory,int show)
{
    SHELLEXECUTEINFOA info={0};info.cbSize=sizeof(info);info.hwnd=owner;info.lpVerb=verb;info.lpFile=file;
    info.lpParameters=parameters;info.lpDirectory=directory;info.nShow=show;ShellExecuteExA(&info);return info.hInstApp;
}
