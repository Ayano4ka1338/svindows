__declspec(dllimport) UINT64 NtUserClassLong(HWND hwnd, int index, LONG_PTR value, UINT write);
/* Compatibility APIs used by the Windows 7 calculator. The kernel owns
 * actual windows; this client-side table keeps the small pieces of USER state
 * the current native window ABI does not expose yet. */
__declspec(dllimport) UINT64 NtUserSetWindowLongPtr(HWND hwnd, int index, LONG_PTR value);
#define SV_USER_WINDOWS 128
#define SV_USER_PROPS 8
#define GWL_STYLE       (-16)
#define GWL_EXSTYLE     (-20)
#define GWL_ID          (-12)
#define GWLP_HINSTANCE  (-6)
#define GWLP_HWNDPARENT  (-8)
#define GWLP_USERDATA   (-21)
#define GWLP_WNDPROC     (-4)
#define SPI_GETWORKAREA  0x0030
#define CF_TEXT          1
#define CF_UNICODETEXT   13
__declspec(dllimport) int SvGdiDrawTextW(HDC dc, const WCHAR *text,
                                          int count, LPRECT rect, UINT format);

typedef struct { int Min, Max, Position; UINT Page; } SV_SCROLL_STATE;

typedef struct {
    HWND Window, Parent;
    WCHAR ClassName[96], Title[160];
    DWORD Style, ExStyle, Id;
    HINSTANCE Instance;
    HMENU Menu, SystemMenu;
    LONG_PTR Longs[8], ExtraLongs[8];
    WORD ClassWords[8];
    BOOL Enabled, Used;
    RECT NormalRect;
    int ShowCmd;
    SV_SCROLL_STATE Scroll[2];
    struct { const WCHAR *Name; HANDLE Value; } Props[SV_USER_PROPS];
} SV_USER_WINDOW_STATE;
static SV_USER_WINDOW_STATE g_sv_windows[SV_USER_WINDOWS];

static HANDLE g_sv_clipboard[32];
static BOOL g_sv_clipboard_open;

static SV_USER_WINDOW_STATE *sv_user_state(HWND hwnd)
{
    if (!hwnd) return 0;
    SV_USER_WINDOW_STATE *free_slot = 0;
    for (UINT i = 0; i < SV_USER_WINDOWS; i++) {
        if (g_sv_windows[i].Used && g_sv_windows[i].Window == hwnd)
            return &g_sv_windows[i];
        /* A reused native slot has a new handle generation. Descendants can
         * be destroyed by the kernel without a client DestroyWindow call. */
        if(g_sv_windows[i].Used&&(((UINT_PTR)g_sv_windows[i].Window^(UINT_PTR)hwnd)&0xffff)==0)
            sv_user_forget_window(g_sv_windows[i].Window);
        if (!g_sv_windows[i].Used && !free_slot)
            free_slot = &g_sv_windows[i];
    }
    /* WM_NCCREATE/WM_CREATE arrive before CreateWindowExW returns. Create the
     * per-HWND record on first API access so GWLP_USERDATA and properties set
     * from those callbacks survive the later creation bookkeeping. */
    if (free_slot) {
        *free_slot = (SV_USER_WINDOW_STATE){0};
        free_slot->Window = hwnd;
        free_slot->Used = TRUE;
    }
    return free_slot;
}

static void sv_user_record_window(HWND hwnd, const WCHAR *class_name,
                                  const WCHAR *title, DWORD style,
                                  DWORD ex_style, HWND parent, HMENU menu,
                                  HINSTANCE instance)
{
    SV_USER_WINDOW_STATE *state = sv_user_state(hwnd);
    if (!state) {
        for (UINT i = 0; i < SV_USER_WINDOWS; i++)
            if (!g_sv_windows[i].Used) { state = &g_sv_windows[i]; break; }
    }
    if (!state) return;
    /* Preserve fields (notably user data) written during synchronous
     * WM_NCCREATE/WM_CREATE callbacks before this function is reached. */
    state->Window = hwnd; state->Parent = parent;
    state->Style = style; state->ExStyle = ex_style;
    state->Id = (DWORD)(UINT_PTR)menu; state->Menu = menu;
    state->Instance = instance; state->Enabled = (style & WS_DISABLED) == 0;
    state->Scroll[0].Max = state->Scroll[1].Max = 100;
    state->Used = TRUE; state->ShowCmd = (style & WS_VISIBLE) ? SW_SHOW : SW_HIDE;
    for (UINT i = 0; class_name && class_name[i] && i + 1 < 96; i++)
        state->ClassName[i] = class_name[i];
    for (UINT i = 0; title && title[i] && i + 1 < 160; i++)
        state->Title[i] = title[i];
    (void)NtUserGetWindowRect(hwnd, &state->NormalRect);
}

