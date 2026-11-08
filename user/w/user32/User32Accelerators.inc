/* PE RT_ACCELERATOR records are WORD-aligned 8-byte resource entries, not
 * the 6-byte public ACCEL structure. Keep cached resource tables per module. */
__declspec(dllimport) HANDLE FindResourceW(HINSTANCE,const WCHAR*,const WCHAR*);
__declspec(dllimport) HANDLE LoadResource(HINSTANCE,HANDLE);
__declspec(dllimport) void *LockResource(HANDLE);
__declspec(dllimport) DWORD SizeofResource(HINSTANCE,HANDLE);
typedef struct { WORD Flags, Key, Command, Reserved; } SV_RESOURCE_ACCEL;
typedef struct { HINSTANCE Instance; HANDLE Resource; const SV_RESOURCE_ACCEL *Entries; UINT Count; } SV_ACCEL_TABLE;
static SV_ACCEL_TABLE g_sv_accels[32];

USER32_API HACCEL LoadAcceleratorsW(HINSTANCE instance, const WCHAR *name)
{
    HANDLE resource=FindResourceW(instance,name,MAKEINTRESOURCEW(9));
    if(!resource)return 0;
    SV_ACCEL_TABLE *free_slot=0;
    for(UINT i=0;i<32;i++){
        SV_ACCEL_TABLE *table=&g_sv_accels[i];
        if(table->Resource==resource&&table->Instance==instance)return (HACCEL)table;
        if(!table->Resource&&!free_slot)free_slot=table;
    }
    if(!free_slot)return 0;
    DWORD size=SizeofResource(instance,resource);
    const SV_RESOURCE_ACCEL *entries=LockResource(LoadResource(instance,resource));
    if(!entries||size<8||size%8)return 0;
    UINT count=size/8;
    for(UINT i=0;i<count;i++)if(entries[i].Flags&0x80){count=i+1;break;}
    *free_slot=(SV_ACCEL_TABLE){instance,resource,entries,count};
    return (HACCEL)free_slot;
}

USER32_API BOOL TranslateAcceleratorW(HWND hwnd, HACCEL accel, MSG *message)
{
    SV_ACCEL_TABLE *table=0;
    for(UINT i=0;i<32;i++)if(accel==(HACCEL)&g_sv_accels[i]&&g_sv_accels[i].Resource){table=&g_sv_accels[i];break;}
    if(!hwnd||!table||!message)return FALSE;
    BOOL key=message->message==WM_KEYDOWN||message->message==WM_SYSKEYDOWN;
    BOOL character=message->message==WM_CHAR||message->message==0x106;
    if(!key&&!character)return FALSE;
    UINT modifiers=(GetKeyState(VK_SHIFT)<0?4:0)|(GetKeyState(VK_CONTROL)<0?8:0)|(GetKeyState(VK_MENU)<0?16:0);
    for(UINT i=0;i<table->Count;i++){
        const SV_RESOURCE_ACCEL *entry=&table->Entries[i];
        BOOL virt=(entry->Flags&1)!=0;
        if(entry->Key!=message->wParam||(virt?!key:!character))continue;
        /* Character entries already carry translated case/control codes; only
         * virtual-key entries require an exact Shift/Ctrl/Alt combination. */
        if(virt&&(entry->Flags&28)!=modifiers)continue;
        SendMessageW(hwnd,WM_COMMAND,((WPARAM)1<<16)|entry->Command,0);
        return TRUE;
    }
    return FALSE;
}
