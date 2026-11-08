#include "Internal.h"
typedef struct UPDOWN_STATE { struct UPDOWN_STATE *next; HWND hwnd,parent,buddy; UINT id; DWORD style; int low,high,position,base; } UPDOWN_STATE;
static UPDOWN_STATE *updowns;
static UPDOWN_STATE *updown_find(HWND hwnd){for(UPDOWN_STATE *s=updowns;s;s=s->next)if(s->hwnd==hwnd)return s;return 0;}
static void updown_buddy(UPDOWN_STATE *s)
{
    if(!s->buddy||!(s->style&2))return;WCHAR text[36];UINT n=0;long long value=s->position;
    if(value<0){text[n++]='-';value=-value;}
    WCHAR digits[32];UINT count=0;do {digits[count++]=L"0123456789ABCDEF"[value%s->base];value/=s->base;}while(value);
    while(count)text[n++]=digits[--count];text[n]=0;SetWindowTextW(s->buddy,text);
}
static void updown_move(UPDOWN_STATE *s,int delta)
{
    struct { NMHDR hdr; int position,delta; } n={{s->hwnd,s->id,(UINT)-722},s->position,delta};
    HWND hwnd=s->hwnd;
    if(s->parent&&SendMessageW(s->parent,WM_NOTIFY,s->id,(LPARAM)&n))return;
    if(updown_find(hwnd)!=s)return;
    long long next=(long long)s->position+n.delta;
    s->position=next<s->low?s->low:next>s->high?s->high:(int)next;
    updown_buddy(s);InvalidateRect(hwnd,0,FALSE);
}
static LRESULT updown_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    UPDOWN_STATE *s=updown_find(hwnd);
    if(msg==WM_NCCREATE){
        s=SvComctlAlloc(sizeof(*s));if(!s)return FALSE;CREATESTRUCTW *cs=(CREATESTRUCTW*)lp;
        s->hwnd=hwnd;s->parent=cs->hwndParent;s->id=(UINT)(UINT_PTR)cs->hMenu;s->style=(DWORD)cs->style;s->low=0;s->high=100;s->base=10;
        s->next=updowns;updowns=s;return TRUE;
    }
    if(!s)return DefWindowProcW(hwnd,msg,wp,lp);
    if(msg==WM_CREATE){
        if(s->style&16)s->buddy=GetWindow(hwnd,3);
        if(s->buddy&&(s->style&(4|8))){
            RECT r;GetWindowRect(s->buddy,&r);
            extern __declspec(dllimport) int MapWindowPoints(HWND,HWND,POINT*,UINT);
            MapWindowPoints(0,s->parent,(POINT*)&r,2);
            int width=r.right-r.left,height=r.bottom-r.top,spin=18;if(width<spin)spin=width;
            SetWindowPos(s->buddy,0,r.left+(s->style&8?spin:0),r.top,width-spin,height,0x14);
            SetWindowPos(hwnd,0,s->style&8?r.left:r.right-spin,r.top,spin,height,0x14);
        }
        updown_buddy(s);return 0;
    }
    if(msg==WM_USER+101){s->low=(short)(lp>>16);s->high=(short)lp;if(s->low>s->high){int n=s->low;s->low=s->high;s->high=n;}return 0;}
    if(msg==WM_USER+102)return ((UINT)(WORD)s->low<<16)|(WORD)s->high;
    if(msg==WM_USER+111){s->low=(int)wp;s->high=(int)lp;if(s->low>s->high){int n=s->low;s->low=s->high;s->high=n;}return 0;}
    if(msg==WM_USER+112){if(wp)*(int*)wp=s->low;if(lp)*(int*)lp=s->high;return 0;}
    if(msg==WM_USER+103||msg==WM_USER+113){int old=s->position,next=msg==WM_USER+103?(short)lp:(int)lp;s->position=next<s->low?s->low:next>s->high?s->high:next;updown_buddy(s);return old;}
    if(msg==WM_USER+104||msg==WM_USER+114){
        if(s->buddy){WCHAR text[36];GetWindowTextW(s->buddy,text,36);int i=0,sign=1;long long value=0;if(text[0]=='-'){sign=-1;i++;}for(;text[i]>='0'&&text[i]<='9'&&value<2147483648LL;i++)value=value*10+text[i]-'0';value*=sign;if(value>=s->low&&value<=s->high)s->position=(int)value;}
        if(msg==WM_USER+114&&lp)*(BOOL*)lp=FALSE;
        return msg==WM_USER+104?(WORD)s->position:s->position;
    }
    if(msg==WM_USER+105){HWND old=s->buddy;s->buddy=(HWND)wp;updown_buddy(s);return (LRESULT)old;}
    if(msg==WM_USER+106)return (LRESULT)s->buddy;
    if(msg==WM_USER+109){if(wp!=10&&wp!=16)return 0;int old=s->base;s->base=(int)wp;updown_buddy(s);return old;}
    if(msg==WM_USER+110)return s->base;
    if(msg==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(!dc)return 0;RECT r;GetClientRect(hwnd,&r);SvComctlFill(dc,&r,RGB(225,225,225));RECT a={0,0,r.right,r.bottom/2},b={0,r.bottom/2,r.right,r.bottom};SelectObject(dc,GetStockObject(DEFAULT_GUI_FONT));SetBkMode(dc,TRANSPARENT);SetTextColor(dc,0);SvGdiDrawTextW(dc,L"^",1,&a,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SvGdiDrawTextW(dc,L"v",1,&b,DT_CENTER|DT_VCENTER|DT_SINGLELINE);EndPaint(hwnd,&ps);return 0;}
    if(msg==WM_LBUTTONDOWN){RECT r;GetClientRect(hwnd,&r);updown_move(s,(short)(lp>>16)<r.bottom/2?1:-1);return 0;}
    if(msg==WM_KEYDOWN&&(wp==VK_UP||wp==VK_DOWN)){updown_move(s,wp==VK_UP?1:-1);return 0;}
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_NCDESTROY){UPDOWN_STATE **link=&updowns;while(*link&&*link!=s)link=&(*link)->next;if(*link)*link=s->next;SvComctlFree(s);return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
BOOL SvRegisterUpDown(void)
{
    WNDCLASSEXW wc={0};wc.cbSize=sizeof(wc);wc.lpfnWndProc=updown_proc;wc.lpszClassName=UPDOWN_CLASSW;return RegisterClassExW(&wc)!=0;
}