static BOOL sv_user_wide_equal(const WCHAR *a, const WCHAR *b)
{
    if (!a || !b) return a == b;
    for (;;) {
        WCHAR x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return FALSE;
        if (!x) return TRUE;
    }
}

static int sv_user_long_slot(int index)
{
    switch (index) {
    case GWL_STYLE: return 0;
    case GWL_EXSTYLE: return 1;
    case GWL_ID: return 2;
    case GWLP_HINSTANCE: return 3;
    case GWLP_HWNDPARENT: return 4;
    case GWLP_USERDATA: return 5;
    case GWLP_WNDPROC: return 6;
    default: return -1;
    }
}

USER32_API LONG_PTR GetWindowLongPtrW(HWND hwnd, int index)
{
    SV_USER_WINDOW_STATE *s = sv_user_state(hwnd);
    if(s && index>=0 && index<64 && !(index&7)) return s->ExtraLongs[index/8];
    int slot = sv_user_long_slot(index);
    if (!s || slot < 0) return 0;
    LONG_PTR result;
    switch (index) {
    case GWL_STYLE: result = s->Style; break;
    case GWL_EXSTYLE: result = s->ExStyle; break;
    case GWL_ID: result = s->Id; break;
    case GWLP_HINSTANCE: result = (LONG_PTR)s->Instance; break;
    case GWLP_HWNDPARENT: result = (LONG_PTR)s->Parent; break;
    default: result = s->Longs[slot]; break;
    }
    return result;
}

USER32_API LONG_PTR SetWindowLongPtrW(HWND hwnd, int index, LONG_PTR value)
{
    SV_USER_WINDOW_STATE *s = sv_user_state(hwnd);
    if(s && index>=0 && index<64 && !(index&7)) {LONG_PTR old=s->ExtraLongs[index/8];s->ExtraLongs[index/8]=value;return old;}
    int slot = sv_user_long_slot(index);
    if (!s || slot < 0) return 0;
    LONG_PTR old = (index == GWLP_WNDPROC || index == GWL_STYLE) ?
        (LONG_PTR)NtUserSetWindowLongPtr(hwnd, index, value) :
        GetWindowLongPtrW(hwnd, index);
    switch (index) {
    case GWL_STYLE: s->Style = (DWORD)value; break;
    case GWL_EXSTYLE: s->ExStyle = (DWORD)value; break;
    case GWL_ID: s->Id = (DWORD)value; break;
    case GWLP_HINSTANCE: s->Instance = (HINSTANCE)value; break;
    case GWLP_HWNDPARENT: s->Parent = (HWND)value; break;
    default: s->Longs[slot] = value; break;
    }
    return old;
}

USER32_API LONG GetWindowLongW(HWND hwnd, int index)
{ return (LONG)GetWindowLongPtrW(hwnd, index); }
USER32_API LONG SetWindowLongW(HWND hwnd, int index, LONG value)
{ return (LONG)SetWindowLongPtrW(hwnd, index, value); }

USER32_API BOOL EnableWindow(HWND hwnd, BOOL enable)
{
    SV_USER_WINDOW_STATE *s = sv_user_state(hwnd);
    if (!s) return FALSE;
    BOOL previous = s->Enabled; s->Enabled = enable != 0;
    return previous;
}
USER32_API BOOL IsWindowEnabled(HWND hwnd)
{ SV_USER_WINDOW_STATE *s = sv_user_state(hwnd); return s ? s->Enabled : FALSE; }

#include "User32Characters.inc"

USER32_API const char *CharNextA(const char *text)
{
    if (!text || !*text) return (char *)text;
    unsigned char c = (unsigned char)*text;
    if (((c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc)) && text[1])
        return text + 2;
    return text + 1;
}

