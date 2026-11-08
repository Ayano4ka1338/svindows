/* Modeless dialogs from standard and extended Win32 resource templates. */
__declspec(dllimport) HANDLE FindResourceW(HINSTANCE,const WCHAR*,const WCHAR*);
__declspec(dllimport) HANDLE LoadResource(HINSTANCE,HANDLE);
__declspec(dllimport) void *LockResource(HANDLE);
__declspec(dllimport) DWORD SizeofResource(HINSTANCE,HANDLE);
typedef struct { HWND Window; WNDPROC Proc; } SV_DIALOG;
static SV_DIALOG g_dialogs[32];
static LRESULT sv_dialog_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    SV_DIALOG *d=0;
    if(msg==WM_NCCREATE) {
        const CREATESTRUCTW *cs=(const CREATESTRUCTW*)lp;
        d=(SV_DIALOG*)cs->lpCreateParams;
        if(d)d->Window=hwnd;
        return TRUE;
    }
    for(UINT i=0;i<32;i++)if(g_dialogs[i].Window==hwnd){d=&g_dialogs[i];break;}
    if(msg==WM_CREATE)return 0;
    if(msg==0x4e)SetWindowLongPtrW(hwnd,0,0);
    LRESULT handled=d&&d->Proc?d->Proc(hwnd,msg,wp,lp):0;
    if(msg==0x4e&&handled)return GetWindowLongPtrW(hwnd,0);
    if(msg==0x82&&d)*d=(SV_DIALOG){0};
    return handled?handled:DefWindowProcW(hwnd,msg,wp,lp);
}
static const unsigned char *sv_dialog_string(const unsigned char *p,
        const unsigned char *end,const WCHAR **text)
{
    if(p+2>end)return 0;
    const WCHAR *s=(const WCHAR*)p;
    if(*s==0xffff){if(p+4>end)return 0;if(text)*text=(const WCHAR*)(UINT_PTR)s[1];return p+4;}
    if(text)*text=s;
    do {if(p+2>end)return 0; p+=2;} while(*(const WORD*)(p-2));
    return p;
}
USER32_API HWND CreateDialogParamW(HINSTANCE instance,const WCHAR *resource,
        HWND parent,WNDPROC proc,LPARAM param)
{
    HANDLE r=FindResourceW(instance,resource,(const WCHAR*)(UINT_PTR)5);
    if(!r)return 0;
    const unsigned char *p=LockResource(LoadResource(instance,r));
    DWORD bytes=SizeofResource(instance,r);
    if(!p||bytes<18)return 0;
    const unsigned char *end=p+bytes;
    BOOL ext=*(const WORD*)(p+2)==0xffff;
    DWORD style,ex;WORD count;int x,y,cx,cy;
    if(ext){if(bytes<26)return 0;ex=*(const DWORD*)(p+8);style=*(const DWORD*)(p+12);count=*(const WORD*)(p+16);x=*(const short*)(p+18);y=*(const short*)(p+20);cx=*(const short*)(p+22);cy=*(const short*)(p+24);p+=26;}
    else {style=*(const DWORD*)p;ex=*(const DWORD*)(p+4);count=*(const WORD*)(p+8);x=*(const short*)(p+10);y=*(const short*)(p+12);cx=*(const short*)(p+14);cy=*(const short*)(p+16);p+=18;}
    const WCHAR *title=0;
    p=sv_dialog_string(p,end,0);if(!p)return 0;
    p=sv_dialog_string(p,end,0);if(!p)return 0;
    p=sv_dialog_string(p,end,&title);if(!p)return 0;
    if(style&0x40){if(p+(ext?6:2)>end)return 0;p+=ext?6:2;p=sv_dialog_string(p,end,0);if(!p)return 0;}
    SV_DIALOG *d=0;for(UINT i=0;i<32;i++)if(!g_dialogs[i].Window&&!g_dialogs[i].Proc){d=&g_dialogs[i];break;}
    if(!d)return 0;d->Proc=proc;
    WNDCLASSEXW cls={0};cls.cbSize=sizeof(cls);cls.lpfnWndProc=sv_dialog_proc;cls.hInstance=instance;cls.lpszClassName=L"SvDialogCompat";
    if(!RegisterClassExW(&cls)){d->Proc=0;return 0;}
    HWND hwnd=CreateWindowExW(ex,cls.lpszClassName,title,style,x*2,y*2,cx*2,cy*2,parent,0,instance,d);
    if(!hwnd){*d=(SV_DIALOG){0};return 0;}
    static const WCHAR *classes[]={L"BUTTON",L"EDIT",L"STATIC",L"LISTBOX",L"SCROLLBAR",L"COMBOBOX"};
    for(WORD i=0;i<count;i++){
        p=(const unsigned char*)(((UINT_PTR)p+3)&~(UINT_PTR)3);
        if(p+(ext?24:18)>end)break;
        DWORD cs,ce,id;
        if(ext){ce=*(const DWORD*)(p+4);cs=*(const DWORD*)(p+8);x=*(const short*)(p+12);y=*(const short*)(p+14);cx=*(const short*)(p+16);cy=*(const short*)(p+18);id=*(const DWORD*)(p+20);p+=24;}
        else {cs=*(const DWORD*)p;ce=*(const DWORD*)(p+4);x=*(const short*)(p+8);y=*(const short*)(p+10);cx=*(const short*)(p+12);cy=*(const short*)(p+14);id=*(const WORD*)(p+16);p+=18;}
        const WCHAR *name=0,*text=0;
        p=sv_dialog_string(p,end,&name);if(!p)break;
        p=sv_dialog_string(p,end,&text);if(!p||p+2>end)break;
        WORD extra=*(const WORD*)p;p+=2;if(p+extra>end)break;
        const void *creation=extra?p:0;p+=extra;
        if(IS_INTRESOURCE(name)){UINT_PTR atom=(UINT_PTR)name;if(atom>=0x80&&atom<=0x85)name=classes[atom-0x80];}
        CreateWindowExW(ce,name,text,cs|WS_CHILD,x*2,y*2,cx*2,cy*2,hwnd,(HMENU)(UINT_PTR)id,instance,(void*)creation);
    }
    if(proc)proc(hwnd,0x110,0,param);
    return hwnd;
}
