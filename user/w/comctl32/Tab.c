#include "Internal.h"

typedef struct TAB_ITEM { WCHAR *text; } TAB_ITEM;
typedef struct TAB_STATE {
    struct TAB_STATE *next;
    HWND hwnd,parent;
    UINT id;
    TAB_ITEM *items;
    int count,capacity,selected;
} TAB_STATE;
static TAB_STATE *states;
static unsigned state_lock;
static void lock_states(void){while(__atomic_exchange_n(&state_lock,1,__ATOMIC_ACQUIRE)){}}
static void unlock_states(void){__atomic_store_n(&state_lock,0,__ATOMIC_RELEASE);}
static TAB_STATE *find_state(HWND hwnd)
{
    lock_states();TAB_STATE *found=NULL;
    for(TAB_STATE *s=states;s;s=s->next)if(s->hwnd==hwnd){found=s;break;}
    unlock_states();return found;
}
static WCHAR *copy_text(const WCHAR *src)
{
    ULONG_PTR n=0;while(src&&src[n]){if(n>=0x7fffffffu/sizeof(WCHAR)-1)return NULL;++n;}
    WCHAR *dst=SvComctlAlloc((n+1)*sizeof(WCHAR));if(!dst)return NULL;
    for(ULONG_PTR i=0;i<n;++i)dst[i]=src[i];dst[n]=0;return dst;
}
static int tab_width(HDC dc,const TAB_ITEM *item)
{
    SIZE extent;extent.cx=0;extent.cy=0;int n=0;while(item->text&&item->text[n])++n;
    if(n)GetTextExtentPoint32W(dc,item->text,n,&extent);
    return extent.cx+28;
}
static RECT tab_rect(HDC dc,TAB_STATE *s,int index,int *next_x)
{
    int x=4;for(int i=0;i<index;++i)x+=tab_width(dc,&s->items[i]);
    int width=tab_width(dc,&s->items[index]);if(next_x)*next_x=x+width;
    RECT client;GetClientRect(s->hwnd,&client);
    int bottom=client.top+26;if(bottom>client.bottom-2)bottom=client.bottom-2;
    return (RECT){x,client.top+2,x+width,bottom};
}
static void draw_tab(HWND hwnd,TAB_STATE *s)
{
    PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(!dc)return;
    RECT client;GetClientRect(hwnd,&client);SvComctlFill(dc,&client,RGB(255,255,255));
    HPEN line=CreatePen(0,1,RGB(173,173,173));HGDIOBJ oldpen=line?SelectObject(dc,line):NULL;
    MoveToEx(dc,client.left,client.bottom-2,NULL);LineTo(dc,client.right,client.bottom-2);
    if(line){SelectObject(dc,oldpen);DeleteObject(line);}
    for(int i=0;i<s->count;++i){RECT r=tab_rect(dc,s,i,NULL);
        HBRUSH brush=CreateSolidBrush(i==s->selected?RGB(255,255,255):RGB(240,240,240));
        HPEN edge=CreatePen(0,1,RGB(173,173,173));HGDIOBJ oldbrush=brush?SelectObject(dc,brush):NULL;
        oldpen=edge?SelectObject(dc,edge):NULL;Rectangle(dc,r.left,r.top,r.right,r.bottom);
        if(edge){SelectObject(dc,oldpen);DeleteObject(edge);}if(brush){SelectObject(dc,oldbrush);DeleteObject(brush);}
        if(i==s->selected){HBRUSH white=CreateSolidBrush(RGB(255,255,255));if(white){oldbrush=SelectObject(dc,white);PatBlt(dc,r.left+1,r.bottom-1,r.right-r.left-2,2,PATCOPY);SelectObject(dc,oldbrush);DeleteObject(white);}}
        RECT text={r.left+8,r.top+2,r.right-8,r.bottom-2};SetBkMode(dc,1);SetTextColor(dc,RGB(0,0,0));
        DrawTextW(dc,s->items[i].text,-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    }
    EndPaint(hwnd,&ps);
}
static void notify_selection(TAB_STATE *s)
{
    NMHDR hdr;hdr.hwndFrom=s->hwnd;hdr.idFrom=s->id;hdr.code=TCN_SELCHANGE;
    SendMessageW(s->parent,WM_NOTIFY,(WPARAM)s->id,(LPARAM)&hdr);
}
static int hit_tab(HDC dc,TAB_STATE *s,int x)
{
    for(int i=0;i<s->count;++i){int next;tab_rect(dc,s,i,&next);if(x>=next-tab_width(dc,&s->items[i])&&x<next)return i;}
    return -1;
}
static BOOL reserve_items(TAB_STATE *s,int wanted)
{
    if(wanted<=s->capacity)return TRUE;
    int cap=s->capacity?s->capacity:4;while(cap<wanted){if(cap>0x3fffffff/2)return FALSE;cap*=2;}
    TAB_ITEM *fresh=SvComctlAlloc((ULONG_PTR)cap*sizeof(*fresh));if(!fresh)return FALSE;
    for(int i=0;i<s->count;++i)fresh[i]=s->items[i];
    SvComctlFree(s->items);s->items=fresh;s->capacity=cap;return TRUE;
}
static BOOL insert_item(TAB_STATE *s,int index,const TCITEMW *item)
{
    if(index<0||index>s->count||!item||!(item->mask&TCIF_TEXT))return FALSE;
    WCHAR *text=copy_text(item->pszText);if(!text)return FALSE;
    if(s->count==0x3fffffff||!reserve_items(s,s->count+1)){SvComctlFree(text);return FALSE;}
    for(int i=s->count;i>index;--i)s->items[i]=s->items[i-1];
    s->items[index].text=text;++s->count;if(s->selected>=index)++s->selected;
    if(s->selected<0)s->selected=0;return TRUE;
}
LRESULT SvTabProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    TAB_STATE *s=find_state(hwnd);
    if(msg==WM_NCCREATE){CREATESTRUCTW *cs=(CREATESTRUCTW *)lp;s=SvComctlAlloc(sizeof(*s));if(!s)return FALSE;
        s->hwnd=hwnd;s->parent=cs->hwndParent;s->id=(UINT)(UINT_PTR)cs->hMenu;s->items=NULL;s->count=0;s->capacity=0;s->selected=-1;
        lock_states();s->next=states;states=s;unlock_states();return TRUE;}
    if(!s)return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg){
    case WM_PAINT:draw_tab(hwnd,s);return 0;
    case WM_ERASEBKGND:return 1;
    case TCM_GETITEMCOUNT:return s->count;
    case TCM_GETCURSEL:return s->selected;
    case TCM_SETCURSEL:{int index=(int)wp,old=s->selected;if(index<0||index>=s->count)return -1;s->selected=index;InvalidateRect(hwnd,NULL,FALSE);return old;}
    case TCM_INSERTITEMW:{int index=(int)wp;if(!insert_item(s,index,(const TCITEMW *)lp))return -1;if(s->selected<0)s->selected=0;InvalidateRect(hwnd,NULL,FALSE);return index;}
    case TCM_DELETEITEM:{int index=(int)wp;if(index<0||index>=s->count)return FALSE;SvComctlFree(s->items[index].text);
        for(int i=index;i<s->count-1;++i)s->items[i]=s->items[i+1];--s->count;
        if(!s->count)s->selected=-1;else if(s->selected>index)--s->selected;else if(s->selected>=s->count)s->selected=s->count-1;
        InvalidateRect(hwnd,NULL,FALSE);return TRUE;}
    case TCM_DELETEALLITEMS:for(int i=0;i<s->count;++i)SvComctlFree(s->items[i].text);s->count=0;s->selected=-1;InvalidateRect(hwnd,NULL,FALSE);return TRUE;
    case WM_LBUTTONDOWN:{HDC dc=GetDC(hwnd);if(!dc)return 0;int index=hit_tab(dc,s,(short)LOWORD(lp));ReleaseDC(hwnd,dc);
        if(index>=0&&index!=s->selected){s->selected=index;SetFocus(hwnd);InvalidateRect(hwnd,NULL,FALSE);notify_selection(s);}return 0;}
    case WM_KEYDOWN:{int next=s->selected;if(wp==VK_LEFT)--next;else if(wp==VK_RIGHT)++next;else break;
        if(next>=0&&next<s->count&&next!=s->selected){s->selected=next;InvalidateRect(hwnd,NULL,FALSE);notify_selection(s);}return 0;}
    case WM_NCDESTROY:{lock_states();TAB_STATE **link=&states;while(*link&&*link!=s)link=&(*link)->next;if(*link)*link=s->next;unlock_states();
        for(int i=0;i<s->count;++i)SvComctlFree(s->items[i].text);SvComctlFree(s->items);SvComctlFree(s);return 0;}
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
BOOL SvRegisterTab(void)
{
    WNDCLASSEXW cls;cls.cbSize=sizeof(cls);cls.style=0;cls.lpfnWndProc=SvTabProc;
    cls.cbClsExtra=0;cls.cbWndExtra=0;cls.hInstance=NULL;cls.hIcon=NULL;cls.hCursor=NULL;
    cls.hbrBackground=NULL;cls.lpszMenuName=NULL;cls.lpszClassName=WC_TABCONTROLW;cls.hIconSm=NULL;
    return RegisterClassExW(&cls)!=0;
}