USER32_API BOOL OpenClipboard(HWND owner)
{ (void)owner; if (g_sv_clipboard_open) return FALSE; g_sv_clipboard_open = TRUE; return TRUE; }
USER32_API BOOL CloseClipboard(void)
{ if (!g_sv_clipboard_open) return FALSE; g_sv_clipboard_open = FALSE; return TRUE; }
USER32_API BOOL EmptyClipboard(void)
{ if (!g_sv_clipboard_open) return FALSE; for (UINT i=0;i<32;i++) g_sv_clipboard[i]=0; return TRUE; }
USER32_API BOOL IsClipboardFormatAvailable(UINT format)
{ return format < 32 && g_sv_clipboard[format] != 0; }
USER32_API HANDLE GetClipboardData(UINT format)
{ return g_sv_clipboard_open && format < 32 ? g_sv_clipboard[format] : 0; }
USER32_API HANDLE SetClipboardData(UINT format, HANDLE data)
{ if (!g_sv_clipboard_open || format >= 32 || !data) return 0; g_sv_clipboard[format]=data; return data; }

#include "User32Menus.inc"
#include "User32Scroll.inc"

USER32_API BOOL BringWindowToTop(HWND hwnd)
{
    if (!SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE))
        return FALSE;
    SetActiveWindow(hwnd);
    return TRUE;
}

USER32_API BOOL SetForegroundWindow(HWND hwnd)
{ return SetActiveWindow(hwnd) != 0 || sv_user_state(hwnd) != 0; }
USER32_API HWND GetAncestor(HWND hwnd, UINT flags)
{ return (HWND)(UINT_PTR)NtUserGetAncestor(hwnd, flags); }
#include "User32WindowBatch.inc"

USER32_API DWORD GetMessageExtraInfo(void) { return 0; }
#include "User32Accelerators.inc"
USER32_API BOOL IsDialogMessageW(HWND dialog, MSG *message)
{ (void)dialog;(void)message; return FALSE; }

USER32_API int GetClassNameW(HWND hwnd, WCHAR *buffer, int capacity)
{
    SV_USER_WINDOW_STATE *s=sv_user_state(hwnd);
    if (!s || !buffer || capacity <= 0) return 0;
    int n=0; while(s->ClassName[n] && n+1<capacity){buffer[n]=s->ClassName[n];n++;}
    buffer[n]=0; return n;
}
USER32_API HWND FindWindowW(const WCHAR *class_name, const WCHAR *title)
{
    for(UINT i=0;i<SV_USER_WINDOWS;i++) { SV_USER_WINDOW_STATE *s=&g_sv_windows[i];
        if(s->Used && (!class_name || sv_user_wide_equal(class_name,s->ClassName)) &&
           (!title || sv_user_wide_equal(title,s->Title))) return s->Window; }
    return 0;
}
USER32_API BOOL SetPropW(HWND hwnd, const WCHAR *name, HANDLE value)
{
    SV_USER_WINDOW_STATE *s=sv_user_state(hwnd); if(!s||!name)return FALSE;
    for(UINT i=0;i<SV_USER_PROPS;i++) if(!s->Props[i].Name || sv_user_wide_equal(s->Props[i].Name,name)) {
        s->Props[i].Name=name; s->Props[i].Value=value; return TRUE; }
    return FALSE;
}
USER32_API WORD GetClassWord(HWND hwnd, int index)
{ SV_USER_WINDOW_STATE *s=sv_user_state(hwnd); return s&&index>=0&&index<8?s->ClassWords[index]:0; }
USER32_API WORD SetClassWord(HWND hwnd, int index, WORD value)
{ SV_USER_WINDOW_STATE *s=sv_user_state(hwnd); if(!s||index<0||index>=8)return 0; WORD old=s->ClassWords[index];s->ClassWords[index]=value;return old; }
USER32_API LONG_PTR GetClassLongPtrW(HWND hwnd, int index)
{ return (LONG_PTR)NtUserClassLong(hwnd,index,0,0); }
USER32_API LONG_PTR SetClassLongPtrW(HWND hwnd, int index, LONG_PTR value)
{ return (LONG_PTR)NtUserClassLong(hwnd,index,value,1); }
USER32_API LONG GetClassLongW(HWND hwnd, int index)
{ return (LONG)GetClassLongPtrW(hwnd,index); }
USER32_API LONG SetClassLongW(HWND hwnd, int index, LONG value)
{ return (LONG)SetClassLongPtrW(hwnd,index,value); }

