/* USER menu objects: resource parsing, recursive lookup and mutable item state. */
__declspec(dllimport) HANDLE FindResourceW(HINSTANCE,const WCHAR*,const WCHAR*);
__declspec(dllimport) HANDLE LoadResource(HINSTANCE,HANDLE);
__declspec(dllimport) void *LockResource(HANDLE);
__declspec(dllimport) DWORD SizeofResource(HINSTANCE,HANDLE);
__declspec(dllimport) UINT64 NtUserMapWindowPoints(HWND,HWND,POINT*,UINT);
__declspec(dllimport) UINT64 NtUserMenuBar(HWND,UINT,HMENU,const WCHAR*,UINT);
#define SV_MENU_MAX 64
#define SV_MENU_ITEMS 64
#define SV_MENU_TEXT 128
#define MF_BY_POS 0x400u
#define MF_POP 0x10u
#define MF_SEPARATOR_ 0x800u
#define MF_DISABLED_ 3u
#define MF_CHECKED_ 8u
typedef struct { UINT Type, State, Id; HMENU Sub; ULONG_PTR Data; HBITMAP Checked,Unchecked,Bitmap; WCHAR Text[SV_MENU_TEXT]; } SV_MENU_ITEM;
typedef struct { BOOL Used; UINT Count; SV_MENU_ITEM Items[SV_MENU_ITEMS]; } SV_USER_MENU;
typedef struct { UINT cbSize,fMask,fType,fState,wID; HMENU hSubMenu; HBITMAP hbmpChecked,hbmpUnchecked; ULONG_PTR dwItemData; WCHAR *dwTypeData; UINT cch; HBITMAP hbmpItem; } SV_MENU_INFO;
_Static_assert(sizeof(SV_MENU_INFO)==80,"MENUITEMINFOW x64");
static SV_USER_MENU g_sv_menus[SV_MENU_MAX];
static SV_USER_MENU *sv_menu(HMENU handle)
{
    for(UINT i=0;i<SV_MENU_MAX;i++)if(handle==(HMENU)&g_sv_menus[i]&&g_sv_menus[i].Used)return &g_sv_menus[i];
    return 0;
}
static void sv_menu_text(WCHAR *out,const WCHAR *in)
{
    UINT n=0;if(in&&!IS_INTRESOURCE(in))while(in[n]&&n+1<SV_MENU_TEXT){out[n]=in[n];n++;}out[n]=0;
}
static SV_MENU_ITEM *sv_menu_find(SV_USER_MENU *m,UINT id,BOOL position,UINT depth,SV_USER_MENU **owner,UINT *index)
{
    if(!m||depth>16)return 0;
    for(UINT i=0;i<m->Count;i++){
        if((position&&i==id)||(!position&&m->Items[i].Id==id)){
            if(owner)*owner=m;if(index)*index=i;return &m->Items[i];
        }
        if(!position&&m->Items[i].Sub){SV_MENU_ITEM *item=sv_menu_find(sv_menu(m->Items[i].Sub),id,FALSE,depth+1,owner,index);if(item)return item;}
    }
    return 0;
}
USER32_API HMENU CreateMenu(void)
{
    for(UINT i=0;i<SV_MENU_MAX;i++)if(!g_sv_menus[i].Used){g_sv_menus[i]=(SV_USER_MENU){0};g_sv_menus[i].Used=TRUE;return (HMENU)&g_sv_menus[i];}
    return 0;
}
USER32_API HMENU CreatePopupMenu(void){return CreateMenu();}
USER32_API BOOL DestroyMenu(HMENU menu)
{
    SV_USER_MENU *m=sv_menu(menu);if(!m)return FALSE;
    m->Used=FALSE;
    for(UINT i=0;i<m->Count;i++)if(m->Items[i].Sub)DestroyMenu(m->Items[i].Sub);
    return TRUE;
}
static SV_MENU_ITEM *sv_menu_insert(SV_USER_MENU *m,UINT index)
{
    if(!m||m->Count==SV_MENU_ITEMS)return 0;
    if(index>m->Count)index=m->Count;
    for(UINT i=m->Count;i>index;i--)m->Items[i]=m->Items[i-1];
    m->Count++;m->Items[index]=(SV_MENU_ITEM){0};return &m->Items[index];
}
static BOOL sv_legacy_menu_item(SV_MENU_ITEM *item, UINT flags, UINT_PTR id,
                                const WCHAR *text)
{
    if ((flags & MF_POP) && !sv_menu((HMENU)id)) return FALSE;
    item->Type = flags & (0x6f64u & ~MF_BY_POS);
    item->State = flags & 0x108b;
    item->Id = (UINT)id;
    if (flags & MF_POP) {
        item->Sub = (HMENU)id;
        item->Id = (UINT)-1;
    }
    if ((flags & MF_SEPARATOR_) || (!(flags & (MF_POP | 4 | 0x100)) &&
        !id && (!text || !text[0])))
        item->Type |= MF_SEPARATOR_;
    else if (flags & 4)
        item->Bitmap = (HBITMAP)text;
    else if (flags & 0x100)
        item->Data = (ULONG_PTR)text;
    else
        sv_menu_text(item->Text, text);
    return TRUE;
}

