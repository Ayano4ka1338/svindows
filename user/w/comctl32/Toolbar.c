#include "Internal.h"

enum { TOOL_HEIGHT=30, BUTTON_GAP=2 };
typedef struct TOOLBAR_STATE {
    struct TOOLBAR_STATE *next;
    HWND hwnd,parent;
    UINT id;
    TBBUTTON *buttons;
    int count,capacity,hover,pressed,button_size;
    WCHAR **strings;
    int string_count,string_capacity;
    UINT struct_size;
} TOOLBAR_STATE;
static TOOLBAR_STATE *states;
static unsigned state_lock;
static void lock_states(void){while(__atomic_exchange_n(&state_lock,1,__ATOMIC_ACQUIRE)){} }
static void unlock_states(void){__atomic_store_n(&state_lock,0,__ATOMIC_RELEASE);}
static TOOLBAR_STATE *find_state(HWND hwnd)
{
    lock_states();TOOLBAR_STATE *found=NULL;
    for(TOOLBAR_STATE *s=states;s;s=s->next)if(s->hwnd==hwnd){found=s;break;}
    unlock_states();return found;
}
static const WCHAR *button_text(TOOLBAR_STATE *s,TBBUTTON *button)
{
    if(button->iString>=0&&(ULONG_PTR)button->iString<(ULONG_PTR)s->string_count)
        return s->strings[(int)button->iString];
    return button->iString?(const WCHAR *)(ULONG_PTR)button->iString:L"";
}
static int button_width(HDC dc,TOOLBAR_STATE *s,int i)
{
    TBBUTTON *b=&s->buttons[i];if(b->fsStyle&TBSTYLE_SEP)return 8;
    if(s->button_size>0)return s->button_size;
    SIZE size;size.cx=0;size.cy=0;const WCHAR *text=button_text(s,b);int n=0;while(text[n])++n;
    if(n)GetTextExtentPoint32W(dc,text,n,&size);
    return size.cx+18>42?size.cx+18:42;
}
static RECT button_rect(HDC dc,TOOLBAR_STATE *s,int index)
{
    int x=2;for(int i=0;i<index;++i)x+=button_width(dc,s,i)+BUTTON_GAP;
    int width=button_width(dc,s,index);return (RECT){x,2,x+width,TOOL_HEIGHT-2};
}
static int hit_button(HDC dc,TOOLBAR_STATE *s,int x,int y)
{
    if(y<0||y>=TOOL_HEIGHT)return -1;
    for(int i=0;i<s->count;++i){RECT r=button_rect(dc,s,i);if(x>=r.left&&x<r.right&&!(s->buttons[i].fsState&TBSTATE_HIDDEN))return i;}
    return -1;
}
static void draw_toolbar(HWND hwnd,TOOLBAR_STATE *s)
{
    PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(!dc)return;
    RECT client;GetClientRect(hwnd,&client);SvComctlFill(dc,&client,RGB(240,240,240));
    HPEN edge=CreatePen(0,1,RGB(210,210,210));HGDIOBJ old=edge?SelectObject(dc,edge):NULL;
    MoveToEx(dc,0,client.bottom-1,NULL);LineTo(dc,client.right,client.bottom-1);
    if(edge){SelectObject(dc,old);DeleteObject(edge);}
    for(int i=0;i<s->count;++i){TBBUTTON *b=&s->buttons[i];if(b->fsState&TBSTATE_HIDDEN)continue;
        RECT r=button_rect(dc,s,i);if(b->fsStyle&TBSTYLE_SEP){int x=(r.left+r.right)/2;
            HPEN sep=CreatePen(0,1,RGB(190,190,190));old=sep?SelectObject(dc,sep):NULL;
            MoveToEx(dc,x,7,NULL);LineTo(dc,x,client.bottom-7);if(sep){SelectObject(dc,old);DeleteObject(sep);}continue;}
        if(!(b->fsState&TBSTATE_ENABLED))continue;
        BOOL active=i==s->pressed||(b->fsState&TBSTATE_PRESSED);
        BOOL hot=i==s->hover;
        if(active||hot||(b->fsState&TBSTATE_CHECKED)){
            HBRUSH fill=CreateSolidBrush(active?RGB(204,228,247):RGB(229,241,251));
            HPEN line=CreatePen(0,1,RGB(100,160,210));HGDIOBJ ob=fill?SelectObject(dc,fill):NULL;
            old=line?SelectObject(dc,line):NULL;Rectangle(dc,r.left,r.top,r.right,r.bottom);
            if(line){SelectObject(dc,old);DeleteObject(line);}if(fill){SelectObject(dc,ob);DeleteObject(fill);}}
        RECT label={r.left+5,r.top+2,r.right-5,r.bottom-2};SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,RGB(0,0,0));DrawTextW(dc,button_text(s,b),-1,&label,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    }
    EndPaint(hwnd,&ps);
}
static BOOL reserve_buttons(TOOLBAR_STATE *s,int wanted)
{
    if(wanted<=s->capacity)return TRUE;int cap=s->capacity?s->capacity:8;
    while(cap<wanted){if(cap>0x3fffffff/2)return FALSE;cap*=2;}
    TBBUTTON *fresh=SvComctlAlloc((ULONG_PTR)cap*sizeof(*fresh));if(!fresh)return FALSE;
    for(int i=0;i<s->count;++i)fresh[i]=s->buttons[i];SvComctlFree(s->buttons);
    s->buttons=fresh;s->capacity=cap;return TRUE;
}
static BOOL reserve_strings(TOOLBAR_STATE *s,int wanted)
{
    if(wanted<=s->string_capacity)return TRUE;int cap=s->string_capacity?s->string_capacity:8;
    while(cap<wanted){if(cap>0x3fffffff/2)return FALSE;cap*=2;}
    WCHAR **fresh=SvComctlAlloc((ULONG_PTR)cap*sizeof(*fresh));if(!fresh)return FALSE;
    for(int i=0;i<s->string_count;++i)fresh[i]=s->strings[i];SvComctlFree(s->strings);
    s->strings=fresh;s->string_capacity=cap;return TRUE;
}
static WCHAR *copy_string(const WCHAR *source)
{
    ULONG_PTR n=0;while(source&&source[n]){if(n>=0x7ffffffeu)return NULL;++n;}
    WCHAR *copy=SvComctlAlloc((n+1)*sizeof(WCHAR));if(!copy)return NULL;
    for(ULONG_PTR i=0;i<n;++i)copy[i]=source[i];copy[n]=0;return copy;
}
static int add_strings(TOOLBAR_STATE *s,const WCHAR *source)
{
    if(!source)return -1;int count=0;const WCHAR *p=source;
    while(*p){if(count>=0x10000)return -1;++count;while(*p)++p;++p;}
    if(!count)return -1;
    if(s->string_count>0x3fffffff-count||!reserve_strings(s,s->string_count+count))return -1;
    int first=s->string_count;p=source;
    for(int i=0;i<count;++i){WCHAR *copy=copy_string(p);if(!copy){
            while(s->string_count>first)SvComctlFree(s->strings[--s->string_count]);return -1;}
        s->strings[s->string_count++]=copy;while(*p)++p;++p;}
    return first;
}
static BOOL insert_button(TOOLBAR_STATE *s,int index,const TBBUTTON *button)
{
    if(!button||index<0||index>s->count||s->count>=0x3fffffff||!reserve_buttons(s,s->count+1))return FALSE;
    for(int i=s->count;i>index;--i)s->buttons[i]=s->buttons[i-1];s->buttons[index]=*button;
    if(!(button->fsStyle&TBSTYLE_SEP)&&!button->fsState) s->buttons[index].fsState=TBSTATE_ENABLED;
    ++s->count;return TRUE;
}
static void autosize(TOOLBAR_STATE *s)
{
    HDC dc=GetDC(s->hwnd);int width=4;
    if(dc){for(int i=0;i<s->count;++i)if(!(s->buttons[i].fsState&TBSTATE_HIDDEN))width+=button_width(dc,s,i)+BUTTON_GAP;ReleaseDC(s->hwnd,dc);}
    SetWindowPos(s->hwnd,NULL,0,0,width,TOOL_HEIGHT,
                 SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
}
LRESULT SvToolbarProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    TOOLBAR_STATE *s=find_state(hwnd);
    if(msg==WM_NCCREATE){CREATESTRUCTW *cs=(CREATESTRUCTW *)lp;if(!cs)return FALSE;
        s=SvComctlAlloc(sizeof(*s));if(!s)return FALSE;s->hwnd=hwnd;s->parent=cs->hwndParent;
        s->id=(UINT)(UINT_PTR)cs->hMenu;s->buttons=NULL;s->count=s->capacity=0;
        s->strings=NULL;s->string_count=s->string_capacity=0;
        s->hover=s->pressed=-1;s->button_size=0;s->struct_size=0;
        lock_states();s->next=states;states=s;unlock_states();return TRUE;}
    if(!s)return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg){
    case WM_PAINT:draw_toolbar(hwnd,s);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_SIZE:InvalidateRect(hwnd,NULL,FALSE);return 0;
    case TB_BUTTONSTRUCTSIZE:s->struct_size=(UINT)wp;return 0;
    case TB_BUTTONCOUNT:return s->count;
    case TB_ADDSTRINGW:return add_strings(s,(const WCHAR *)lp);
    case TB_ADDBUTTONSW:{int count=(int)wp;const TBBUTTON *buttons=(const TBBUTTON *)lp;
        if(count<0||count>0x10000||(count&&!buttons))return FALSE;
        for(int i=0;i<count;++i)if(!insert_button(s,s->count,&buttons[i]))return FALSE;
        InvalidateRect(hwnd,NULL,FALSE);return TRUE;}
    case TB_INSERTBUTTONW:{int i=(int)wp;BOOL ok=insert_button(s,i,(const TBBUTTON *)lp);if(ok)InvalidateRect(hwnd,NULL,FALSE);return ok;}
    case TB_DELETEBUTTON:{int i=(int)wp;if(i<0||i>=s->count)return FALSE;
        for(int j=i;j<s->count-1;++j)s->buttons[j]=s->buttons[j+1];--s->count;InvalidateRect(hwnd,NULL,FALSE);return TRUE;}
    case TB_GETBUTTON:{int i=(int)wp;TBBUTTON *out=(TBBUTTON *)lp;if(i<0||i>=s->count||!out)return FALSE;*out=s->buttons[i];return TRUE;}
    case TB_GETSTATE:{int id=(int)wp;for(int i=0;i<s->count;++i)if(s->buttons[i].idCommand==id)return s->buttons[i].fsState;return -1;}
    case TB_SETSTATE:{int id=(int)wp;BYTE state=(BYTE)LOWORD(lp);for(int i=0;i<s->count;++i)if(s->buttons[i].idCommand==id){s->buttons[i].fsState=state;InvalidateRect(hwnd,NULL,FALSE);return TRUE;}return FALSE;}
    case TB_AUTOSIZE:autosize(s);return 0;
    case WM_MOUSEMOVE:{HDC dc=GetDC(hwnd);if(dc){int next=hit_button(dc,s,(short)LOWORD(lp),(short)HIWORD(lp));ReleaseDC(hwnd,dc);
        if(next!=s->hover){s->hover=next;InvalidateRect(hwnd,NULL,FALSE);}}return 0;}
    case WM_LBUTTONDOWN:{HDC dc=GetDC(hwnd);if(dc){int i=hit_button(dc,s,(short)LOWORD(lp),(short)HIWORD(lp));ReleaseDC(hwnd,dc);
        if(i>=0&&(s->buttons[i].fsState&TBSTATE_ENABLED)&&!(s->buttons[i].fsStyle&TBSTYLE_SEP)){s->pressed=i;SetCapture(hwnd);InvalidateRect(hwnd,NULL,FALSE);}}return 0;}
    case WM_LBUTTONUP:if(s->pressed>=0){int i=s->pressed;HDC dc=GetDC(hwnd);int hit=-1;
        if(dc){hit=hit_button(dc,s,(short)LOWORD(lp),(short)HIWORD(lp));ReleaseDC(hwnd,dc);}s->pressed=-1;ReleaseCapture();
        if(hit==i){TBBUTTON *b=&s->buttons[i];if(b->fsStyle&TBSTYLE_CHECK)b->fsState^=TBSTATE_CHECKED;
            SendMessageW(s->parent,WM_COMMAND,(WPARAM)(WORD)b->idCommand,(LPARAM)hwnd);}
        InvalidateRect(hwnd,NULL,FALSE);}return 0;
    case WM_CAPTURECHANGED:s->pressed=-1;InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_NCDESTROY:{lock_states();TOOLBAR_STATE **link=&states;while(*link&&*link!=s)link=&(*link)->next;if(*link)*link=s->next;unlock_states();
        for(int i=0;i<s->string_count;++i)SvComctlFree(s->strings[i]);
        SvComctlFree(s->strings);SvComctlFree(s->buttons);SvComctlFree(s);return 0;}
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
BOOL SvRegisterToolbar(void)
{
    WNDCLASSEXW cls;cls.cbSize=sizeof(cls);cls.style=0;cls.lpfnWndProc=SvToolbarProc;
    cls.cbClsExtra=0;cls.cbWndExtra=0;cls.hInstance=NULL;cls.hIcon=NULL;cls.hCursor=NULL;
    cls.hbrBackground=NULL;cls.lpszMenuName=NULL;cls.lpszClassName=TOOLBARCLASSNAMEW;cls.hIconSm=NULL;
    return RegisterClassExW(&cls)!=0;
}