typedef struct { UINT Length, Flags, ShowCmd; POINT MinPosition, MaxPosition; RECT NormalPosition; } SV_WINDOWPLACEMENT;
USER32_API BOOL GetWindowPlacement(HWND hwnd, SV_WINDOWPLACEMENT *placement)
{
    SV_USER_WINDOW_STATE *s=sv_user_state(hwnd); if(!s||!placement||placement->Length<sizeof(*placement))return FALSE;
    placement->ShowCmd=s->ShowCmd; placement->NormalPosition=s->NormalRect; return TRUE;
}
USER32_API BOOL SetWindowPlacement(HWND hwnd, const SV_WINDOWPLACEMENT *placement)
{
    SV_USER_WINDOW_STATE *s=sv_user_state(hwnd); if(!s||!placement||placement->Length<sizeof(*placement))return FALSE;
    s->NormalRect=placement->NormalPosition;s->ShowCmd=(int)placement->ShowCmd;
    return SetWindowPos(hwnd,0,s->NormalRect.left,s->NormalRect.top,
       s->NormalRect.right-s->NormalRect.left,s->NormalRect.bottom-s->NormalRect.top,0);
}
USER32_API BOOL GetProcessDefaultLayout(DWORD *layout)
{ if(!layout)return FALSE;*layout=0;return TRUE; }

USER32_API BOOL SetRect(LPRECT rect, int left, int top, int right, int bottom)
{
    if (!rect) return FALSE;
    *rect = (RECT){left, top, right, bottom};
    return TRUE;
}

USER32_API BOOL OffsetRect(LPRECT rect, int dx, int dy)
{ if(!rect)return FALSE;rect->left+=dx;rect->right+=dx;rect->top+=dy;rect->bottom+=dy;return TRUE; }
USER32_API BOOL CopyRect(LPRECT dst, const RECT *src)
{ if(!dst||!src)return FALSE;*dst=*src;return TRUE; }
USER32_API BOOL EqualRect(const RECT *a, const RECT *b)
{ return a&&b&&a->left==b->left&&a->top==b->top&&a->right==b->right&&a->bottom==b->bottom; }
USER32_API BOOL IntersectRect(LPRECT dst, const RECT *a, const RECT *b)
{
    if(!dst||!a||!b)return FALSE;dst->left=a->left>b->left?a->left:b->left;
    dst->top=a->top>b->top?a->top:b->top;dst->right=a->right<b->right?a->right:b->right;
    dst->bottom=a->bottom<b->bottom?a->bottom:b->bottom;
    if(dst->right<=dst->left||dst->bottom<=dst->top){*dst=(RECT){0};return FALSE;}return TRUE;
}
__declspec(dllimport) UINT64 NtUserMapWindowPoints(HWND,HWND,POINT*,UINT);
USER32_API int MapWindowPoints(HWND from, HWND to, POINT *points, UINT count)
{ return (int)NtUserMapWindowPoints(from,to,points,count); }

__declspec(dllimport) UINT64 SvNtUserGetCursorPos(POINT *point);
__declspec(dllimport) UINT64 SvNtUserChildWindowFromPoint(HWND parent, LONG x, LONG y);

USER32_API BOOL GetCursorPos(POINT *point)
{
    return SvNtUserGetCursorPos(point) != 0;
}

USER32_API BOOL ScreenToClient(HWND hwnd, POINT *point)
{
    RECT client;
    if (!point || !GetClientRect(hwnd, &client)) return FALSE;
    MapWindowPoints(0, hwnd, point, 1);
    return TRUE;
}

USER32_API BOOL ClientToScreen(HWND hwnd, POINT *point)
{
    RECT client;
    if (!point || !GetClientRect(hwnd, &client)) return FALSE;
    MapWindowPoints(hwnd, 0, point, 1);
    return TRUE;
}

USER32_API HWND ChildWindowFromPoint(HWND parent, POINT point)
{
    return (HWND)(UINT_PTR)SvNtUserChildWindowFromPoint(parent, point.x, point.y);
}