USER32_API BOOL InsertMenuW(HMENU handle, UINT target, UINT flags, UINT_PTR id,
                            const WCHAR *text)
{
    SV_USER_MENU *menu = sv_menu(handle);
    if (!menu) return FALSE;
    UINT index = menu->Count;
    if (flags & MF_BY_POS) {
        if (target != (UINT)-1) index = target;
    } else if (target != (UINT)-1 &&
               !sv_menu_find(menu, target, FALSE, 0, &menu, &index))
        return FALSE;

    SV_MENU_ITEM replacement = {0};
    if (!sv_legacy_menu_item(&replacement, flags, id, text)) return FALSE;
    SV_MENU_ITEM *item = sv_menu_insert(menu, index);
    if (!item) return FALSE;
    *item = replacement;
    return TRUE;
}

USER32_API BOOL AppendMenuW(HMENU menu, UINT flags, UINT_PTR id, const WCHAR *text)
{
    return InsertMenuW(menu, (UINT)-1, flags | MF_BY_POS, id, text);
}

USER32_API BOOL ModifyMenuW(HMENU menu, UINT target, UINT flags, UINT_PTR id,
                            const WCHAR *text)
{
    SV_MENU_ITEM *item = sv_menu_find(sv_menu(menu), target, (flags & MF_BY_POS) != 0,
                                      0, 0, 0);
    SV_MENU_ITEM replacement = {0};
    if (!item || !sv_legacy_menu_item(&replacement, flags, id, text)) return FALSE;
    HMENU old = item->Sub;
    *item = replacement;
    if (old && old != replacement.Sub) DestroyMenu(old);
    return TRUE;
}

USER32_API HMENU GetSystemMenu(HWND hwnd, BOOL revert)
{
    RECT bounds;
    if (!GetWindowRect(hwnd, &bounds)) return 0;
    SV_USER_WINDOW_STATE *window = sv_user_state(hwnd);
    if (!window || (window->Style & WS_CHILD) || !(window->Style & WS_SYSMENU))
        return 0;

    if (revert) {
        if (window->SystemMenu) DestroyMenu(window->SystemMenu);
        window->SystemMenu = 0;
        return 0;
    }
    if (sv_menu(window->SystemMenu)) return window->SystemMenu;

    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;
    BOOL ok = AppendMenuW(menu, 0, 0xf120, L"&Restore") &&
              AppendMenuW(menu, window->Style & WS_MINIMIZEBOX ? 0 : MF_DISABLED_,
                          0xf020, L"Mi&nimize") &&
              AppendMenuW(menu, window->Style & WS_MAXIMIZEBOX ? 0 : MF_DISABLED_,
                          0xf030, L"Ma&ximize") &&
              AppendMenuW(menu, MF_SEPARATOR_, 0, 0) &&
              AppendMenuW(menu, 0, SC_CLOSE, L"&Close");
    if (!ok) {
        DestroyMenu(menu);
        return 0;
    }
    window->SystemMenu = menu;
    return menu;
}

