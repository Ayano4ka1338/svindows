/* Folder paths follow the selected user's registered profile and shell-folder
 * policy. Missing paths are created only when the caller requests creation. */
__declspec(dllimport) HANDLE GetCurrentProcess(void);
__declspec(dllimport) BOOL OpenProcessToken(HANDLE,DWORD,HANDLE *);
__declspec(dllimport) BOOL GetTokenInformation(HANDLE,int,void *,DWORD,DWORD *);
__declspec(dllimport) BOOL ConvertSidToStringSidW(const void *,WCHAR **);
__declspec(dllimport) DWORD GetWindowsDirectoryW(WCHAR *,DWORD);
__declspec(dllimport) DWORD GetSystemDirectoryW(WCHAR *,DWORD);
#define SEX_HKU ((HANDLE)(SEX_INT64)(LONG)0x80000003UL)
typedef struct {WCHAR *Path,*Sid;HANDLE UserRoot;BOOL CloseRoot;} SEX_PROFILE;
typedef struct {unsigned Id,Base;const WCHAR *Value,*Suffix,*Environment;} SEX_FOLDER;
enum{SEX_FOLDER_USER,SEX_FOLDER_PROGRAMDATA,SEX_FOLDER_PUBLIC,SEX_FOLDER_WINDOWS,SEX_FOLDER_SYSTEM,SEX_FOLDER_PROGRAMFILES};
static const SEX_FOLDER sex_folders[]={
    {0,0,L"Desktop",L"\\Desktop",0},{0x10,0,L"Desktop",L"\\Desktop",0},
    {2,0,L"Programs",L"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs",0},
    {5,0,L"Personal",L"\\Documents",0},{6,0,L"Favorites",L"\\Favorites",0},
    {7,0,L"Startup",L"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup",0},
    {8,0,L"Recent",L"\\AppData\\Roaming\\Microsoft\\Windows\\Recent",0},
    {9,0,L"SendTo",L"\\AppData\\Roaming\\Microsoft\\Windows\\SendTo",0},
    {0x0b,0,L"Start Menu",L"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu",0},
    {0x0d,0,L"My Music",L"\\Music",0},{0x0e,0,L"My Video",L"\\Videos",0},
    {0x13,0,L"NetHood",L"\\AppData\\Roaming\\Microsoft\\Windows\\Network Shortcuts",0},
    {0x14,SEX_FOLDER_WINDOWS,L"Fonts",L"\\Fonts",0},
    {0x15,0,L"Templates",L"\\AppData\\Roaming\\Microsoft\\Windows\\Templates",0},
    {0x16,SEX_FOLDER_PROGRAMDATA,L"Common Start Menu",L"\\Microsoft\\Windows\\Start Menu",0},
    {0x17,SEX_FOLDER_PROGRAMDATA,L"Common Programs",L"\\Microsoft\\Windows\\Start Menu\\Programs",0},
    {0x18,SEX_FOLDER_PROGRAMDATA,L"Common Startup",L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup",0},
    {0x19,SEX_FOLDER_PUBLIC,L"Common Desktop",L"\\Desktop",0},
    {0x1a,0,L"AppData",L"\\AppData\\Roaming",L"APPDATA"},
    {0x1b,0,L"PrintHood",L"\\AppData\\Roaming\\Microsoft\\Windows\\Printer Shortcuts",0},
    {0x1c,0,L"Local AppData",L"\\AppData\\Local",L"LOCALAPPDATA"},
    {0x20,0,L"Cache",L"\\AppData\\Local\\Microsoft\\Windows\\Temporary Internet Files",0},
    {0x21,0,L"Cookies",L"\\AppData\\Roaming\\Microsoft\\Windows\\Cookies",0},
    {0x22,0,L"History",L"\\AppData\\Local\\Microsoft\\Windows\\History",0},
    {0x23,SEX_FOLDER_PROGRAMDATA,L"Common AppData",L"",L"PROGRAMDATA"},
    {0x24,SEX_FOLDER_WINDOWS,0,L"",0},{0x25,SEX_FOLDER_SYSTEM,0,L"",0},
    {0x26,SEX_FOLDER_PROGRAMFILES,0,L"",L"ProgramFiles"},
    {0x27,0,L"My Pictures",L"\\Pictures",0},{0x28,0,0,L"",L"USERPROFILE"},
    {0x2e,SEX_FOLDER_PUBLIC,L"Common Documents",L"\\Documents",0},
    {0x2f,SEX_FOLDER_PROGRAMDATA,L"Common Administrative Tools",L"\\Microsoft\\Windows\\Start Menu\\Programs\\Administrative Tools",0},
    {0x30,0,L"Administrative Tools",L"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Administrative Tools",0},
    {0x35,SEX_FOLDER_PUBLIC,L"CommonMusic",L"\\Music",0},
    {0x36,SEX_FOLDER_PUBLIC,L"CommonPictures",L"\\Pictures",0},
    {0x37,SEX_FOLDER_PUBLIC,L"CommonVideo",L"\\Videos",0}
};
static void sex_profile_release(SEX_PROFILE *profile)
{
    if(profile->CloseRoot)RegCloseKey(profile->UserRoot);
    sex_free(profile->Path);if(profile->Sid)LocalFree(profile->Sid);
}
static DWORD sex_profile_resolve(HANDLE token,SEX_PROFILE *profile)
{
    *profile=(SEX_PROFILE){0};
    if(!token) {
        profile->UserRoot=SEX_HKCU;profile->Path=sex_environment(L"USERPROFILE");
        if(profile->Path && *profile->Path)return 0;
        sex_free(profile->Path);profile->Path=0;
    }
    if(token==(HANDLE)(SEX_INT64)-1) {
        WCHAR *registered=0;DWORD type=0;
        DWORD status=sex_registry_string(SEX_HKLM,L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",L"Default",&registered,&type);
        if(status)return status;
        profile->Path=type==2 ? sex_expand_environment(registered) : sex_duplicate(registered);
        sex_free(registered);return profile->Path ? 0 : 8;
    }
    HANDLE opened=0;
    if(!token){if(!OpenProcessToken(GetCurrentProcess(),8,&opened))return GetLastError();token=opened;}
    DWORD bytes=0;
    GetTokenInformation(token,1,0,0,&bytes);
    if(!bytes){DWORD error=GetLastError();if(opened)CloseHandle(opened);return error?error:13;}
    void *information=HeapAlloc(GetProcessHeap(),0,bytes);if(!information){if(opened)CloseHandle(opened);return 8;}
    BOOL got=GetTokenInformation(token,1,information,bytes,&bytes) && ConvertSidToStringSidW(*(void **)information,&profile->Sid);
    DWORD error=GetLastError();sex_free(information);if(opened)CloseHandle(opened);
    if(!got)return error?error:13;
    SEX_STRING key={0};WCHAR *registered=0;DWORD type=0,status=8;
    if(!sex_append(&key,L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\") || !sex_append(&key,profile->Sid))goto done;
    status=sex_registry_string(SEX_HKLM,key.Text,L"ProfileImagePath",&registered,&type);if(status)goto done;
    profile->Path=type==2 ? sex_expand_environment(registered) : sex_duplicate(registered);
    if(!profile->Path){status=8;goto done;}
    if(!opened && token) {
        HANDLE hive=0;LONG opened_hive=RegOpenKeyExW(SEX_HKU,profile->Sid,0,SEX_KEY_READ,&hive);
        if(!opened_hive){profile->UserRoot=hive;profile->CloseRoot=1;}
        else if(opened_hive!=2 && opened_hive!=3){status=(DWORD)opened_hive;goto done;}
    }
done:sex_free(key.Text);sex_free(registered);return status;
}
static WCHAR *sex_profile_expand(const WCHAR *value,const SEX_PROFILE *profile)
{
    SEX_STRING text={0};const WCHAR *token=L"%USERPROFILE%";
    for(SEX_SIZE i=0;value[i];) {
        BOOL match=value[i]=='%';
        for(unsigned j=0;match && token[j];++j)if(sex_lower(value[i+j])!=sex_lower(token[j]))match=0;
        if(match){if(!sex_append(&text,profile->Path))goto failed;i+=13;}
        else if(!sex_character(&text,value[i++]))goto failed;
    }
    WCHAR *expanded=sex_expand_environment(text.Text ? text.Text:L"");sex_free(text.Text);return expanded;
failed:sex_free(text.Text);return 0;
}
static DWORD sex_folder_registry(const SEX_FOLDER *folder,const SEX_PROFILE *profile,WCHAR **out)
{
    if(!folder->Value)return 2;
    HANDLE root=folder->Base==SEX_FOLDER_USER ? profile->UserRoot : SEX_HKLM;
    if(!root)return 2;
    DWORD type=0;
    DWORD status=sex_registry_string(root,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",folder->Value,out,&type);
    if(status==2 || status==3)status=sex_registry_string(root,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",folder->Value,out,&type);
    if(!status && type==2) {
        WCHAR *expanded=profile->Path ? sex_profile_expand(*out,profile) : sex_expand_environment(*out);
        sex_free(*out);*out=expanded;if(!expanded)return 8;
    }
    return status;
}
static WCHAR *sex_system_directory(BOOL system)
{
    DWORD count=MAX_PATH;
    for(;;) {
        WCHAR *path=HeapAlloc(GetProcessHeap(),0,(SEX_SIZE)count*sizeof(WCHAR));if(!path){SetLastError(8);return 0;}
        DWORD needed=system ? GetSystemDirectoryW(path,count) : GetWindowsDirectoryW(path,count);
        if(!needed){sex_free(path);return 0;}
        if(needed>=count){sex_free(path);count=needed+1;continue;}return path;
    }
}
static DWORD sex_create_directory_tree(const WCHAR *path,void *security)
{
    DWORD attributes=GetFileAttributesW(path);
    if(attributes!=0xffffffffUL)return attributes&0x10 ? 183 : 267;
    WCHAR *working=sex_duplicate(path);if(!working)return 8;
    SEX_SIZE first=3;
    if(working[0]=='\\' && working[1]=='\\') {
        first=2;while(working[first] && working[first]!='\\')first++;
        if(working[first])first++;while(working[first] && working[first]!='\\')first++;
        if(working[first])first++;
    }
    DWORD status=0;
    for(SEX_SIZE i=first;;++i) {
        WCHAR ch=working[i];if(ch && ch!='\\' && ch!='/')continue;
        working[i]=0;attributes=GetFileAttributesW(working);
        if(attributes==0xffffffffUL) {
            if(!CreateDirectoryW(working,ch?0:security)) {
                status=GetLastError();DWORD existing=GetFileAttributesW(working);
                if(status==183 && existing!=0xffffffffUL && (existing&0x10))status=0;
                if(status)break;
            }
        } else if(!(attributes&0x10)){status=267;break;}
        working[i]=ch;if(!ch)break;
    }
    sex_free(working);return status;
}
__declspec(dllexport) int SHCreateDirectoryExW(HWND owner,const WCHAR *path,void *security)
{
    (void)owner;if(!path || !*path)return 87;
    WCHAR *absolute=sex_full_path(path,0);if(!absolute)return (int)GetLastError();
    DWORD status=sex_create_directory_tree(absolute,security);sex_free(absolute);return (int)status;
}
__declspec(dllexport) HRESULT SHGetFolderPathW(HWND owner,int id,HANDLE token,DWORD flags,WCHAR *out)
{
    (void)owner;if(!out || flags>1)return (HRESULT)0x80070057UL;out[0]=0;
    const SEX_FOLDER *folder=0;unsigned value=(unsigned)id&0xff;
    for(unsigned i=0;i<sizeof(sex_folders)/sizeof(*sex_folders);++i)if(sex_folders[i].Id==value){folder=sex_folders+i;break;}
    if(!folder || ((unsigned)id&~0xffffu))return (HRESULT)0x80070057UL;
    SEX_PROFILE profile={0};WCHAR *path=0,*base=0;DWORD status=0;
    if(folder->Base==SEX_FOLDER_USER) {status=sex_profile_resolve(token,&profile);if(status)goto done;}
    if(!flags){status=sex_folder_registry(folder,&profile,&path);if(status && status!=2 && status!=3)goto done;}
    if(!path) {
        if(folder->Base==SEX_FOLDER_USER) {
            if(!token && !flags && folder->Environment)base=sex_environment(folder->Environment);
            if(base && *base){path=base;base=0;}
            else {sex_free(base);base=sex_duplicate(profile.Path);}
        } else if(folder->Base==SEX_FOLDER_WINDOWS || folder->Base==SEX_FOLDER_SYSTEM)base=sex_system_directory(folder->Base==SEX_FOLDER_SYSTEM);
        else base=sex_environment(folder->Base==SEX_FOLDER_PUBLIC ? L"PUBLIC":folder->Base==SEX_FOLDER_PROGRAMFILES ? L"ProgramFiles":L"PROGRAMDATA");
        if(!path) {
            if(!base || !*base){status=GetLastError();if(!status)status=3;goto done;}
            SEX_STRING joined={0};if(!sex_append(&joined,base) || !sex_append(&joined,folder->Suffix)){sex_free(joined.Text);status=8;goto done;}path=joined.Text;
        }
    }
    if(!path || !*path){status=13;goto done;}
    WCHAR *normalized=sex_full_path(path,0);
    if(!normalized){status=GetLastError();if(!status)status=3;goto done;}
    sex_free(path);path=normalized;
    if((unsigned)id&0x8000u) {status=sex_create_directory_tree(path,0);if(status==183)status=0;if(status)goto done;}
    else if(!((unsigned)id&0x4000u)) {
        DWORD attributes=GetFileAttributesW(path);if(attributes==0xffffffffUL || !(attributes&0x10)){status=3;goto done;}
    }
    if(sex_length(path)>=MAX_PATH){status=206;goto done;}
    copy_path(out,path);status=0;
done:sex_free(path);sex_free(base);sex_profile_release(&profile);
    if(status){SetLastError(status);return (HRESULT)(0x80070000UL|(status&0xffff));}return S_OK;
}
__declspec(dllexport) BOOL SHGetSpecialFolderPathW(HWND owner,WCHAR *path,int folder,BOOL create)
{return SHGetFolderPathW(owner,folder|(create?0x8000:0),0,0,path)>=0;}