typedef struct { DWORD Size; RECT Monitor, Work; DWORD Flags; } SV_MONITORINFO;
USER32_API HANDLE MonitorFromWindow(HWND hwnd, DWORD flags)
{ (void)flags;return sv_user_state(hwnd)?(HANDLE)1:0; }
USER32_API HANDLE MonitorFromRect(const RECT *rect, DWORD flags)
{ (void)rect;(void)flags;return (HANDLE)1; }
USER32_API BOOL GetMonitorInfoW(HANDLE monitor, SV_MONITORINFO *info)
{
    if(!monitor||!info||info->Size<sizeof(*info))return FALSE;
    info->Monitor=(RECT){0,0,1200,800};info->Work=info->Monitor;info->Flags=1;return TRUE;
}
USER32_API BOOL SystemParametersInfoW(UINT action, UINT parameter,
                                      void *data, UINT update)
{
    (void)parameter;(void)update;
    if(action==SPI_GETWORKAREA&&data){*(RECT*)data=(RECT){0,0,1200,800};return TRUE;}
    return FALSE;
}
USER32_API BOOL EnumDisplayMonitors(HDC dc, const RECT *clip,
        BOOL (*callback)(HANDLE,HDC,RECT*,LONG_PTR), LONG_PTR data)
{
    RECT monitor={0,0,1200,800};if(clip&&!IntersectRect(&monitor,&monitor,clip))return TRUE;
    return !callback||callback((HANDLE)1,dc,&monitor,data);
}
USER32_API BOOL EnumDesktopWindows(HANDLE desktop,
        BOOL (*callback)(HWND,LONG_PTR), LONG_PTR data)
{
    (void)desktop;for(UINT i=0;i<SV_USER_WINDOWS;i++)if(g_sv_windows[i].Used&&!g_sv_windows[i].Parent)
        if(callback&&!callback(g_sv_windows[i].Window,data))return FALSE;return TRUE;
}
USER32_API BOOL EnumChildWindows(HWND parent,
        BOOL (*callback)(HWND,LONG_PTR), LONG_PTR data)
{
    for(UINT i=0;i<SV_USER_WINDOWS;i++)if(g_sv_windows[i].Used&&g_sv_windows[i].Parent==parent)
        if(callback&&!callback(g_sv_windows[i].Window,data))return FALSE;return TRUE;
}
USER32_API HWND GetDlgItem(HWND dialog, int id)
{
    for(UINT i=0;i<SV_USER_WINDOWS;i++)if(g_sv_windows[i].Used&&g_sv_windows[i].Parent==dialog&&g_sv_windows[i].Id==(DWORD)id)return g_sv_windows[i].Window;return 0;
}
USER32_API LRESULT SendDlgItemMessageW(HWND dialog, int id, UINT message,
                                       WPARAM wparam, LPARAM lparam)
{ HWND child=GetDlgItem(dialog,id);return child?SendMessageW(child,message,wparam,lparam):0; }
USER32_API UINT GetDlgItemTextW(HWND dialog, int id, WCHAR *text, int capacity)
{
    if (!text || capacity <= 0) return 0;
    text[0] = 0;
    return (UINT)GetWindowTextW(GetDlgItem(dialog, id), text, capacity);
}

USER32_API BOOL SetDlgItemTextW(HWND dialog, int id, const WCHAR *text)
{
    return SetWindowTextW(GetDlgItem(dialog, id), text);
}

USER32_API int GetDlgCtrlID(HWND control)
{
    return (int)GetWindowLongPtrW(control, GWL_ID);
}

USER32_API BOOL IsDlgButtonChecked(HWND dialog, int id)
{ return (BOOL)SendDlgItemMessageW(dialog,id,0x00f0,0,0); }
USER32_API BOOL CheckDlgButton(HWND dialog, int id, UINT check)
{
    HWND button = GetDlgItem(dialog, id);
    if (!button || check > 2) return FALSE;
    SendMessageW(button, BM_SETCHECK, check, 0);
    return TRUE;
}

USER32_API BOOL SetDlgItemInt(HWND dialog, int id, UINT value, BOOL signed_value)
{
    WCHAR text[16];UINT n=value,i=0;if(signed_value&&(int)value<0)n=(UINT)-(int)value;
    do{text[i++]=(WCHAR)('0'+n%10);n/=10;}while(n&&i<14);
    if(signed_value&&(int)value<0)text[i++]='-';text[i]=0;
    for(UINT a=0;a<i/2;a++){WCHAR t=text[a];text[a]=text[i-1-a];text[i-1-a]=t;}
    return SetDlgItemTextW(dialog, id, text);
}
USER32_API UINT GetDlgItemInt(HWND dialog, int id, BOOL *translated,
                              BOOL signed_value)
{
    WCHAR text[32];int n=(int)GetDlgItemTextW(dialog,id,text,32);
    UINT value=0;int i=0,neg=0;if(n&&text[0]=='-'){neg=1;i++;}
    for(;i<n&&text[i]>='0'&&text[i]<='9';i++)value=value*10+(text[i]-'0');
    if(translated)*translated=n>0&&i==n;if(neg&&signed_value)value=(UINT)-(int)value;return value;
}
USER32_API BOOL CheckRadioButton(HWND dialog, int first, int last, int check)
{
    for(int id=first;id<=last;id++)(void)SendDlgItemMessageW(dialog,id,0x00f1,id==check,0);return TRUE;
}
USER32_API HWND GetNextDlgTabItem(HWND dialog, HWND current, BOOL previous)
{
    SV_USER_WINDOW_STATE *cur=sv_user_state(current);DWORD id=cur?cur->Id:0;
    HWND best=0;DWORD best_id=0xffffffffu;
    for(UINT i=0;i<SV_USER_WINDOWS;i++){SV_USER_WINDOW_STATE*s=&g_sv_windows[i];
        if(!s->Used||s->Parent!=dialog||!(s->Style&WS_TABSTOP))continue;
        if((!previous&&s->Id>id&&s->Id<best_id)||(previous&&s->Id<id&&s->Id>best_id)){best=s->Window;best_id=s->Id;}}
    return best;
}
#include "User32Dialogs.inc"
USER32_API int DialogBoxParamW(HINSTANCE instance,const WCHAR *resource,
        HWND parent,WNDPROC proc,LPARAM param)
{ (void)instance;(void)resource;(void)parent;(void)proc;(void)param;return -1; }
USER32_API BOOL EndDialog(HWND dialog, INT_PTR result)
{ (void)result;return DestroyWindow(dialog); }