static BOOL sv_menu_apply(SV_MENU_ITEM *item,const SV_MENU_INFO *info)
{
    if(!item||!info||info->cbSize<sizeof(SV_MENU_INFO))return FALSE;
    UINT mask=info->fMask;
    if((mask&4)&&info->hSubMenu&&!sv_menu(info->hSubMenu))return FALSE;
    if(mask&1)item->State=info->fState;
    if(mask&2)item->Id=info->wID;
    if(mask&4)item->Sub=info->hSubMenu;
    if(mask&8){item->Checked=info->hbmpChecked;item->Unchecked=info->hbmpUnchecked;}
    if(mask&0x20)item->Data=info->dwItemData;
    if(mask&0x100)item->Type=info->fType;
    if(mask&0x40)sv_menu_text(item->Text,info->dwTypeData);
    if(mask&0x10){item->Type=info->fType;if(!(item->Type&(4|0x100|MF_SEPARATOR_)))sv_menu_text(item->Text,info->dwTypeData);}
    if(mask&0x80)item->Bitmap=info->hbmpItem;
    return TRUE;
}
USER32_API BOOL InsertMenuItemW(HMENU menu,UINT id,BOOL position,const void *value)
{
    const SV_MENU_INFO *info=value;if(!info||info->cbSize<sizeof(*info))return FALSE;
    SV_MENU_ITEM pending={0};if(!sv_menu_apply(&pending,info))return FALSE;
    SV_USER_MENU *m=sv_menu(menu);UINT index=id;
    if(!position&&!sv_menu_find(m,id,FALSE,0,&m,&index))index=m?m->Count:0;
    SV_MENU_ITEM *item=sv_menu_insert(m,index);if(!item)return FALSE;*item=pending;return TRUE;
}
USER32_API BOOL SetMenuItemInfoW(HMENU menu,UINT id,BOOL position,const void *info)
{ return sv_menu_apply(sv_menu_find(sv_menu(menu),id,position,0,0,0),info); }
USER32_API BOOL GetMenuItemInfoW(HMENU menu, UINT id, BOOL position, void *value)
{
    SV_MENU_INFO *info = value;
    if (!info || (info->cbSize != sizeof(*info) && info->cbSize != 72)) return FALSE;
    const SV_MENU_ITEM *item = sv_menu_find(sv_menu(menu), id, position, 0, 0, 0);
    if (!item) return FALSE;

    UINT mask = info->fMask;
    if (mask & (0x100 | 0x10)) info->fType = item->Type;
    if (mask & 1) info->fState = item->State;
    if (mask & 2) info->wID = item->Id;
    if (mask & 4) info->hSubMenu = item->Sub;
    if (mask & 8) {
        info->hbmpChecked = item->Checked;
        info->hbmpUnchecked = item->Unchecked;
    }
    if (mask & 0x20) info->dwItemData = item->Data;
    if ((mask & 0x80) && info->cbSize == sizeof(*info)) info->hbmpItem = item->Bitmap;
    if ((mask & 0x10) && (item->Type & 4)) {
        info->dwTypeData = (WCHAR *)item->Bitmap;
        info->cch = 0;
    } else if ((mask & 0x40) || ((mask & 0x10) &&
              !(item->Type & (0x100 | MF_SEPARATOR_)))) {
        UINT length = 0;
        while (item->Text[length]) length++;
        if (info->dwTypeData && info->cch) {
            if (length >= info->cch) length = info->cch - 1;
            for (UINT i = 0; i < length; i++) info->dwTypeData[i] = item->Text[i];
            info->dwTypeData[length] = 0;
        }
        info->cch = length;
    }
    return TRUE;
}

USER32_API BOOL SetMenuDefaultItem(HMENU handle, UINT id, UINT by_position)
{
    SV_USER_MENU *menu = sv_menu(handle);
    if (!menu) return FALSE;
    SV_MENU_ITEM *item = 0;
    if (id != (UINT)-1) {
        for (UINT i = 0; i < menu->Count; i++)
            if ((by_position ? i : menu->Items[i].Id) == id) {
                item = &menu->Items[i];
                break;
            }
        if (!item) return FALSE;
    }
    for (UINT i = 0; i < menu->Count; i++) menu->Items[i].State &= ~0x1000u;
    if (item) item->State |= 0x1000; /* MFS_DEFAULT */
    return TRUE;
}

