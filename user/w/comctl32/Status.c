#include "Internal.h"

typedef struct STATUS_STATE {
    struct STATUS_STATE *next;
    HWND hwnd;
    WCHAR *text;
    DWORD flags;
} STATUS_STATE;
static STATUS_STATE *states;
static unsigned state_lock;
static void lock_states(void) { while (__atomic_exchange_n(&state_lock,1,__ATOMIC_ACQUIRE)) {} }
static void unlock_states(void) { __atomic_store_n(&state_lock,0,__ATOMIC_RELEASE); }
static STATUS_STATE *find_state(HWND hwnd)
{
    lock_states(); STATUS_STATE *found=NULL;
    for(STATUS_STATE *s=states;s;s=s->next)if(s->hwnd==hwnd){found=s;break;}
    unlock_states(); return found;
}
static BOOL set_text(STATUS_STATE *s, const WCHAR *src, BOOL reset_flags)
{
    ULONG_PTR count=0;
    while(src && src[count]) { if(count >= 0x7fffffffu/sizeof(WCHAR)-1) return FALSE; ++count; }
    WCHAR *copy=SvComctlAlloc((count+1)*sizeof(WCHAR)); if(!copy)return FALSE;
    for(ULONG_PTR i=0;i<count;++i)copy[i]=src[i]; copy[count]=0;
    WCHAR *old=s->text; s->text=copy; SvComctlFree(old);
    if(reset_flags)s->flags=0;
    return TRUE;
}
static void draw_status(HWND hwnd, STATUS_STATE *s)
{
    PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps); if(!dc)return;
    RECT r; GetClientRect(hwnd,&r); SvComctlFill(dc,&r,RGB(240,240,240));
    RECT top={r.left,r.top,r.right,r.top+1}; SvComctlFill(dc,&top,RGB(204,204,204));
    if (!(s->flags&SBT_NOBORDERS)) {
        RECT left={r.left,r.top+3,r.left+1,r.bottom-3};
        SvComctlFill(dc,&left,RGB(255,255,255));
        RECT right={r.right-2,r.top+3,r.right-1,r.bottom-3};
        SvComctlFill(dc,&right,RGB(160,160,160));
    }
    SetBkMode(dc,1); SetTextColor(dc,RGB(0,0,0));
    RECT t={r.left+6,r.top+2,r.right-5,r.bottom-2};
    DrawTextW(dc,s->text ? s->text : L"",-1,&t,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    EndPaint(hwnd,&ps);
}
LRESULT SvStatusProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    STATUS_STATE *s=find_state(hwnd);
    if(msg==WM_NCCREATE){
        s=SvComctlAlloc(sizeof(*s)); if(!s)return FALSE;
        s->hwnd=hwnd;s->text=NULL;s->flags=0;lock_states();s->next=states;states=s;unlock_states();
        CREATESTRUCTW *cs=(CREATESTRUCTW *)lp;
        if(cs && cs->lpszName) SendMessageW(hwnd,WM_SETTEXT,0,(LPARAM)cs->lpszName);
        return TRUE;
    }
    if(!s)return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg){
    case WM_PAINT: draw_status(hwnd,s);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_SETTEXT:{
        if(!set_text(s,(const WCHAR *)lp,TRUE))return 0;
        InvalidateRect(hwnd,NULL,FALSE);return 1; }
    case SB_SETTEXTW:
        if ((wp & 0xffu) != 0 || (wp & SBT_OWNERDRAW)) return FALSE;
        if(!set_text(s,(const WCHAR *)lp,FALSE))return 0;
        s->flags=(DWORD)wp; InvalidateRect(hwnd,NULL,FALSE); return 1;
    case WM_SIZE:InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_NCDESTROY:{STATUS_STATE **link=&states;while(*link&&*link!=s)link=&(*link)->next;
        if(*link)*link=s->next;unlock_states();SvComctlFree(s->text);SvComctlFree(s);return 0;}
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
BOOL SvRegisterStatus(void)
{
    WNDCLASSEXW cls;cls.cbSize=sizeof(cls);cls.style=0;
    cls.lpfnWndProc=SvStatusProc;cls.cbClsExtra=0;cls.cbWndExtra=0;
    cls.hInstance=NULL;cls.hIcon=NULL;cls.hCursor=NULL;cls.hbrBackground=NULL;
    cls.lpszMenuName=NULL;cls.lpszClassName=STATUSCLASSNAMEW;cls.hIconSm=NULL;
    return RegisterClassExW(&cls)!=0;
}