USER32_API BOOL UnregisterClassA(const char *class_name, HINSTANCE instance)
{
    WCHAR wide[128];UINT i=0;if(!class_name)return FALSE;
    while(class_name[i]&&i+1<128){wide[i]=(unsigned char)class_name[i];i++;}wide[i]=0;
    return UnregisterClassW(wide,instance);
}
__declspec(dllimport) UINT64 SvNtUserCreateIconHandle(HINSTANCE instance,
    UINT_PTR resource, BOOL shared);
__declspec(dllimport) UINT64 SvNtUserDestroyIcon(HICON icon);

static HICON load_icon_resource(HINSTANCE instance, const WCHAR *name, BOOL shared)
{
    if (!name) return NULL;
    UINT_PTR key = (UINT_PTR)name;
    if (!instance) {
        if (key < 32512 || key > 32518) return NULL;
    } else {
        HANDLE group = FindResourceW(instance, name, MAKEINTRESOURCEW(14));
        if (!group || SizeofResource(instance, group) < 6) return NULL;
        key = (UINT_PTR)group;
    }
    /* Window classes can reference the resource. The window manager does
     * not yet paint icon pixels in its title bar. */
    return (HICON)(UINT_PTR)SvNtUserCreateIconHandle(instance, key, shared);
}

USER32_API HICON LoadIconW(HINSTANCE instance, const WCHAR *name)
{
    return load_icon_resource(instance, name, TRUE);
}

USER32_API BOOL DestroyIcon(HICON icon)
{
    if (icon && SvNtUserDestroyIcon(icon)) return TRUE;
    SetLastError(1402); /* ERROR_INVALID_CURSOR_HANDLE */
    return FALSE;
}

