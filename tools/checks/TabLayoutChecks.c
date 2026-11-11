#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define __declspec(x)
#include "../../user/w/comctl32/Internal.h"
static DWORD style;
LONG_PTR GetWindowLongPtrW(HWND h,int i){(void)h;assert(i==-16);return style;}
void *SvComctlAlloc(ULONG_PTR n){return calloc(1,n);}
void SvComctlFree(void *p){free(p);}
void SvComctlFill(HDC d,const RECT *r,COLORREF c){(void)d;(void)r;(void)c;}
BOOL GetClientRect(HWND h,RECT *r){(void)h;*r=(RECT){0,0,514,400};return TRUE;}
HDC GetDC(HWND h){(void)h;return (HDC)1;}
int ReleaseDC(HWND h,HDC d){(void)h;(void)d;return 1;}
BOOL GetTextExtentPoint32W(HDC d,const WCHAR *t,int n,SIZE *s){(void)d;(void)t;s->cx=n*8;s->cy=16;return TRUE;}
BOOL InvalidateRect(HWND h,const RECT *r,BOOL e){(void)h;(void)r;(void)e;return TRUE;}
HDC BeginPaint(HWND h,PAINTSTRUCT *p){(void)h;(void)p;return 0;}
BOOL EndPaint(HWND h,const PAINTSTRUCT *p){(void)h;(void)p;return TRUE;}
HPEN CreatePen(int a,int b,COLORREF c){(void)a;(void)b;(void)c;return 0;}
HBRUSH CreateSolidBrush(COLORREF c){(void)c;return 0;}
HGDIOBJ SelectObject(HDC d,HGDIOBJ o){(void)d;return o;}
BOOL DeleteObject(HGDIOBJ o){(void)o;return TRUE;}
BOOL MoveToEx(HDC d,int x,int y,POINT *p){(void)d;(void)x;(void)y;(void)p;return TRUE;}
BOOL LineTo(HDC d,int x,int y){(void)d;(void)x;(void)y;return TRUE;}
BOOL Rectangle(HDC d,int a,int b,int c,int e){(void)d;(void)a;(void)b;(void)c;(void)e;return TRUE;}
BOOL PatBlt(HDC d,int a,int b,int c,int e,DWORD f){(void)d;(void)a;(void)b;(void)c;(void)e;(void)f;return TRUE;}
int SetBkMode(HDC d,int m){(void)d;return m;}
COLORREF SetTextColor(HDC d,COLORREF c){(void)d;return c;}
int SvGdiDrawTextW(HDC d,const WCHAR *t,int n,RECT *r,UINT f){(void)d;(void)t;(void)n;(void)r;(void)f;return 0;}
HWND SetFocus(HWND h){return h;}
ATOM RegisterClassExW(const WNDCLASSEXW *c){(void)c;return 1;}
LRESULT SendMessageW(HWND h,UINT m,WPARAM w,LPARAM l){(void)h;(void)m;(void)w;(void)l;return 0;}
LRESULT DefWindowProcW(HWND h,UINT m,WPARAM w,LPARAM l){(void)h;(void)m;(void)w;(void)l;return 0;}
#include "../../user/w/comctl32/Tab.c"
int main(void)
{
    HWND h=(HWND)1;CREATESTRUCTW cs={0};assert(SvTabProc(h,WM_NCCREATE,0,(LPARAM)&cs));
    TCITEMW item={0};item.mask=TCIF_TEXT;item.pszText=L"Applications";
    assert(SvTabProc(h,TCM_INSERTITEMW,0,(LPARAM)&item)==0);
    RECT r={10,20,524,420};SvTabProc(h,TCM_ADJUSTRECT,FALSE,(LPARAM)&r);
    assert(r.left==14&&r.top==48&&r.right==520&&r.bottom==416);
    RECT header=tab_rect((HDC)1,find_state(h),0,NULL);
    assert(r.top-20>header.bottom&&r.bottom>r.top);
    SvTabProc(h,TCM_ADJUSTRECT,TRUE,(LPARAM)&r);
    assert(r.left==10&&r.top==20&&r.right==524&&r.bottom==420);
    style=2;SvTabProc(h,TCM_ADJUSTRECT,FALSE,(LPARAM)&r);
    assert(r.top==24&&r.bottom==392);
    SvTabProc(h,TCM_ADJUSTRECT,TRUE,(LPARAM)&r);assert(r.top==20&&r.bottom==420);
    SvTabProc(h,TCM_ADJUSTRECT,FALSE,0);SvTabProc(h,WM_NCDESTROY,0,0);assert(!states);
    puts("PASS tab display/page rectangle conversion, header exclusion and inverse conversion");
}