USER32_API BOOL RemoveMenu(HMENU menu,UINT id,UINT flags)
{
    SV_USER_MENU *m=0;UINT index=0;
    if(!sv_menu_find(sv_menu(menu),id,(flags&MF_BY_POS)!=0,0,&m,&index))return FALSE;
    for(UINT i=index;i+1<m->Count;i++)m->Items[i]=m->Items[i+1];m->Count--;return TRUE;
}
USER32_API BOOL DeleteMenu(HMENU menu,UINT id,UINT flags)
{
    SV_MENU_ITEM *item=sv_menu_find(sv_menu(menu),id,(flags&MF_BY_POS)!=0,0,0,0);if(!item)return FALSE;
    if(item->Sub)DestroyMenu(item->Sub);return RemoveMenu(menu,id,flags);
}
USER32_API HMENU GetSubMenu(HMENU menu,int position)
{ SV_USER_MENU *m=sv_menu(menu);return m&&position>=0&&(UINT)position<m->Count?m->Items[position].Sub:0; }
USER32_API int GetMenuItemCount(HMENU menu){SV_USER_MENU *m=sv_menu(menu);return m?(int)m->Count:-1;}
USER32_API UINT GetMenuItemID(HMENU menu,int position)
{ SV_USER_MENU *m=sv_menu(menu);return m&&position>=0&&(UINT)position<m->Count&&!m->Items[position].Sub?m->Items[position].Id:(UINT)-1; }
USER32_API DWORD GetMenuState(HMENU menu,UINT id,UINT flags)
{
    SV_MENU_ITEM *item=sv_menu_find(sv_menu(menu),id,(flags&MF_BY_POS)!=0,0,0,0);if(!item)return (DWORD)-1;
    SV_USER_MENU *sub=sv_menu(item->Sub);return item->Type|item->State|(sub?MF_POP|((sub->Count&255)<<8):0);
}
USER32_API BOOL EnableMenuItem(HMENU menu,UINT id,UINT flags)
{
    SV_MENU_ITEM *item=sv_menu_find(sv_menu(menu),id,(flags&MF_BY_POS)!=0,0,0,0);if(!item)return (BOOL)-1;
    UINT old=item->State&MF_DISABLED_;item->State=(item->State&~MF_DISABLED_)|(flags&MF_DISABLED_);return old;
}
USER32_API DWORD CheckMenuItem(HMENU menu,UINT id,UINT flags)
{
    SV_MENU_ITEM *item=sv_menu_find(sv_menu(menu),id,(flags&MF_BY_POS)!=0,0,0,0);if(!item)return (DWORD)-1;
    UINT old=item->State&MF_CHECKED_;item->State=(item->State&~MF_CHECKED_)|(flags&MF_CHECKED_);return old;
}
USER32_API BOOL CheckMenuRadioItem(HMENU menu,UINT first,UINT last,UINT check,UINT flags)
{
    SV_USER_MENU *m=sv_menu(menu);if(!m||last<first)return FALSE;
    if(!(flags&MF_BY_POS)&&!sv_menu_find(m,first,FALSE,0,&m,0))return FALSE;
    BOOL found=FALSE;
    for(UINT i=0;i<m->Count;i++){
        SV_MENU_ITEM *item=&m->Items[i];UINT key=flags&MF_BY_POS?i:item->Id;
        if(key<first||key>last)continue;
        item->Type|=0x200;item->State=(item->State&~MF_CHECKED_)|(key==check?MF_CHECKED_:0);found=TRUE;
    }
    return found;
}
/* Standard MENU resources are a recursive stream of WORD flags/id/text.
 * A popup omits the id and immediately contains its child item stream. */
static BOOL sv_menu_parse(HMENU menu,const unsigned char **cursor,const unsigned char *end,UINT depth)
{
    if(depth>16)return FALSE;
    for(;;){
        const unsigned char *p=*cursor;if(p+2>end)return FALSE;
        WORD flags=*(const WORD*)p;p+=2;WORD id=0;
        if(!(flags&MF_POP)){if(p+2>end)return FALSE;id=*(const WORD*)p;p+=2;}
        const WCHAR *text=(const WCHAR*)p;
        do {if(p+2>end)return FALSE;p+=2;}while(*(const WORD*)(p-2));
        HMENU sub=0;*cursor=p;
        if(flags&MF_POP){sub=CreatePopupMenu();if(!sub)return FALSE;if(!sv_menu_parse(sub,cursor,end,depth+1)){DestroyMenu(sub);return FALSE;}}
        if(!AppendMenuW(menu,flags&~0x80u,sub?(UINT_PTR)sub:id,text)){if(sub)DestroyMenu(sub);return FALSE;}
        if(flags&0x80)return TRUE;
    }
}
/* MENUEX items carry DWORD type/state/id, a WORD resInfo, a terminated
 * UTF-16 label, and (for popup items) an aligned help ID before children. */