USER32_API HBITMAP LoadImageW(HINSTANCE instance,const WCHAR *name,UINT type,
                              int width,int height,UINT flags)
{
    if (type == 1) /* IMAGE_ICON */
        return load_icon_resource(instance, name, !instance || (flags & 0x8000) != 0);
    if(type!=0)return 0; /* IMAGE_BITMAP */
    HANDLE resource=FindResourceW(instance,name,(const WCHAR*)(UINT_PTR)2);
    if(!resource)return 0;
    DWORD size=SizeofResource(instance,resource);
    const unsigned char *data=LockResource(LoadResource(instance,resource));
    if(!data||size<40)return 0;
    DWORD header=*(const DWORD*)data;
    int sw=*(const LONG*)(data+4),sh=*(const LONG*)(data+8);
    WORD bpp=*(const WORD*)(data+14);
    DWORD compression=*(const DWORD*)(data+16),colors=*(const DWORD*)(data+32);
    if(header<40||header>size||sw<=0||!sh||sh==(-2147483647-1)||compression||
        (bpp!=1&&bpp!=4&&bpp!=8&&bpp!=24&&bpp!=32))return 0;
    int rows=sh<0?-sh:sh;
    if(sw>4096||rows>4096)return 0;
    if(bpp<=8&&!colors)colors=1u<<bpp;
    if(bpp>8)colors=0;
    if(colors>256||header+colors*4>size)return 0;
    DWORD start=header+colors*4,stride=((sw*bpp+31)/32)*4;
    if((UINT64)stride*rows>size-start)return 0;
    int dw=width>0?width:sw,dh=height>0?height:rows;
    struct { DWORD Size;LONG Width,Height;WORD Planes,Bits;DWORD Compression,Bytes;LONG X,Y;DWORD Used,Important; } info={40,dw,-dh,1,32,0,0,0,0,0,0};
    void *pixels=0;HBITMAP bitmap=CreateDIBSection(0,&info,0,&pixels,0,0);
    if(!bitmap)return 0;
    for(int y=0;y<dh;y++){
        int sy=(int)((UINT64)y*rows/dh);if(sh>0)sy=rows-1-sy;
        const unsigned char *line=data+start+(UINT64)sy*stride;
        for(int x=0;x<dw;x++){
            int sx=(int)((UINT64)x*sw/dw);DWORD pixel;
            if(bpp<=8){UINT index=bpp==8?line[sx]:bpp==4?(line[sx/2]>>(sx%2?0:4))&15:(line[sx/8]>>(7-sx%8))&1;
                pixel=index<colors?*(const DWORD*)(data+header+index*4):0;
            }else {const unsigned char *v=line+sx*(bpp/8);pixel=v[0]|((DWORD)v[1]<<8)|((DWORD)v[2]<<16);}
            ((DWORD*)pixels)[(UINT64)y*dw+x]=pixel|0xff000000u;
        }
    }
    return bitmap;
}
USER32_API BOOL MessageBeep(UINT type) { (void)type; return TRUE; }
USER32_API BOOL FillRect(HDC dc,const RECT *rect,HBRUSH brush)
{
    if(!dc||!rect||!brush||rect->right<rect->left||rect->bottom<rect->top)return FALSE;
    HGDIOBJ old=SelectObject(dc,brush);
    /* FillRect is a brush operation. Rectangle also strokes with the selected
     * pen, which adds an unrelated outline to every filled area. */
    BOOL ok=PatBlt(dc,rect->left,rect->top,rect->right-rect->left,
                   rect->bottom-rect->top,PATCOPY);
    if(old)SelectObject(dc,old);return ok;
}
USER32_API int DrawTextW(HDC dc,const WCHAR *text,int count,LPRECT rect,UINT format)
{ return SvGdiDrawTextW(dc,text,count,rect,format); }


typedef struct {
    UINT Size;
    int TabLength, LeftMargin, RightMargin;
    UINT LengthDrawn;
} SV_DRAWTEXTPARAMS;

__declspec(dllimport) int SvGdiDrawTextExW(HDC dc, const WCHAR *text, int count,
    LPRECT rect, UINT format, UINT *processed);

USER32_API int DrawTextExW(HDC dc, WCHAR *text, int count, LPRECT rect, UINT format,
    SV_DRAWTEXTPARAMS *params)
{
    if (!params) return DrawTextW(dc, text, count, rect, format);
    if (!rect || params->Size != sizeof(*params)) return 0;
    params->LengthDrawn = 0;
    RECT layout = *rect;
    INT64 right_margin = 0;
    if (params->LeftMargin || params->RightMargin) {
        TEXTMETRICW metrics;
        if (!GetTextMetricsW(dc, &metrics)) return 0;
        INT64 left = layout.left + (INT64)params->LeftMargin * metrics.tmAveCharWidth;
        right_margin = (INT64)params->RightMargin * metrics.tmAveCharWidth;
        INT64 right = layout.right - right_margin;
        if (left < -2147483648LL || left > 2147483647LL ||
            right < -2147483648LL || right > 2147483647LL) return 0;
        layout.left = (LONG)left;
        layout.right = (LONG)right;
    }
    int height = SvGdiDrawTextExW(dc, text, count, &layout, format,
                                  &params->LengthDrawn);
    if (height && (format & DT_CALCRECT)) {
        INT64 right = layout.right + right_margin;
        if (right < -2147483648LL || right > 2147483647LL) return 0;
        rect->right = (LONG)right;
        rect->bottom = layout.bottom;
    }
    return height;
}

USER32_API BOOL WinHelpW(HWND owner, const WCHAR *file, UINT command, ULONG_PTR data)
{
    (void)owner;
    (void)file;
    (void)command;
    (void)data;
    /* There is no WinHelp viewer to service the request. */
    extern void SetLastError(DWORD);
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return FALSE;
}

USER32_API BOOL DrawAnimatedRects(HWND window, int animation,
    const RECT *from, const RECT *to)
{
    (void)window;
    (void)animation;
    (void)from;
    (void)to;
    SetLastError(50); /* ERROR_NOT_SUPPORTED: animated window transitions. */
    return FALSE;
}

