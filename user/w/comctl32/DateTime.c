#include "Internal.h"
__declspec(dllimport) void GetLocalTime(SV_SYSTEMTIME *);
__declspec(dllimport) int GetDateFormatW(DWORD,DWORD,const SV_SYSTEMTIME*,const WCHAR*,WCHAR*,int);
__declspec(dllimport) LONG_PTR GetWindowLongPtrW(HWND,int);
__declspec(dllimport) LONG_PTR SetWindowLongPtrW(HWND,int,LONG_PTR);
__declspec(dllimport) int MapWindowPoints(HWND,HWND,POINT*,UINT);
#define DATE_COMMIT 0x5001u
#define DATE_CANCEL 0x5002u
#define DATE_FIELD 0x5003u
_Static_assert(sizeof(SV_SYSTEMTIME)==16,"SYSTEMTIME");
_Static_assert(sizeof(SV_NMDATETIMECHANGE)==48,"NMDATETIMECHANGE");
typedef struct DATE_STATE {
    struct DATE_STATE *next; HWND hwnd,parent,calendar; UINT id; DWORD style,range; HFONT font;
    SV_SYSTEMTIME value,minimum,maximum; BOOL none; int field,digits; UINT number;
    WCHAR format[128];
} DATE_STATE;
typedef struct MONTH_STATE { struct MONTH_STATE *next; HWND hwnd,owner; HFONT font; SV_SYSTEMTIME value; int year,month; } MONTH_STATE;
static DATE_STATE *date_states;
static MONTH_STATE *month_states;
static DATE_STATE *date_find(HWND hwnd){for(DATE_STATE *s=date_states;s;s=s->next)if(s->hwnd==hwnd)return s;return 0;}
static MONTH_STATE *month_find(HWND hwnd){for(MONTH_STATE *s=month_states;s;s=s->next)if(s->hwnd==hwnd)return s;return 0;}
static int date_days(int year,int month)
{
    static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(month<1||month>12)return 0;
    return days[month-1]+(month==2&&year%4==0&&(year%100!=0||year%400==0));
}
static BOOL date_valid(const SV_SYSTEMTIME *d)
{
    return d&&d->wYear>=1601&&d->wYear<=9999&&d->wMonth>=1&&d->wMonth<=12&&
        d->wDay>=1&&d->wDay<=date_days(d->wYear,d->wMonth)&&d->wHour<24&&d->wMinute<60&&d->wSecond<60&&d->wMilliseconds<1000;
}
static int date_compare(const SV_SYSTEMTIME *a,const SV_SYSTEMTIME *b)
{
    const WORD av[]={a->wYear,a->wMonth,a->wDay,a->wHour,a->wMinute,a->wSecond,a->wMilliseconds};
    const WORD bv[]={b->wYear,b->wMonth,b->wDay,b->wHour,b->wMinute,b->wSecond,b->wMilliseconds};
    for(UINT i=0;i<7;i++)if(av[i]!=bv[i])return av[i]<bv[i]?-1:1;return 0;
}
static UINT date_weekday(int year,int month,int day)
{
    static const int offsets[]={0,3,2,5,0,3,5,1,4,6,2,4};
    if(month<3)year--;
    return (year+year/4-year/100+year/400+offsets[month-1]+day)%7;
}
static BOOL date_in_range(const DATE_STATE *s,const SV_SYSTEMTIME *d)
{
    return date_valid(d)&&(!(s->range&GDTR_MIN)||date_compare(d,&s->minimum)>=0)&&
        (!(s->range&GDTR_MAX)||date_compare(d,&s->maximum)<=0);
}
static void date_notify(DATE_STATE *s)
{
    SV_NMDATETIMECHANGE n={{s->hwnd,s->id,DTN_DATETIMECHANGE},s->none?GDT_NONE:GDT_VALID,s->value};
    HWND parent=s->parent;InvalidateRect(s->hwnd,0,FALSE);
    if(parent)SendMessageW(parent,WM_NOTIFY,n.nmhdr.idFrom,(LPARAM)&n);
}
static BOOL date_set(DATE_STATE *s,WPARAM flags,const SV_SYSTEMTIME *d,BOOL notify)
{
    BOOL none=flags==GDT_NONE;
    if(none){if(!(s->style&DTS_SHOWNONE))return FALSE;}
    else if(flags!=GDT_VALID||!date_in_range(s,d))return FALSE;
    BOOL changed=s->none!=none||(!none&&date_compare(&s->value,d)!=0);
    s->none=none;
    if(!none){s->value=*d;s->value.wDayOfWeek=date_weekday(d->wYear,d->wMonth,d->wDay);}
    InvalidateRect(s->hwnd,0,FALSE);
    if(changed&&notify)date_notify(s);
    return TRUE;
}
static void date_step(DATE_STATE *s,int direction)
{
    SV_SYSTEMTIME d=s->value;
    if(s->field==0){int m=(int)d.wMonth+direction;if(m<1){m=12;d.wYear--;}if(m>12){m=1;d.wYear++;}d.wMonth=m;}
    else if(s->field==2)d.wYear+=direction;
    else {
        int day=(int)d.wDay+direction;
        if(day<1){if(d.wMonth==1){d.wMonth=12;d.wYear--;}else d.wMonth--;day=date_days(d.wYear,d.wMonth);}
        else if(day>date_days(d.wYear,d.wMonth)){day=1;if(d.wMonth==12){d.wMonth=1;d.wYear++;}else d.wMonth++;}
        d.wDay=day;
    }
    if(d.wDay>date_days(d.wYear,d.wMonth))d.wDay=date_days(d.wYear,d.wMonth);
    date_set(s,GDT_VALID,&d,TRUE);
}
static void date_close_calendar(DATE_STATE *s)
{
    HWND calendar=s->calendar;s->calendar=0;
    if(calendar){ReleaseCapture();DestroyWindow(calendar);}
}
static LRESULT month_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    MONTH_STATE *s=month_find(hwnd);
    if(msg==WM_NCCREATE){
        s=SvComctlAlloc(sizeof(*s));if(!s)return FALSE;
        CREATESTRUCTW *cs=(CREATESTRUCTW*)lp;s->hwnd=hwnd;s->font=GetStockObject(DEFAULT_GUI_FONT);
        GetLocalTime(&s->value);s->year=s->value.wYear;s->month=s->value.wMonth;
        (void)cs;s->next=month_states;month_states=s;return TRUE;
    }
    if(!s)return DefWindowProcW(hwnd,msg,wp,lp);
    if(msg==DATE_FIELD){s->owner=(HWND)wp;return 0;}
    if(msg==0x1001){if(!lp)return FALSE;*(SV_SYSTEMTIME*)lp=s->value;return TRUE;}
    if(msg==0x1002){if(!date_valid((SV_SYSTEMTIME*)lp))return FALSE;s->value=*(SV_SYSTEMTIME*)lp;s->year=s->value.wYear;s->month=s->value.wMonth;InvalidateRect(hwnd,0,FALSE);return TRUE;}
    if(msg==WM_SETFONT){s->font=(HFONT)wp;if(lp)InvalidateRect(hwnd,0,FALSE);return 0;}
    if(msg==WM_GETFONT)return (LRESULT)s->font;
    if(msg==WM_PAINT){
        PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(!dc)return 0;
        RECT r;GetClientRect(hwnd,&r);SvComctlFill(dc,&r,RGB(255,255,255));SelectObject(dc,s->font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,0,0));
        SV_SYSTEMTIME heading=s->value;heading.wYear=s->year;heading.wMonth=s->month;heading.wDay=1;
        WCHAR title[96];int len=GetDateFormatW(0x409,0,&heading,L"MMMM yyyy",title,96);
        RECT h={24,2,r.right-24,27};SvGdiDrawTextW(dc,title,len>0?len-1:0,&h,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        RECT left={0,2,24,27},right={r.right-24,2,r.right,27};
        SvGdiDrawTextW(dc,L"<",1,&left,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SvGdiDrawTextW(dc,L">",1,&right,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        static const WCHAR *names[]={L"Su",L"Mo",L"Tu",L"We",L"Th",L"Fr",L"Sa"};
        int cell=(r.right-8)/7,offset=date_weekday(s->year,s->month,1);
        for(int i=0;i<7;i++){RECT day={4+i*cell,28,4+(i+1)*cell,48};SvGdiDrawTextW(dc,names[i],2,&day,DT_CENTER|DT_SINGLELINE);}
        for(int i=1;i<=date_days(s->year,s->month);i++){
            int index=offset+i-1;RECT day={4+(index%7)*cell,48+(index/7)*24,4+(index%7+1)*cell,72+(index/7)*24};
            if(s->value.wYear==s->year&&s->value.wMonth==s->month&&s->value.wDay==i)SvComctlFill(dc,&day,RGB(205,232,255));
            WCHAR text[3];int n=0;if(i>=10)text[n++]=(WCHAR)('0'+i/10);text[n++]=(WCHAR)('0'+i%10);
            SvGdiDrawTextW(dc,text,n,&day,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
        }
        EndPaint(hwnd,&ps);return 0;
    }
    if(msg==WM_LBUTTONDOWN){
        int x=(short)lp,y=(short)(lp>>16);RECT r;GetClientRect(hwnd,&r);
        if(x<0||y<0||x>=r.right||y>=r.bottom){if(s->owner)SendMessageW(s->owner,DATE_CANCEL,0,0);return 0;}
        if(y<28){int delta=x<24?-1:x>=r.right-24?1:0;s->month+=delta;if(s->month<1){s->month=12;s->year--;}if(s->month>12){s->month=1;s->year++;}if(s->year<1601)s->year=1601;if(s->year>9999)s->year=9999;InvalidateRect(hwnd,0,FALSE);return 0;}
        int col=(x-4)/((r.right-8)/7),row=(y-48)/24;
        if(x<4||y<48||col<0||col>6||row<0||row>5)return 0;
        int day=row*7+col-date_weekday(s->year,s->month,1)+1;
        if(day<1||day>date_days(s->year,s->month))return 0;
        SV_SYSTEMTIME value=s->value;value.wYear=s->year;value.wMonth=s->month;value.wDay=day;value.wDayOfWeek=date_weekday(s->year,s->month,day);
        HWND owner=s->owner;
        if(owner)SendMessageW(owner,DATE_COMMIT,0,(LPARAM)&value);
        else {s->value=value;InvalidateRect(hwnd,0,FALSE);}
        return 0;
    }
    if(msg==WM_KEYDOWN&&wp==VK_ESCAPE){if(s->owner)SendMessageW(s->owner,DATE_CANCEL,0,0);return 0;}
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_NCDESTROY){MONTH_STATE **link=&month_states;while(*link&&*link!=s)link=&(*link)->next;if(*link)*link=s->next;SvComctlFree(s);return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static void date_open_calendar(DATE_STATE *s)
{
    if(s->calendar){date_close_calendar(s);return;}
    RECT r;GetWindowRect(s->hwnd,&r);int x=r.left,y=r.bottom;
    int sw=GetSystemMetrics(0),sh=GetSystemMetrics(1);if(x+226>sw)x=sw-226;if(y+198>sh)y=r.top-198;
    HWND calendar=CreateWindowExW(0x80,MONTHCAL_CLASSW,L"",WS_POPUP,x,y,226,198,s->hwnd,0,0,s->hwnd);
    if(!calendar)return;
    s->calendar=calendar;SendMessageW(calendar,DATE_FIELD,(WPARAM)s->hwnd,0);SendMessageW(calendar,0x1002,0,(LPARAM)&s->value);SendMessageW(calendar,WM_SETFONT,(WPARAM)s->font,0);
    ShowWindow(calendar,SW_SHOW);SetCapture(calendar);SetFocus(calendar);
}
LRESULT SvDateTimeProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    DATE_STATE *s=date_find(hwnd);
    if(msg==WM_NCCREATE){
        s=SvComctlAlloc(sizeof(*s));if(!s)return FALSE;CREATESTRUCTW *cs=(CREATESTRUCTW*)lp;
        s->hwnd=hwnd;s->parent=cs->hwndParent;s->id=(UINT)(UINT_PTR)cs->hMenu;s->style=(DWORD)cs->style;s->font=GetStockObject(DEFAULT_GUI_FONT);
        GetLocalTime(&s->value);s->field=0;s->next=date_states;date_states=s;return TRUE;
    }
    if(!s)return DefWindowProcW(hwnd,msg,wp,lp);
    if(msg==DTM_GETSYSTEMTIME){if(!lp)return GDT_ERROR;if(s->none)return GDT_NONE;*(SV_SYSTEMTIME*)lp=s->value;return GDT_VALID;}
    if(msg==DTM_SETSYSTEMTIME)return date_set(s,wp,(const SV_SYSTEMTIME*)lp,TRUE);
    if(msg==DTM_GETRANGE){if(!lp)return 0;SV_SYSTEMTIME *d=(SV_SYSTEMTIME*)lp;d[0]=s->minimum;d[1]=s->maximum;return s->range;}
    if(msg==DTM_SETRANGE){
        if(wp&~3u)return FALSE;const SV_SYSTEMTIME *d=(const SV_SYSTEMTIME*)lp;
        if(wp&&!d)return FALSE;
        if((wp&1)&&!date_valid(d))return FALSE;if((wp&2)&&!date_valid(d+1))return FALSE;
        if(wp==3&&date_compare(d,d+1)>0)return FALSE;
        s->range=(DWORD)wp;if(wp&1)s->minimum=d[0];if(wp&2)s->maximum=d[1];
        if((wp&1)&&date_compare(&s->value,&s->minimum)<0)s->value=s->minimum;
        if((wp&2)&&date_compare(&s->value,&s->maximum)>0)s->value=s->maximum;
        InvalidateRect(hwnd,0,FALSE);return TRUE;
    }
    if(msg==DTM_SETFORMATW){UINT n=0;const WCHAR *f=(const WCHAR*)lp;if(f)while(f[n]&&n<127){s->format[n]=f[n];n++;}s->format[n]=0;InvalidateRect(hwnd,0,FALSE);return TRUE;}
    if(msg==DTM_GETMONTHCAL)return (LRESULT)s->calendar;
    if(msg==0x1009){s->font=(HFONT)wp;if(s->calendar)SendMessageW(s->calendar,WM_SETFONT,wp,lp);return 0;}
    if(msg==0x100a)return (LRESULT)s->font;
    if(msg==WM_SETFONT){s->font=(HFONT)wp;if(lp)InvalidateRect(hwnd,0,FALSE);return 0;}
    if(msg==WM_GETFONT)return (LRESULT)s->font;
    if(msg==WM_PAINT){
        PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(!dc)return 0;RECT r;GetClientRect(hwnd,&r);
        SvComctlFill(dc,&r,RGB(150,150,150));RECT inside={1,1,r.right-1,r.bottom-1};SvComctlFill(dc,&inside,RGB(255,255,255));
        SelectObject(dc,s->font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,0,0));
        WCHAR text[128]={0};if(!s->none)GetDateFormatW(0x409,0,&s->value,s->format[0]?s->format:0,text,128);
        RECT t={4,1,r.right-20,r.bottom-1};SvGdiDrawTextW(dc,text,-1,&t,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        RECT arrow={r.right-18,1,r.right-1,r.bottom-1};SvComctlFill(dc,&arrow,RGB(235,235,235));SvGdiDrawTextW(dc,L"v",1,&arrow,DT_SINGLELINE|DT_VCENTER|DT_CENTER);EndPaint(hwnd,&ps);return 0;
    }
    if(msg==WM_LBUTTONDOWN){SetFocus(hwnd);RECT r;GetClientRect(hwnd,&r);int x=(short)lp;if(x>=r.right-20)date_open_calendar(s);else {s->field=x<(r.right-20)/3?0:x<2*(r.right-20)/3?1:2;s->digits=0;}return 0;}
    if(msg==WM_KEYDOWN){
        if(wp==VK_LEFT||wp==VK_RIGHT){s->field=(s->field+(wp==VK_RIGHT?1:2))%3;s->digits=0;return 0;}
        if(wp==VK_UP||wp==VK_DOWN){date_step(s,wp==VK_UP?1:-1);return 0;}
        if(wp==0x73){date_open_calendar(s);return 0;}
    }
    if(msg==WM_CHAR&&wp>='0'&&wp<='9'){
        int maxdigits=s->field==2?4:2;if(!s->digits||s->digits>=maxdigits){s->digits=0;s->number=0;}
        s->number=s->number*10+(UINT)wp-'0';s->digits++;SV_SYSTEMTIME d=s->value;
        if(s->field==2)d.wYear=(WORD)s->number;else if(s->field==0)d.wMonth=(WORD)s->number;else d.wDay=(WORD)s->number;
        date_set(s,GDT_VALID,&d,TRUE);return 0;
    }
    if(msg==DATE_COMMIT){SV_SYSTEMTIME d=*(SV_SYSTEMTIME*)lp;date_close_calendar(s);SetFocus(hwnd);return date_set(s,GDT_VALID,&d,TRUE);}
    if(msg==DATE_CANCEL){date_close_calendar(s);SetFocus(hwnd);return 0;}
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_NCDESTROY){date_close_calendar(s);DATE_STATE **link=&date_states;while(*link&&*link!=s)link=&(*link)->next;if(*link)*link=s->next;SvComctlFree(s);return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
BOOL SvRegisterDateTime(void)
{
    WNDCLASSEXW wc={0};wc.cbSize=sizeof(wc);wc.lpfnWndProc=SvDateTimeProc;wc.lpszClassName=DATETIMEPICK_CLASSW;
    if(!RegisterClassExW(&wc))return FALSE;
    wc.lpfnWndProc=month_proc;wc.lpszClassName=MONTHCAL_CLASSW;return RegisterClassExW(&wc)!=0;
}