static BOOL sv_menu_parse_ex(HMENU menu, const unsigned char **cursor,
                              const unsigned char *end, UINT depth)
{
    if (depth > 16) return FALSE;
    for (;;) {
        const unsigned char *p = (const unsigned char *)
            (((UINT_PTR)*cursor + 3) & ~(UINT_PTR)3);
        if (p > end || (UINT_PTR)(end - p) < 14) return FALSE;
        SV_MENU_ITEM pending = {0};
        pending.Type = *(const DWORD *)p;
        pending.State = *(const DWORD *)(p + 4);
        pending.Id = *(const DWORD *)(p + 8);
        WORD info = *(const WORD *)(p + 12);
        p += 14;
        const WCHAR *text = (const WCHAR *)p;
        do {
            if (p + 2 > end) return FALSE;
            p += 2;
        } while (*(const WORD *)(p - 2));
        sv_menu_text(pending.Text, text);
        if (info & 1) {
            p = (const unsigned char *)(((UINT_PTR)p + 3) & ~(UINT_PTR)3);
            if (p > end || (UINT_PTR)(end - p) < 4) return FALSE;
            p += 4; /* Popup help ID. */
            pending.Sub = CreatePopupMenu();
            if (!pending.Sub) return FALSE;
            if (!sv_menu_parse_ex(pending.Sub, &p, end, depth + 1)) {
                DestroyMenu(pending.Sub);
                return FALSE;
            }
        }
        SV_MENU_ITEM *item = sv_menu_insert(sv_menu(menu), SV_MENU_ITEMS);
        if (!item) {
            if (pending.Sub) DestroyMenu(pending.Sub);
            return FALSE;
        }
        *item = pending;
        *cursor = p;
        if (info & 0x80) return TRUE;
    }
}

USER32_API HMENU LoadMenuW(HINSTANCE instance,const WCHAR *name)
{
    HANDLE r=FindResourceW(instance,name,MAKEINTRESOURCEW(4));if(!r)return 0;
    const unsigned char *data=LockResource(LoadResource(instance,r));DWORD size=SizeofResource(instance,r);
    if (!data || size < 4) return 0;
    WORD version = *(const WORD *)data;
    if (version > 1) return 0;
    UINT start = 4 + *(const WORD *)(data + 2);
    if (start > size || (version == 1 && start < 8)) return 0;
    HMENU menu = CreateMenu();
    if (!menu) return 0;
    const unsigned char *p = data + start;
    BOOL parsed = version ? sv_menu_parse_ex(menu, &p, data + size, 0) :
                            sv_menu_parse(menu, &p, data + size, 0);
    if (!parsed) { DestroyMenu(menu); return 0; }
    return menu;
}
USER32_API HMENU GetMenu(HWND hwnd){return (HMENU)(UINT_PTR)NtUserMenuBar(hwnd,0,0,0,0);}
USER32_API BOOL DrawMenuBar(HWND hwnd)
{
    HMENU menu=GetMenu(hwnd);SV_USER_MENU *m=sv_menu(menu);WCHAR labels[16][64]={{0}};
    UINT count=m?m->Count:0;if(count>16)count=16;
    for(UINT i=0;i<count;i++){UINT j=0;for(UINT k=0;m->Items[i].Text[k]&&j<63;k++)if(m->Items[i].Text[k]!='&')labels[i][j++]=m->Items[i].Text[k];}
    return NtUserMenuBar(hwnd,2,menu,(const WCHAR*)labels,count)!=0;
}
USER32_API BOOL SetMenu(HWND hwnd,HMENU menu)
{
    if(menu&&!sv_menu(menu))return FALSE;
    if(!NtUserMenuBar(hwnd,1,menu,0,0))return FALSE;
    SV_USER_WINDOW_STATE *s=sv_user_state(hwnd);if(s)s->Menu=menu;return DrawMenuBar(hwnd);
}

