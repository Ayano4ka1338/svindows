#include "Control.h"
USER32_API HMENU CreatePopupMenu(void);
USER32_API BOOL AppendMenuW(HMENU,UINT,UINT_PTR,const WCHAR*);
USER32_API BOOL DestroyMenu(HMENU);
USER32_API BOOL TrackPopupMenu(HMENU,UINT,int,int,int,HWND,const RECT*);
typedef struct { WCHAR *text; LPARAM data; } COMBO_ITEM;
typedef struct { COMBO_ITEM *items; int count,capacity,selected,dropped,drop_height; } COMBO_STATE;
static COMBO_STATE *combo_state(SV_CONTROL *c)
{
    if(!c->private_data){COMBO_STATE *s=SvControlAlloc(sizeof(*s));if(!s)return 0;s->selected=-1;c->private_data=s;}
    return c->private_data;
}
void SvComboDispose(SV_CONTROL *c)
{
    COMBO_STATE *s=c->private_data;if(!s)return;
    for(int i=0;i<s->count;i++)SvControlFree(s->items[i].text);
    SvControlFree(s->items);SvControlFree(s);c->private_data=0;
}
static int combo_compare(const WCHAR *a,const WCHAR *b,BOOL prefix)
{
    while(*a&&*b){WCHAR x=*a++,y=*b++;if(x>='a'&&x<='z')x-=32;if(y>='a'&&y<='z')y-=32;if(x!=y)return x<y?-1:1;}
    return !*b&&(prefix||!*a)?0:!*a?-1:1;
}
static int combo_insert(SV_CONTROL *c,COMBO_STATE *s,int index,const WCHAR *text)
{
    if(!text||s->count>=1024)return -1;
    int length=0;while(text[length]&&length<32767)length++;
    WCHAR *copy=SvControlAlloc((length+1)*sizeof(WCHAR));if(!copy)return -1;
    for(int i=0;i<=length;i++)copy[i]=text[i];copy[length]=0;
    if(s->count==s->capacity){int capacity=s->capacity?s->capacity*2:8;COMBO_ITEM *items=SvControlAlloc(capacity*sizeof(*items));if(!items){SvControlFree(copy);return -1;}for(int i=0;i<s->count;i++)items[i]=s->items[i];SvControlFree(s->items);s->items=items;s->capacity=capacity;}
    if(index<0||index>s->count)index=s->count;
    if(c->style&0x100){index=0;while(index<s->count&&combo_compare(s->items[index].text,text,FALSE)<0)index++;}
    for(int i=s->count;i>index;i--)s->items[i]=s->items[i-1];
    s->items[index]=(COMBO_ITEM){copy,0};s->count++;
    if(s->selected>=index)s->selected++;return index;
}
static int combo_select(SV_CONTROL *c,COMBO_STATE *s,int index,BOOL notify)
{
    if(index<0||index>=s->count){s->selected=-1;SvEditSetText(&c->model,L"");SvControlInvalidate(c);return -1;}
    BOOL changed=index!=s->selected;s->selected=index;SvEditSetText(&c->model,s->items[index].text);SvControlInvalidate(c);
    if(changed&&notify)SvControlNotify(c,1);
    return index;
}
static void combo_drop(SV_CONTROL *c,COMBO_STATE *s)
{
    if(s->dropped||!s->count)return;
    HMENU menu=CreatePopupMenu();if(!menu)return;
    for(int i=0;i<s->count;i++)AppendMenuW(menu,i==s->selected?8:0,(UINT_PTR)i+1,s->items[i].text);
    s->dropped=TRUE;SvControlNotify(c,7);
    if(c->dead){s->dropped=FALSE;DestroyMenu(menu);return;}
    RECT r;GetWindowRect(c->hwnd,&r);
    UINT selected=(UINT)TrackPopupMenu(menu,0x100,r.left,r.bottom,0,c->hwnd,0);DestroyMenu(menu);s->dropped=FALSE;
    if(c->dead)return;
    if(selected){combo_select(c,s,(int)selected-1,TRUE);if(c->dead)return;SvControlNotify(c,9);}
    else SvControlNotify(c,10);
    if(!c->dead)SvControlNotify(c,8);
}
LRESULT SvComboMessage(SV_CONTROL *c,UINT msg,WPARAM wp,LPARAM lp)
{
    COMBO_STATE *s=combo_state(c);if(!s)return -1;
    if(msg==WM_SIZE){
        int height=(int)((lp>>16)&65535),width=(int)(lp&65535);TEXTMETRICW metrics={0};
        HDC dc=GetDC(c->hwnd);HGDIOBJ old=SelectObject(dc,c->font);GetTextMetricsW(dc,&metrics);if(old)SelectObject(dc,old);ReleaseDC(c->hwnd,dc);
        int closed=metrics.tmHeight+6;if(closed<22)closed=22;
        if(height>closed){s->drop_height=height;SetWindowPos(c->hwnd,0,0,0,width,closed,0x16);}
        return 0;
    }
    if(msg==0x143||msg==0x14a)return combo_insert(c,s,msg==0x143?-1:(int)wp,(const WCHAR*)lp);
    if(msg==0x146)return s->count;
    if(msg==0x147)return s->selected;
    if(msg==0x14e)return combo_select(c,s,(int)wp,FALSE);
    if(msg==0x148||msg==0x149){
        if(wp>=(UINT)s->count)return -1;
        const WCHAR *t=s->items[wp].text;int n=0;while(t[n]){if(msg==0x148&&lp)((WCHAR*)lp)[n]=t[n];n++;}
        if(msg==0x148&&lp)((WCHAR*)lp)[n]=0;return n;
    }
    if(msg==0x150||msg==0x151){if(wp>=(UINT)s->count)return -1;if(msg==0x151){s->items[wp].data=lp;return 0;}return s->items[wp].data;}
    if(msg==0x14b){for(int i=0;i<s->count;i++)SvControlFree(s->items[i].text);s->count=0;combo_select(c,s,-1,FALSE);return 0;}
    if(msg==0x144){if(wp>=(UINT)s->count)return -1;SvControlFree(s->items[wp].text);for(int i=(int)wp;i+1<s->count;i++)s->items[i]=s->items[i+1];s->count--;if(s->selected==(int)wp)combo_select(c,s,-1,FALSE);else if(s->selected>(int)wp)s->selected--;return s->count;}
    if(msg==0x14c||msg==0x158||msg==0x14d){
        if(!lp||!s->count)return -1;int start=(int)wp;
        if(start<-1||start>=s->count)start=-1;
        for(int n=1;n<=s->count;n++){int i=(start+n)%s->count;if(!combo_compare(s->items[i].text,(const WCHAR*)lp,msg!=0x158)){if(msg==0x14d)combo_select(c,s,i,FALSE);return i;}}
        return -1;
    }
    if(msg==0x157)return s->dropped;
    if(msg==0x14f){if(wp)combo_drop(c,s);return TRUE;}
    if(msg==0x154)return 22;
    if(msg==0x152){if(!lp)return FALSE;GetWindowRect(c->hwnd,(RECT*)lp);((RECT*)lp)->bottom+=s->count*24;return TRUE;}
    if(msg==WM_PAINT){
        PAINTSTRUCT ps;HDC dc=BeginPaint(c->hwnd,&ps);if(!dc)return 0;RECT r;GetClientRect(c->hwnd,&r);
        SvControlFill(dc,0,0,r.right,r.bottom,RGB(140,140,140));SvControlFill(dc,1,1,r.right-2,r.bottom-2,RGB(255,255,255));
        SelectObject(dc,c->font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,c->style&WS_DISABLED?RGB(128,128,128):0);
        RECT text={4,1,r.right-20,r.bottom-1};SvGdiDrawTextW(dc,c->model.text,c->model.length,&text,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        RECT arrow={r.right-18,1,r.right-1,r.bottom-1};SvControlFill(dc,arrow.left,arrow.top,arrow.right-arrow.left,arrow.bottom-arrow.top,RGB(235,235,235));SvGdiDrawTextW(dc,L"v",1,&arrow,DT_SINGLELINE|DT_VCENTER|DT_CENTER);EndPaint(c->hwnd,&ps);return 0;
    }
    if(msg==WM_LBUTTONDOWN){SetFocus(c->hwnd);combo_drop(c,s);return 0;}
    if(msg==WM_KEYDOWN){
        if(wp==VK_SPACE||wp==0x73){combo_drop(c,s);return 0;}
        int next=s->selected;
        if(wp==VK_DOWN&&next+1<s->count)next++;
        else if(wp==VK_UP&&next>0)next--;
        else if(wp==VK_HOME)next=0;else if(wp==VK_END)next=s->count-1;else return DefWindowProcW(c->hwnd,msg,wp,lp);
        combo_select(c,s,next,TRUE);return 0;
    }
    return DefWindowProcW(c->hwnd,msg,wp,lp);
}
