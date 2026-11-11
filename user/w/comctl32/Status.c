#include "Internal.h"
__declspec(dllimport) LONG_PTR GetWindowLongPtrW(HWND,int);
__declspec(dllimport) int MapWindowPoints(HWND,HWND,POINT*,UINT);

/* SB_SETPARTS allows up to 256 panes; pane 255 is reserved for simple text
 * when addressing SB_SETTEXT. Keep the simple text independent of the panes. */
typedef struct STATUS_PART { WCHAR *text; DWORD flags; int right; } STATUS_PART;
typedef struct STATUS_STATE {
    struct STATUS_STATE *next;
    HWND hwnd, parent;
    STATUS_PART *parts, simple_part;
    UINT count;
    HFONT font;
    int min_height;
    BOOL simple, sizing;
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
static BOOL set_text(WCHAR **slot, const WCHAR *src)
{
    ULONG_PTR count=0;
    while(src && src[count]) { if(count >= 0x7fffffffu/sizeof(WCHAR)-1) return FALSE; ++count; }
    WCHAR *copy=SvComctlAlloc((count+1)*sizeof(WCHAR)); if(!copy)return FALSE;
    for(ULONG_PTR i=0;i<count;++i)copy[i]=src[i]; copy[count]=0;
    WCHAR *old=*slot; *slot=copy; SvComctlFree(old);
    return TRUE;
}
static void status_layout(STATUS_STATE *s)
{
    DWORD style=(DWORD)GetWindowLongPtrW(s->hwnd,-16);
    if(s->sizing || !s->parent || (style&0x0004u))return; /* CCS_NORESIZE */
    RECT parent, current;
    if(!GetClientRect(s->parent,&parent)||!GetWindowRect(s->hwnd,&current))return;
    MapWindowPoints(NULL,s->parent,(POINT *)&current,2);
    int height=16;
    HDC dc=GetDC(s->hwnd);
    if(dc){
        HGDIOBJ old=SelectObject(dc,s->font?s->font:GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW tm;
        if(GetTextMetricsW(dc,&tm)&&tm.tmHeight>0)height=tm.tmHeight;
        if(old)SelectObject(dc,old);
        ReleaseDC(s->hwnd,dc);
    }
    if(height<s->min_height)height=s->min_height;
    height+=6;
    int x=0,y=parent.bottom-height,width=parent.right-parent.left;
    if((style&3u)==1)y=0; /* CCS_TOP */
    if((style&3u)==2)y=current.top; /* CCS_NOMOVEY */
    if(style&8u){x=current.left;y=current.top;width=current.right-current.left;} /* CCS_NOPARENTALIGN */
    s->sizing=TRUE;
    SetWindowPos(s->hwnd,NULL,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
    /* SetWindowPos can synchronously dispatch WM_SIZE. */
    s->sizing=FALSE;
}
static BOOL status_rect(STATUS_STATE *s,UINT index,RECT *r)
{
    if(index>=s->count||!r)return FALSE;
    GetClientRect(s->hwnd,r);
    r->left=index?(s->parts[index-1].right<0?r->right:s->parts[index-1].right):0;
    if(s->parts[index].right>=0)r->right=s->parts[index].right;
    return TRUE;
}
static void draw_part(HDC dc,STATUS_PART *part,RECT r)
{
    if(r.right<=r.left)return;
    if(!(part->flags&SBT_NOBORDERS)){
        RECT left={r.left,r.top+3,r.left+1,r.bottom-3};
        SvComctlFill(dc,&left,RGB(255,255,255));
        RECT right={r.right-2,r.top+3,r.right-1,r.bottom-3};
        SvComctlFill(dc,&right,RGB(160,160,160));
    }
    RECT t={r.left+6,r.top+2,r.right-5,r.bottom-2};
    DrawTextW(dc,part->text?part->text:L"",-1,&t,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
}
static void draw_status(HWND hwnd,STATUS_STATE *s)
{
    PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps); if(!dc)return;
    RECT r; GetClientRect(hwnd,&r); SvComctlFill(dc,&r,RGB(240,240,240));
    RECT top={r.left,r.top,r.right,r.top+1}; SvComctlFill(dc,&top,RGB(204,204,204));
    HGDIOBJ old=SelectObject(dc,s->font?s->font:GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc,1); SetTextColor(dc,RGB(0,0,0));
    if(s->simple)draw_part(dc,&s->simple_part,r);
    else for(UINT i=0;i<s->count;++i){status_rect(s,i,&r);draw_part(dc,&s->parts[i],r);}
    if(old)SelectObject(dc,old);
    EndPaint(hwnd,&ps);
}
LRESULT SvStatusProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    STATUS_STATE *s=find_state(hwnd);
    if(msg==WM_NCCREATE){
        s=SvComctlAlloc(sizeof(*s)); if(!s)return FALSE;
        STATUS_PART *part=SvComctlAlloc(sizeof(*part));
        if(!part){SvComctlFree(s);return FALSE;}
        *s=(STATUS_STATE){0};*part=(STATUS_PART){0};part->right=-1;
        CREATESTRUCTW *cs=(CREATESTRUCTW *)lp;
        s->hwnd=hwnd;s->parent=cs?cs->hwndParent:NULL;s->parts=part;s->count=1;
        if(cs&&cs->lpszName&&!set_text(&part->text,cs->lpszName)){SvComctlFree(part);SvComctlFree(s);return FALSE;}
        lock_states();s->next=states;states=s;unlock_states();
        return TRUE;
    }
    if(!s)return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg){
    case WM_CREATE: status_layout(s);return 0;
    case WM_PAINT: draw_status(hwnd,s);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_SETTEXT:
        if(!set_text(&s->parts[0].text,(const WCHAR *)lp))return 0;
        s->parts[0].flags=0;InvalidateRect(hwnd,NULL,FALSE);return 1;
    case SB_SETTEXTW:{
        UINT index=wp&0xffu;
        if((index!=255&&index>=s->count)||(wp&SBT_OWNERDRAW))return FALSE;
        STATUS_PART *part=index==255?&s->simple_part:&s->parts[index];
        if(!set_text(&part->text,(const WCHAR *)lp))return FALSE;
        part->flags=(DWORD)wp&0xff00u;
        InvalidateRect(hwnd,NULL,FALSE);return TRUE;}
    case SB_SETPARTS:{
        if(!wp||wp>256||!lp)return FALSE;
        const int *edges=(const int *)lp;
        for(UINT i=0;i<wp;++i)if(edges[i]<-1)return FALSE;
        STATUS_PART *parts=SvComctlAlloc(wp*sizeof(*parts));if(!parts)return FALSE;
        for(UINT i=0;i<wp;++i){parts[i]=i<s->count?s->parts[i]:(STATUS_PART){0};parts[i].right=edges[i];}
        for(UINT i=(UINT)wp;i<s->count;++i)SvComctlFree(s->parts[i].text);
        SvComctlFree(s->parts);s->parts=parts;s->count=(UINT)wp;
        InvalidateRect(hwnd,NULL,FALSE);return TRUE;}
    case WM_USER+6: /* SB_GETPARTS */
        if(lp)for(UINT i=0;i<(wp<s->count?wp:s->count);++i)((int *)lp)[i]=s->parts[i].right;
        return s->count;
    case WM_USER+7: /* SB_GETBORDERS */
        if(!lp)return FALSE;((int *)lp)[0]=0;((int *)lp)[1]=3;((int *)lp)[2]=2;return TRUE;
    case WM_USER+8: /* SB_SETMINHEIGHT: applied on the next WM_SIZE */
        if(wp<=0x7ffffff9u)s->min_height=(int)wp;return 0;
    case WM_USER+10: return status_rect(s,(UINT)wp,(RECT *)lp); /* SB_GETRECT */
    case WM_USER+12: case WM_USER+13: case WM_GETTEXT: case WM_GETTEXTLENGTH:{
        BOOL window_text=msg==WM_GETTEXT||msg==WM_GETTEXTLENGTH;
        UINT index=window_text?0:(UINT)wp&0xffu;
        if(index!=255&&index>=s->count)return 0;
        STATUS_PART *part=index==255?&s->simple_part:&s->parts[index];
        UINT n=0;while(part->text&&part->text[n])++n;
        if(msg==WM_GETTEXTLENGTH)return n;
        if(msg==WM_GETTEXT){
            if(!wp||!lp)return 0;UINT copied=n<wp-1?n:(UINT)wp-1;
            for(UINT i=0;i<copied;++i)((WCHAR *)lp)[i]=part->text[i];((WCHAR *)lp)[copied]=0;return copied;
        }
        if(msg==WM_USER+13){ /* SB_GETTEXTW has caller-sized output */
            if(!lp)return 0;for(UINT i=0;i<n;++i)((WCHAR *)lp)[i]=part->text[i];((WCHAR *)lp)[n]=0;
        }
        return (n&0xffffu)|((LRESULT)part->flags<<16);}
    case SB_SIMPLE:s->simple=wp!=0;InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_USER+14:return s->simple;
    case WM_SETFONT:s->font=(HFONT)wp;status_layout(s);if(lp)InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_GETFONT:return (LRESULT)s->font;
    case WM_SIZE:status_layout(s);InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_NCDESTROY:{lock_states();STATUS_STATE **link=&states;while(*link&&*link!=s)link=&(*link)->next;
        if(*link)*link=s->next;unlock_states();
        for(UINT i=0;i<s->count;++i)SvComctlFree(s->parts[i].text);
        SvComctlFree(s->parts);SvComctlFree(s->simple_part.text);SvComctlFree(s);return 0;}
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