USER32_API HWND GetLastActivePopup(HWND owner)
{
    RECT rect;
    if (!GetWindowRect(owner, &rect)) return 0;
    HWND active = GetForegroundWindow();
    SV_USER_WINDOW_STATE *state = sv_user_state(active);
    for (UINT depth = 0; state && depth < SV_USER_WINDOWS; depth++) {
        if (state->Window == owner) return active;
        if ((state->Style & WS_CHILD) || !state->Parent) break;
        state = sv_user_state(state->Parent);
    }
    return owner;
}

/* All native display metrics are currently 96 DPI. Awareness is process-local
 * state; no coordinate virtualization is necessary at this display scale. */
static BOOL g_process_dpi_aware;
USER32_API BOOL SetProcessDPIAware(void)
{
    g_process_dpi_aware=TRUE;
    return TRUE;
}
USER32_API BOOL IsProcessDPIAware(void)
{ return g_process_dpi_aware; }

__declspec(dllimport) UINT64 SvNtUserQueryWindowState(HWND window);
USER32_API BOOL IsWindow(HWND window)
{ return (SvNtUserQueryWindowState(window)&1)!=0; }
USER32_API BOOL IsWindowVisible(HWND window)
{ return (SvNtUserQueryWindowState(window)&2)!=0; }
USER32_API BOOL IsZoomed(HWND window)
{ return (SvNtUserQueryWindowState(window)&8)!=0; }
USER32_API DWORD GetWindowThreadProcessId(HWND window, DWORD *process)
{
    DWORD thread=(DWORD)NtUserQueryWindow(window,0);
    if (!thread) { SetLastError(1400); return 0; }
    if (process) *process=(DWORD)NtUserQueryWindow(window,4);
    return thread;
}
USER32_API int InternalGetWindowText(HWND window, WCHAR *text, int capacity)
{
    if (!text || capacity<=0) return 0;
    return (int)NtUserGetWindowText(window,text,(ULONG)capacity);
}

USER32_API DWORD GetDialogBaseUnits(void)
{
    /* The desktop currently uses the fixed 8x16 system raster font.  Windows
     * dialog units are based on the average character width and font height. */
    return (DWORD)8 | ((DWORD)16 << 16);
}

__declspec(dllimport) UINT64 NtUserGetThreadDesktop(DWORD thread_id);
USER32_API void *GetThreadDesktop(DWORD thread_id)
{
    if (!thread_id) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }
    UINT64 desktop = NtUserGetThreadDesktop(thread_id);
    if (!desktop)
        SetLastError(1400); /* ERROR_INVALID_HANDLE */
    return (void *)(UINT_PTR)desktop;
}

__declspec(dllimport) UINT64 NtUserGetClassInfoEx(HINSTANCE, const void *, WNDCLASSEXW *, void *, UINT);
USER32_API BOOL GetClassInfoExW(HINSTANCE instance, const WCHAR *name, WNDCLASSEXW *out)
{
    if (!name || !out || out->cbSize!=sizeof(*out)) { SetLastError(87); return FALSE; }
    struct { USHORT Length,MaximumLength; const WCHAR *Buffer; } string;
    string.Length=string.MaximumLength=0; string.Buffer=name;
    if (!IS_INTRESOURCE(name)) {
        UINT n=0; while (name[n] && n<32767) n++;
        if (n>=32767) { SetLastError(87); return FALSE; }
        string.Length=(USHORT)(n*sizeof(WCHAR));
        string.MaximumLength=string.Length;
        if (!SvRegisterControl(name)) return FALSE;
    }
    if (NtUserGetClassInfoEx(instance,&string,out,0,0)) return TRUE;
    SetLastError(1411); return FALSE;
}
USER32_API BOOL GetClassInfoW(HINSTANCE instance, const WCHAR *name, WNDCLASSW *out)
{
    if (!out) { SetLastError(87); return FALSE; }
    WNDCLASSEXW value; value.cbSize=sizeof(value);
    if (!GetClassInfoExW(instance,name,&value)) return FALSE;
    out->style=value.style; out->lpfnWndProc=value.lpfnWndProc;
    out->cbClsExtra=value.cbClsExtra; out->cbWndExtra=value.cbWndExtra;
    out->hInstance=value.hInstance; out->hIcon=value.hIcon; out->hCursor=value.hCursor;
    out->hbrBackground=value.hbrBackground;
    out->lpszMenuName=value.lpszMenuName; out->lpszClassName=value.lpszClassName;
    return TRUE;
}