typedef struct { ATOM Atom; HINSTANCE Instance; WCHAR Class[96],Name[128]; const WCHAR *Resource; } SV_CLASS_MENU;
static SV_CLASS_MENU g_sv_class_menus[64];
static void sv_user_record_class(const WNDCLASSEXW *wc,ATOM atom)
{
    if(!wc->lpszMenuName)return;
    SV_CLASS_MENU *c=0;
    for(UINT i=0;i<64;i++){if(g_sv_class_menus[i].Atom==atom){c=&g_sv_class_menus[i];break;}if(!g_sv_class_menus[i].Atom&&!c)c=&g_sv_class_menus[i];}
    if(!c)return;*c=(SV_CLASS_MENU){0};c->Atom=atom;c->Instance=wc->hInstance;
    for(UINT i=0;wc->lpszClassName[i]&&i<95;i++)c->Class[i]=wc->lpszClassName[i];
    if(IS_INTRESOURCE(wc->lpszMenuName))c->Resource=wc->lpszMenuName;
    else {sv_menu_text(c->Name,wc->lpszMenuName);c->Resource=c->Name;}
}
static HMENU sv_user_default_menu(const WCHAR *name,HINSTANCE instance)
{
    for(UINT i=0;i<64;i++){
        SV_CLASS_MENU *c=&g_sv_class_menus[i];if(!c->Atom)continue;
        if(instance&&c->Instance&&instance!=c->Instance)continue;
        if(IS_INTRESOURCE(name)?(UINT_PTR)name!=c->Atom:!sv_user_wide_equal(name,c->Class))continue;
        return LoadMenuW(c->Instance,c->Resource);
    }
    return 0;
}
static void sv_user_attach_menu(HWND hwnd){DrawMenuBar(hwnd);}

/* Popup menus run a normal USER message loop so paints and owner messages
 * continue to dispatch while selection/capture belongs to the menu window. */
