#include "Internal.h"

typedef struct TRACKBAR_STATE {
    struct TRACKBAR_STATE *next;
    HWND hwnd, parent;
    UINT id;
    DWORD style;
    int low, high, position, tick_frequency;
    BOOL dragging;
} TRACKBAR_STATE;
static TRACKBAR_STATE *states;
static unsigned state_lock;
static void lock_states(void) { while (__atomic_exchange_n(&state_lock,1,__ATOMIC_ACQUIRE)) {} }
static void unlock_states(void) { __atomic_store_n(&state_lock,0,__ATOMIC_RELEASE); }
static TRACKBAR_STATE *find_state(HWND hwnd)
{
    lock_states(); TRACKBAR_STATE *found=NULL;
    for(TRACKBAR_STATE *s=states;s;s=s->next)if(s->hwnd==hwnd){found=s;break;}
    unlock_states(); return found;
}
static BOOL vertical(TRACKBAR_STATE *s) { return (s->style&TBS_VERT)!=0; }
static int clamp_position(TRACKBAR_STATE *s, int value)
{ return value<s->low?s->low:value>s->high?s->high:value; }
static RECT client_track(TRACKBAR_STATE *s)
{
    RECT r; GetClientRect(s->hwnd,&r);
    if(vertical(s)){r.top+=10;r.bottom-=10;r.left=(r.left+r.right)/2-2;r.right=r.left+4;}
    else {r.left+=10;r.right-=10;r.top=(r.top+r.bottom)/2-2;r.bottom=r.top+4;}
    return r;
}
static int axis_span(TRACKBAR_STATE *s, RECT r)
{ return vertical(s)?r.bottom-r.top:r.right-r.left; }
static int axis_start(TRACKBAR_STATE *s, RECT r)
{ return vertical(s)?r.top:r.left; }
static int position_axis(TRACKBAR_STATE *s, RECT r)
{
    int span=axis_span(s,r); long long range=(long long)s->high-s->low;
    return axis_start(s,r)+(range>0?(int)((((long long)s->position-s->low)*span)/range):0);
}
static void notify_scroll(TRACKBAR_STATE *s, int code)
{
    UINT msg=vertical(s)?WM_VSCROLL:WM_HSCROLL;
    SendMessageW(s->parent,msg,(WPARAM)MAKELONG(code,(WORD)s->position),(LPARAM)s->hwnd);
}
static void set_from_point(TRACKBAR_STATE *s, int x, int y, BOOL notify)
{
    RECT r=client_track(s); int span=axis_span(s,r); int p=vertical(s)?y:x;
    if(span<=0)return;
    if(p<axis_start(s,r))p=axis_start(s,r);
    if(p>axis_start(s,r)+span)p=axis_start(s,r)+span;
    long long range=(long long)s->high-s->low;
    int value=range>0?s->low+(int)(((long long)(p-axis_start(s,r))*range)/span):s->low;
    value=clamp_position(s,value);
    if(value==s->position)return;
    s->position=value; InvalidateRect(s->hwnd,NULL,FALSE);
    if(notify)notify_scroll(s,TB_THUMBTRACK);
}
static void draw_trackbar(HWND hwnd, TRACKBAR_STATE *s)
{
    PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps); if(!dc)return;
    RECT r; GetClientRect(hwnd,&r); SvComctlFill(dc,&r,RGB(255,255,255));
    RECT channel=client_track(s);
    HPEN pen=CreatePen(0,1,RGB(160,160,160));
    HGDIOBJ oldpen=pen?SelectObject(dc,pen):NULL;
    int cx=(channel.left+channel.right)/2,cy=(channel.top+channel.bottom)/2;
    MoveToEx(dc,vertical(s)?cx:channel.left,vertical(s)?channel.top:cy,NULL);
    LineTo(dc,vertical(s)?cx:channel.right,vertical(s)?channel.bottom:cy);
    if(pen){SelectObject(dc,oldpen);DeleteObject(pen);}
    if(!(s->style&TBS_NOTICKS)){
        int span=axis_span(s,channel); long long range=(long long)s->high-s->low;
        int count=s->tick_frequency>0?(int)(range/s->tick_frequency):10;
        if(count>64)count=64; if(count<1)count=1;
        HPEN tick=CreatePen(0,1,RGB(100,100,100)); oldpen=tick?SelectObject(dc,tick):NULL;
        for(int i=0;i<=count;++i){int a=axis_start(s,channel)+(span*i)/count;
            if(vertical(s)){int x=(r.left+r.right)/2;MoveToEx(dc,x+3,a,NULL);LineTo(dc,x+7,a);}
            else {int y=(r.top+r.bottom)/2;MoveToEx(dc,a,y+3,NULL);LineTo(dc,a,y+7);}}
        if(tick){SelectObject(dc,oldpen);DeleteObject(tick);}
    }
    int a=position_axis(s,channel); RECT thumb;
    if(vertical(s))thumb=(RECT){(r.left+r.right)/2-6,a-5,(r.left+r.right)/2+7,a+6};
    else thumb=(RECT){a-5,(r.top+r.bottom)/2-6,a+6,(r.top+r.bottom)/2+7};
    HBRUSH fill=CreateSolidBrush(s->dragging?RGB(0,84,153):RGB(0,120,215));
    HPEN outline=CreatePen(0,1,RGB(90,90,90)); HGDIOBJ oldbrush=fill?SelectObject(dc,fill):NULL;
    oldpen=outline?SelectObject(dc,outline):NULL; Rectangle(dc,thumb.left,thumb.top,thumb.right,thumb.bottom);
    if(outline){SelectObject(dc,oldpen);DeleteObject(outline);} if(fill){SelectObject(dc,oldbrush);DeleteObject(fill);}
    EndPaint(hwnd,&ps);
}
LRESULT SvTrackbarProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    TRACKBAR_STATE *s=find_state(hwnd);
    if(msg==WM_NCCREATE){
        CREATESTRUCTW *cs=(CREATESTRUCTW *)lp; s=SvComctlAlloc(sizeof(*s));if(!s)return FALSE;
        s->hwnd=hwnd;s->parent=cs->hwndParent;s->id=(UINT)(UINT_PTR)cs->hMenu;s->style=(DWORD)cs->style;
        s->low=0;s->high=100;s->position=0;s->tick_frequency=10;s->dragging=FALSE;
        lock_states();s->next=states;states=s;unlock_states();return TRUE;
    }
    if(!s)return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg){
    case WM_PAINT:draw_trackbar(hwnd,s);return 0;
    case WM_ERASEBKGND:return 1;
    case TBM_GETPOS:return s->position;
    case TBM_GETRANGEMIN:return s->low;
    case TBM_GETRANGEMAX:return s->high;
    case TBM_SETPOS:{int old=s->position;s->position=clamp_position(s,(int)lp);InvalidateRect(hwnd,NULL,FALSE);return old;}
    case TBM_SETRANGEMIN:s->low=(int)lp;if(s->high<s->low)s->high=s->low;s->position=clamp_position(s,s->position);InvalidateRect(hwnd,NULL,FALSE);return 0;
    case TBM_SETRANGEMAX:s->high=(int)lp;if(s->high<s->low)s->low=s->high;s->position=clamp_position(s,s->position);InvalidateRect(hwnd,NULL,FALSE);return 0;
    case TBM_SETRANGE:s->low=(short)LOWORD(lp);s->high=(short)HIWORD(lp);if(s->high<s->low)s->high=s->low;s->position=clamp_position(s,s->position);InvalidateRect(hwnd,NULL,FALSE);return 0;
    case TBM_SETTICFREQ:s->tick_frequency=(int)wp>0?(int)wp:1;InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_LBUTTONDOWN:{s->dragging=TRUE;SetFocus(hwnd);SetCapture(hwnd);set_from_point(s,(short)LOWORD(lp),(short)HIWORD(lp),TRUE);InvalidateRect(hwnd,NULL,FALSE);return 0;}
    case WM_MOUSEMOVE:if(s->dragging)set_from_point(s,(short)LOWORD(lp),(short)HIWORD(lp),TRUE);return 0;
    case WM_LBUTTONUP:if(s->dragging){set_from_point(s,(short)LOWORD(lp),(short)HIWORD(lp),TRUE);s->dragging=FALSE;ReleaseCapture();notify_scroll(s,TB_THUMBPOSITION);InvalidateRect(hwnd,NULL,FALSE);}return 0;
    case WM_CAPTURECHANGED:s->dragging=FALSE;InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_KEYDOWN:{int delta=vertical(s)?((wp==VK_UP)?1:(wp==VK_DOWN)?-1:0):((wp==VK_RIGHT)?1:(wp==VK_LEFT)?-1:0);
        if(wp==VK_HOME)s->position=s->low;else if(wp==VK_END)s->position=s->high;else if(delta)s->position=clamp_position(s,s->position+delta);else break;
        InvalidateRect(hwnd,NULL,FALSE);notify_scroll(s,TB_THUMBPOSITION);return 0;}
    case WM_NCDESTROY:{lock_states();TRACKBAR_STATE **link=&states;while(*link&&*link!=s)link=&(*link)->next;if(*link)*link=s->next;unlock_states();SvComctlFree(s);return 0;}
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
BOOL SvRegisterTrackbar(void)
{
    WNDCLASSEXW cls;cls.cbSize=sizeof(cls);cls.style=0;cls.lpfnWndProc=SvTrackbarProc;
    cls.cbClsExtra=0;cls.cbWndExtra=0;cls.hInstance=NULL;cls.hIcon=NULL;cls.hCursor=NULL;
    cls.hbrBackground=NULL;cls.lpszMenuName=NULL;cls.lpszClassName=TRACKBAR_CLASSW;cls.hIconSm=NULL;
    return RegisterClassExW(&cls)!=0;
}
