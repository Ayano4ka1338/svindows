/* Actual common-control procedures; native UI calls are mocked only at the
 * boundary. Run with ASan/UBSan to exercise notification-driven destruction. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define __declspec(x)
#include "../../user/w/comctl32/Internal.h"
static int notifications,allocations,frees,allocation_fail,veto,destroy_on_notify;
static UINT last_code;static SV_NMDATETIMECHANGE last_date;
static WCHAR buddy_text[40];
void *SvComctlAlloc(ULONG_PTR n){if(allocation_fail)return 0;void *p=calloc(1,n);if(p)allocations++;return p;}
void SvComctlFree(void *p){if(p){frees++;free(p);}}
void GetLocalTime(SV_SYSTEMTIME *t){*t=(SV_SYSTEMTIME){2024,2,3,28,0,0,0,0};}
void SvComctlFill(HDC dc,const RECT *r,COLORREF c){(void)dc;(void)r;(void)c;}
HGDIOBJ GetStockObject(int i){return (HGDIOBJ)(ULONG_PTR)i;}
HGDIOBJ SelectObject(HDC dc,HGDIOBJ obj){(void)dc;return obj;}
int SetBkMode(HDC dc,int m){(void)dc;return m;}
COLORREF SetTextColor(HDC dc,COLORREF c){(void)dc;return c;}
int SvGdiDrawTextW(HDC dc,const WCHAR *t,int n,LPRECT r,UINT f){(void)dc;(void)t;(void)n;(void)r;(void)f;return 0;}
int GetDateFormatW(DWORD l,DWORD f,const SV_SYSTEMTIME *t,const WCHAR *p,WCHAR *out,int n){(void)l;(void)f;(void)t;(void)p;(void)out;(void)n;return 0;}
BOOL InvalidateRect(HWND w,const RECT *r,BOOL e){(void)w;(void)r;(void)e;return TRUE;}
HDC BeginPaint(HWND w,PAINTSTRUCT *p){(void)w;(void)p;return 0;}
BOOL EndPaint(HWND w,const PAINTSTRUCT *p){(void)w;(void)p;return TRUE;}
BOOL GetClientRect(HWND w,LPRECT r){(void)w;*r=(RECT){0,0,200,24};return TRUE;}
BOOL GetWindowRect(HWND w,LPRECT r){return GetClientRect(w,r);}
HWND SetFocus(HWND w){return w;}
HWND GetWindow(HWND w,UINT command){(void)w;(void)command;return (HWND)5;}
int MapWindowPoints(HWND f,HWND t,POINT *p,UINT n){(void)f;(void)t;(void)p;(void)n;return 0;}
BOOL SetWindowPos(HWND w,HWND a,int x,int y,int width,int height,UINT f){(void)w;(void)a;(void)x;(void)y;(void)width;(void)height;(void)f;return TRUE;}
HWND SetCapture(HWND w){return w;}
BOOL ReleaseCapture(void){return TRUE;}
BOOL ShowWindow(HWND w,int c){(void)w;(void)c;return TRUE;}
BOOL DestroyWindow(HWND w){(void)w;return TRUE;}
int GetSystemMetrics(int i){return i?768:1024;}
HWND CreateWindowExW(DWORD e,const WCHAR *c,const WCHAR *t,DWORD st,int x,int y,int w,int h,HWND p,HMENU m,HINSTANCE i,void *v){(void)e;(void)c;(void)t;(void)st;(void)x;(void)y;(void)w;(void)h;(void)p;(void)m;(void)i;(void)v;return 0;}
ATOM RegisterClassExW(const WNDCLASSEXW *c){(void)c;return 1;}
LRESULT DefWindowProcW(HWND w,UINT m,WPARAM p,LPARAM l){(void)w;(void)m;(void)p;(void)l;return 0;}
BOOL SetWindowTextW(HWND w,const WCHAR *t){(void)w;UINT n=0;while(t[n]&&n<39){buddy_text[n]=t[n];n++;}buddy_text[n]=0;return TRUE;}
int GetWindowTextW(HWND w,WCHAR *t,int count){(void)w;int n=0;while(buddy_text[n]&&n+1<count){t[n]=buddy_text[n];n++;}t[n]=0;return n;}
LRESULT SendMessageW(HWND w,UINT msg,WPARAM wp,LPARAM lp);
#include "../../user/w/comctl32/DateTime.c"
#include "../../user/w/comctl32/UpDown.c"
LRESULT SendMessageW(HWND w,UINT msg,WPARAM wp,LPARAM lp)
{
    (void)w;(void)wp;
    if(msg==WM_NOTIFY){NMHDR *hdr=(NMHDR*)lp;notifications++;last_code=hdr->code;
        if(hdr->code==DTN_DATETIMECHANGE){last_date=*(SV_NMDATETIMECHANGE*)lp;if(destroy_on_notify)SvDateTimeProc(hdr->hwndFrom,WM_NCDESTROY,0,0);}
        return veto;
    }
    return 0;
}
int main(void)
{
    HWND date=(HWND)1,optional=(HWND)2,up=(HWND)3;CREATESTRUCTW cs={0};cs.hwndParent=(HWND)9;cs.hMenu=(HMENU)77;
    assert(SvDateTimeProc(date,WM_NCCREATE,0,(LPARAM)&cs));
    SV_SYSTEMTIME value={0};assert(SvDateTimeProc(date,DTM_GETSYSTEMTIME,0,(LPARAM)&value)==GDT_VALID&&value.wDay==28);
    value=(SV_SYSTEMTIME){2024,2,0,29,0,0,0,0};
    assert(SvDateTimeProc(date,DTM_SETSYSTEMTIME,GDT_VALID,(LPARAM)&value));
    assert(notifications==1&&last_code==DTN_DATETIMECHANGE&&last_date.nmhdr.idFrom==77&&last_date.st.wDayOfWeek==4);
    assert(SvDateTimeProc(date,DTM_SETSYSTEMTIME,GDT_VALID,(LPARAM)&value)&&notifications==1);
    value.wYear=1900;assert(!SvDateTimeProc(date,DTM_SETSYSTEMTIME,0,(LPARAM)&value));
    value.wYear=2000;assert(SvDateTimeProc(date,DTM_SETSYSTEMTIME,0,(LPARAM)&value));
    value.wMonth=13;assert(!SvDateTimeProc(date,DTM_SETSYSTEMTIME,0,(LPARAM)&value));
    assert(SvDateTimeProc(date,DTM_GETSYSTEMTIME,0,0)==GDT_ERROR);
    assert(!SvDateTimeProc(date,DTM_SETSYSTEMTIME,GDT_NONE,0));
    SV_SYSTEMTIME range[2]={{2024,2,0,28,0,0,0,0},{2024,3,0,1,0,0,0,0}};
    assert(SvDateTimeProc(date,DTM_SETRANGE,3,(LPARAM)range));
    value=(SV_SYSTEMTIME){2024,3,0,2,0,0,0,0};assert(!SvDateTimeProc(date,DTM_SETSYSTEMTIME,0,(LPARAM)&value));
    value.wDay=1;assert(SvDateTimeProc(date,DTM_SETSYSTEMTIME,0,(LPARAM)&value));
    SV_SYSTEMTIME reversed[2]={range[1],range[0]};assert(!SvDateTimeProc(date,DTM_SETRANGE,3,(LPARAM)reversed));
    SV_SYSTEMTIME result[2];assert(SvDateTimeProc(date,DTM_GETRANGE,0,(LPARAM)result)==3&&date_compare(result,range)==0);
    assert(SvDateTimeProc(date,DTM_SETRANGE,0,0));
    cs.style=DTS_SHOWNONE;assert(SvDateTimeProc(optional,WM_NCCREATE,0,(LPARAM)&cs));
    assert(SvDateTimeProc(optional,DTM_SETSYSTEMTIME,GDT_NONE,0));
    value.wYear=1999;assert(SvDateTimeProc(optional,DTM_GETSYSTEMTIME,0,(LPARAM)&value)==GDT_NONE&&value.wYear==1999);
    assert(last_date.dwFlags==GDT_NONE);
    allocation_fail=1;assert(!SvDateTimeProc((HWND)4,WM_NCCREATE,0,(LPARAM)&cs));allocation_fail=0;
    destroy_on_notify=1;value=(SV_SYSTEMTIME){2026,11,0,8,0,0,0,0};assert(SvDateTimeProc(optional,DTM_SETSYSTEMTIME,0,(LPARAM)&value));assert(!date_find(optional));destroy_on_notify=0;
    assert(date_weekday(2026,11,8)==0);
    assert(SvDateTimeProc(date,WM_NCDESTROY,0,0)==0);
    cs.style=2|4|16;assert(updown_proc(up,WM_NCCREATE,0,(LPARAM)&cs));
    updown_proc(up,WM_CREATE,0,0);assert(updown_proc(up,WM_USER+106,0,0)==(LRESULT)(HWND)5);
    updown_proc(up,WM_USER+111,(WPARAM)-100,100);updown_proc(up,WM_USER+113,0,1000);assert(updown_proc(up,WM_USER+114,0,0)==100);
    updown_proc(up,WM_USER+113,0,0);updown_proc(up,WM_USER+105,(WPARAM)5,0);
    updown_move(updown_find(up),1);assert(updown_proc(up,WM_USER+114,0,0)==1&&last_code==(UINT)-722);
    veto=1;updown_move(updown_find(up),1);veto=0;assert(updown_proc(up,WM_USER+114,0,0)==1);
    updown_proc(up,WM_NCDESTROY,0,0);assert(allocations==frees);
    puts("PASS date/time messages, leap years, ranges, none state, notifications, reentrant destruction and up/down bounds");
    return 0;
}