typedef struct { HWND Window,Owner; HMENU Menu; int Hot,Sub,Width,Height; BOOL Done; UINT Command; } SV_MENU_RUN;
static int sv_menu_row_y(SV_USER_MENU *m,UINT index)
{ int y=3;for(UINT i=0;i<index&&i<m->Count;i++)y+=(m->Items[i].Type&MF_SEPARATOR_)?8:24;return y; }
static int sv_menu_row_at(SV_USER_MENU *m,int x,int y,int width)
{
    if(x<0||x>=width)return -1;
    for(UINT i=0;i<m->Count;i++)if(y>=sv_menu_row_y(m,i)&&y<sv_menu_row_y(m,i+1))return (int)i;
    return -1;
}
static BOOL sv_menu_selectable(SV_MENU_ITEM *item)
{ return !(item->Type&MF_SEPARATOR_)&&!(item->State&MF_DISABLED_); }
static void sv_menu_choose(SV_MENU_RUN *run,SV_USER_MENU *m,int index)
{
    if(index<0||(UINT)index>=m->Count||!sv_menu_selectable(&m->Items[index]))return;
    if(m->Items[index].Sub)run->Sub=index;
    else {run->Command=m->Items[index].Id;run->Done=TRUE;}
}
static LRESULT sv_menu_popup_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    if(msg==WM_NCCREATE){SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);return TRUE;}
    SV_MENU_RUN *run=(SV_MENU_RUN*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(!run)return DefWindowProcW(hwnd,msg,wp,lp);
    SV_USER_MENU *m=sv_menu(run->Menu);if(!m){run->Done=TRUE;return 0;}
    int x=(short)(lp&65535),y=(short)((lp>>16)&65535);
    if(msg==WM_PAINT){
        PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(!dc)return 0;
        SelectObject(dc,GetStockObject(DEFAULT_GUI_FONT));SetBkMode(dc,TRANSPARENT);
        SvControlFill(dc,0,0,run->Width,run->Height,RGB(250,250,250));
        for(UINT i=0;i<m->Count;i++){
            SV_MENU_ITEM *item=&m->Items[i];int top=sv_menu_row_y(m,i);
            if(item->Type&MF_SEPARATOR_){SvControlFill(dc,26,top+3,run->Width-32,1,RGB(210,210,210));continue;}
            BOOL hot=(int)i==run->Hot&&sv_menu_selectable(item);
            if(hot)SvControlFill(dc,2,top,run->Width-4,24,RGB(205,232,255));
            SetTextColor(dc,item->State&MF_DISABLED_?RGB(128,128,128):RGB(0,0,0));
            RECT r={28,top,run->Width-22,top+24};WCHAR label[SV_MENU_TEXT];UINT n=0;
            UINT out=0;while(item->Text[n]&&item->Text[n]!='\t'&&n+1<SV_MENU_TEXT){if(item->Text[n]=='&'&&item->Text[n+1]){n++;if(item->Text[n]!='&')continue;}label[out++]=item->Text[n++];}label[out]=0;
            SvGdiDrawTextW(dc,label,out,&r,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
            if(item->Text[n]=='\t')SvGdiDrawTextW(dc,item->Text+n+1,-1,&r,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);
            if(item->State&MF_CHECKED_){RECT c={6,top,24,top+24};SvGdiDrawTextW(dc,L"*",1,&c,DT_SINGLELINE|DT_VCENTER|DT_CENTER);}
            if(item->Sub){RECT a={run->Width-20,top,run->Width-4,top+24};SvGdiDrawTextW(dc,L">",1,&a,DT_SINGLELINE|DT_VCENTER|DT_CENTER);}
        }
        EndPaint(hwnd,&ps);return 0;
    }
    if(msg==WM_MOUSEMOVE){
        int hot=sv_menu_row_at(m,x,y,run->Width);
        if(hot!=run->Hot){run->Hot=hot;InvalidateRect(hwnd,0,FALSE);}return 0;
    }
    if(msg==WM_LBUTTONDOWN){if(x<0||y<0||x>=run->Width||y>=run->Height)run->Done=TRUE;return 0;}
    if(msg==WM_LBUTTONUP){sv_menu_choose(run,m,sv_menu_row_at(m,x,y,run->Width));return 0;}
    if(msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN){
        if(wp==VK_ESCAPE||wp==VK_LEFT){run->Done=TRUE;return 0;}
        if(wp==VK_RETURN||wp==VK_RIGHT){sv_menu_choose(run,m,run->Hot);return 0;}
        if(wp==VK_DOWN||wp==VK_UP){
            int next=run->Hot<0&&wp==VK_UP?0:run->Hot;
            for(UINT i=0;i<m->Count;i++){next=(next+(wp==VK_DOWN?1:(int)m->Count-1))%(int)m->Count;if(sv_menu_selectable(&m->Items[next]))break;}
            run->Hot=next;InvalidateRect(hwnd,0,FALSE);return 0;
        }
    }
    if(msg==WM_CHAR||msg==0x106){
        WCHAR key=(WCHAR)wp;if(key>='a'&&key<='z')key-=32;
        for(UINT i=0;i<m->Count;i++)for(UINT j=0;m->Items[i].Text[j];j++)if(m->Items[i].Text[j]=='&'){
            WCHAR k=m->Items[i].Text[j+1];if(k>='a'&&k<='z')k-=32;
            if(k==key){sv_menu_choose(run,m,i);return 0;}break;
        }
        return 0;
    }
    if(msg==WM_CLOSE){run->Done=TRUE;return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
USER32_API BOOL TrackPopupMenu(HMENU menu,UINT flags,int x,int y,int reserved,HWND owner,const RECT *exclude)
{
    (void)reserved;(void)exclude;
    SV_USER_MENU *m=sv_menu(menu);if(!m||!owner||!m->Count)return FALSE;
    SendMessageW(owner,0x211,1,0);SendMessageW(owner,0x117,(WPARAM)menu,0);
    SV_MENU_RUN run={0};run.Menu=menu;run.Owner=owner;run.Hot=-1;run.Sub=-1;run.Width=260;
    HDC measure=GetDC(owner);SelectObject(measure,GetStockObject(DEFAULT_GUI_FONT));
    for(UINT i=0;i<m->Count;i++){SIZE size={0};GetTextExtentPoint32W(measure,m->Items[i].Text,sv_wcslen(m->Items[i].Text),&size);if(size.cx+70>run.Width)run.Width=size.cx+70;}
    ReleaseDC(owner,measure);run.Height=sv_menu_row_y(m,m->Count)+3;
    int sw=GetSystemMetrics(0),sh=GetSystemMetrics(1);
    if(flags&8)x-=run.Width;if(flags&4)x-=run.Width/2;
    if(flags&0x20)y-=run.Height;if(flags&0x10)y-=run.Height/2;
    if(x+run.Width>sw)x=sw-run.Width;if(y+run.Height>sh)y=sh-run.Height;
    if(x<0)x=0;if(y<0)y=0;
    WNDCLASSEXW wc={0};wc.cbSize=sizeof(wc);wc.lpfnWndProc=sv_menu_popup_proc;wc.lpszClassName=L"SvMenuPopup";
    RegisterClassExW(&wc);
    HWND old_focus=GetFocus();
    run.Window=CreateWindowExW(0x80,wc.lpszClassName,L"",WS_POPUP,x,y,run.Width+2,run.Height+2,owner,0,0,&run);
    if(!run.Window){SendMessageW(owner,0x212,1,0);return FALSE;}
    ShowWindow(run.Window,SW_SHOW);HWND old_capture=SetCapture(run.Window);SetFocus(run.Window);
    while(!run.Done){
        MSG msg;int got=GetMessageW(&msg,0,0,0);
        if(got<=0){if(got==0)PostQuitMessage((int)msg.wParam);break;}
        TranslateMessage(&msg);DispatchMessageW(&msg);
        if(run.Sub>=0){
            int index=run.Sub;run.Sub=-1;
            UINT command=(UINT)TrackPopupMenu(m->Items[index].Sub,0x100,x+run.Width,y+sv_menu_row_y(m,index),0,owner,0);
            if(command){run.Command=command;run.Done=TRUE;}
        }
    }
    ReleaseCapture();DestroyWindow(run.Window);SetActiveWindow(owner);
    if(old_focus)SetFocus(old_focus);if(old_capture)SetCapture(old_capture);
    SendMessageW(owner,0x125,(WPARAM)menu,0);SendMessageW(owner,0x212,1,0);
    if(flags&0x100)return (BOOL)run.Command;
    if (run.Command && !(flags & 0x80)) {
        SV_USER_WINDOW_STATE *window = sv_user_state(owner);
        UINT message = window && window->SystemMenu == menu ? WM_SYSCOMMAND : WM_COMMAND;
        PostMessageW(owner, message, run.Command, 0);
    }
    return run.Command!=0;
}
typedef struct { DWORD Size; RECT Exclude; } SV_POPUP_PARAMS;

USER32_API BOOL TrackPopupMenuEx(HMENU menu, UINT flags, int x, int y,
    HWND owner, const SV_POPUP_PARAMS *params)
{
    if (params && params->Size != sizeof(*params)) return FALSE;
    return TrackPopupMenu(menu, flags, x, y, 0, owner,
                          params ? &params->Exclude : 0);
}

static BOOL sv_user_menu_command(HWND hwnd,WPARAM wp,LPARAM lp)
{
    HMENU handle=GetMenu(hwnd);SV_USER_MENU *menu=sv_menu(handle);if(!menu)return FALSE;
    SendMessageW(hwnd,0x116,(WPARAM)handle,0);
    RECT rect;GetWindowRect(hwnd,&rect);int x=rect.left+1;
    POINT origin={0,0};NtUserMapWindowPoints(hwnd,0,&origin,1);
    int click=(short)(lp&65535);int index=0;
    HDC dc=GetDC(hwnd);SelectObject(dc,GetStockObject(DEFAULT_GUI_FONT));
    for(UINT i=0;i<menu->Count;i++){
        WCHAR label[SV_MENU_TEXT];UINT n=0;for(UINT j=0;menu->Items[i].Text[j]&&n+1<SV_MENU_TEXT;j++)if(menu->Items[i].Text[j]!='&')label[n++]=menu->Items[i].Text[j];label[n]=0;
        SIZE size={0};GetTextExtentPoint32W(dc,label,n,&size);
        if((wp&0xfff0)==0xf100||click<x+size.cx+16){index=i;break;}x+=size.cx+16;
    }
    ReleaseDC(hwnd,dc);
    if((UINT)index<menu->Count&&menu->Items[index].Sub)TrackPopupMenu(menu->Items[index].Sub,0,x,origin.y,0,hwnd,0);
    return TRUE;
}

/* USER handles may reuse slots with a new generation. Drop client metadata
 * after synchronous destruction so repeated popup loops do not exhaust it. */
static void sv_user_forget_window(HWND hwnd)
{
    for(UINT i=0;i<SV_USER_WINDOWS;i++)if(g_sv_windows[i].Used&&g_sv_windows[i].Window==hwnd){
        HMENU owned=(g_sv_windows[i].Style&WS_CHILD)?0:g_sv_windows[i].Menu;
        HMENU system = g_sv_windows[i].SystemMenu;
        g_sv_windows[i]=(SV_USER_WINDOW_STATE){0};
        if(sv_menu(owned))DestroyMenu(owned);
        if(sv_menu(system))DestroyMenu(system);
        return;
    }
}
