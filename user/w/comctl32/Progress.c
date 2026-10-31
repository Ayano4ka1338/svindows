#include "Internal.h"

typedef struct PROGRESS_STATE {
    struct PROGRESS_STATE *next;
    HWND hwnd;
    int low, high, position, step;
} PROGRESS_STATE;
static PROGRESS_STATE *states;
static unsigned state_lock;
static void lock_states(void) { while (__atomic_exchange_n(&state_lock,1,__ATOMIC_ACQUIRE)) {} }
static void unlock_states(void) { __atomic_store_n(&state_lock,0,__ATOMIC_RELEASE); }

static PROGRESS_STATE *find_state(HWND hwnd)
{
    lock_states(); PROGRESS_STATE *found=NULL;
    for (PROGRESS_STATE *s=states;s;s=s->next) if(s->hwnd==hwnd){found=s;break;}
    unlock_states(); return found;
}

static void draw_progress(HWND hwnd, PROGRESS_STATE *s)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    if (!dc) return;
    RECT r; GetClientRect(hwnd, &r);
    SvComctlFill(dc, &r, RGB(240,240,240));
    if (r.right-r.left > 2 && r.bottom-r.top > 2) {
        RECT edge = r; SvComctlFill(dc, &(RECT){edge.left,edge.top,edge.right,edge.top+1}, RGB(191,191,191));
        SvComctlFill(dc, &(RECT){edge.left,edge.bottom-1,edge.right,edge.bottom}, RGB(191,191,191));
        SvComctlFill(dc, &(RECT){edge.left,edge.top,edge.left+1,edge.bottom}, RGB(191,191,191));
        SvComctlFill(dc, &(RECT){edge.right-1,edge.top,edge.right,edge.bottom}, RGB(191,191,191));
        long long span = (long long)s->high - s->low;
        int value = s->position < s->low ? s->low : s->position > s->high ? s->high : s->position;
        int width = span ? (int)((((long long)value-s->low) * (r.right-r.left-2))/span) : 0;
        if (width > 0) {
            RECT fill = {r.left+1,r.top+1,r.left+1+width,r.bottom-1};
            SvComctlFill(dc, &fill, RGB(6,176,37));
        }
    }
    EndPaint(hwnd, &ps);
}

LRESULT SvProgressProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PROGRESS_STATE *s = find_state(hwnd);
    if (msg == WM_NCCREATE) {
        s = SvComctlAlloc(sizeof(*s));
        if (!s) return FALSE;
        s->hwnd=hwnd; s->low=0; s->high=100; s->position=0; s->step=10;
        lock_states(); s->next=states; states=s; unlock_states();
        return TRUE;
    }
    if (!s) return DefWindowProcW(hwnd,msg,wp,lp);
    switch (msg) {
    case WM_PAINT: draw_progress(hwnd,s); return 0;
    case WM_ERASEBKGND: return 1;
    case PBM_SETRANGE32:
        s->low=(int)wp; s->high=(int)lp;
        if (s->high < s->low) s->high=s->low;
        if (s->position < s->low) s->position=s->low;
        if (s->position > s->high) s->position=s->high;
        InvalidateRect(hwnd,NULL,FALSE); return 1;
    case PBM_SETPOS: { int old=s->position; s->position=(int)wp;
        if (s->position<s->low) s->position=s->low;
        if (s->position>s->high) s->position=s->high;
        InvalidateRect(hwnd,NULL,FALSE); return old; }
    case PBM_GETPOS: return s->position;
    case PBM_SETSTEP: { int old=s->step; s->step=(int)wp; return old; }
    case PBM_STEPIT: { int old=s->position; long long p=(long long)s->position+s->step;
        s->position=p<s->low?s->low:p>s->high?s->high:(int)p;
        InvalidateRect(hwnd,NULL,FALSE); return old; }
    case WM_NCDESTROY: {
        lock_states(); PROGRESS_STATE **link=&states; while (*link && *link!=s) link=&(*link)->next;
        if (*link) *link=s->next; unlock_states();
        SvComctlFree(s); return 0;
    }
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

BOOL SvRegisterProgress(void)
{
    WNDCLASSEXW cls; cls.cbSize=sizeof(cls); cls.style=0;
    cls.lpfnWndProc=SvProgressProc; cls.cbClsExtra=0; cls.cbWndExtra=0;
    cls.hInstance=NULL; cls.hIcon=NULL; cls.hCursor=NULL; cls.hbrBackground=NULL;
    cls.lpszMenuName=NULL; cls.lpszClassName=PROGRESS_CLASSW; cls.hIconSm=NULL;
    return RegisterClassExW(&cls)!=0;
}
