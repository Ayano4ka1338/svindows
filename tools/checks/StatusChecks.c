/* Exercise the actual status procedure against native geometry boundaries. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define __declspec(x)
#include "../../user/w/comctl32/Internal.h"
static RECT parent={0,0,528,450}, child;
static DWORD style;static int moves,fail_alloc,allocs,frees;
void *SvComctlAlloc(ULONG_PTR n){if(fail_alloc)return NULL;void *p=calloc(1,n);if(p)allocs++;return p;}
void SvComctlFree(void *p){if(p){frees++;free(p);}}
void SvComctlFill(HDC d,const RECT *r,COLORREF c){(void)d;(void)r;(void)c;}
LONG_PTR GetWindowLongPtrW(HWND w,int i){(void)w;assert(i==-16);return style;}
int MapWindowPoints(HWND a,HWND b,POINT *p,UINT n){(void)a;(void)b;(void)p;(void)n;return 0;}
BOOL GetClientRect(HWND w,RECT *r){*r=w==(HWND)9?parent:(RECT){0,0,child.right-child.left,child.bottom-child.top};return TRUE;}
BOOL GetWindowRect(HWND w,RECT *r){(void)w;*r=child;return TRUE;}
HDC GetDC(HWND w){(void)w;return (HDC)1;}
int ReleaseDC(HWND w,HDC d){(void)w;(void)d;return 1;}
HGDIOBJ GetStockObject(int i){return (HGDIOBJ)(ULONG_PTR)i;}
HGDIOBJ SelectObject(HDC d,HGDIOBJ o){(void)d;return o;}
BOOL GetTextMetricsW(HDC d,TEXTMETRICW *m){(void)d;*m=(TEXTMETRICW){0};m->tmHeight=16;return TRUE;}
BOOL InvalidateRect(HWND w,const RECT *r,BOOL e){(void)w;(void)r;(void)e;return TRUE;}
HDC BeginPaint(HWND w,PAINTSTRUCT *p){(void)w;(void)p;return NULL;}
BOOL EndPaint(HWND w,const PAINTSTRUCT *p){(void)w;(void)p;return TRUE;}
int SetBkMode(HDC d,int m){(void)d;return m;}
COLORREF SetTextColor(HDC d,COLORREF c){(void)d;return c;}
int SvGdiDrawTextW(HDC d,const WCHAR *t,int n,RECT *r,UINT f){(void)d;(void)t;(void)n;(void)r;(void)f;return 0;}
ATOM RegisterClassExW(const WNDCLASSEXW *c){(void)c;return 1;}
LRESULT DefWindowProcW(HWND w,UINT m,WPARAM p,LPARAM l){(void)w;(void)m;(void)p;(void)l;return 0;}
#include "../../user/w/comctl32/Status.c"
BOOL SetWindowPos(HWND w,HWND a,int x,int y,int width,int height,UINT flags)
{
    (void)a;assert(flags&(SWP_NOACTIVATE|SWP_NOZORDER));
    child=(RECT){x,y,x+width,y+height};moves++;
    /* Native SetWindowPos dispatches WM_SIZE synchronously. */
    SvStatusProc(w,WM_SIZE,0,0);return TRUE;
}
static int equal(const WCHAR *a,const WCHAR *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
int main(void)
{
    HWND w=(HWND)1;CREATESTRUCTW cs={0};cs.hwndParent=(HWND)9;
    assert(SvStatusProc(w,WM_NCCREATE,0,(LPARAM)&cs));
    SvStatusProc(w,WM_CREATE,0,0);
    assert(moves==1&&child.top==428&&child.bottom==450&&child.right==528);
    parent.right=800;parent.bottom=600;SvStatusProc(w,WM_SIZE,0,0);
    assert(moves==2&&child.top==578&&child.right==800);
    SvStatusProc(w,WM_USER+8,30,0);SvStatusProc(w,WM_SIZE,0,0);
    assert(child.top==564&&child.bottom-child.top==36);
    style=1;SvStatusProc(w,WM_SIZE,0,0);assert(child.top==0);
    style=4;int old=moves;SvStatusProc(w,WM_SIZE,0,0);assert(old==moves);
    style=0;
    int edges[]={180,400,-1};assert(SvStatusProc(w,SB_SETPARTS,3,(LPARAM)edges));
    assert(SvStatusProc(w,SB_SETTEXTW,0,(LPARAM)L"Processes: 6"));
    assert(SvStatusProc(w,SB_SETTEXTW,1,(LPARAM)L"CPU: 17%"));
    assert(SvStatusProc(w,SB_SETTEXTW,2,(LPARAM)L"Memory: 25%"));
    assert(!SvStatusProc(w,SB_SETTEXTW,3,(LPARAM)L"bad"));
    WCHAR text[40];SvStatusProc(w,WM_USER+13,1,(LPARAM)text);assert(equal(text,L"CPU: 17%"));
    assert(SvStatusProc(w,WM_USER+12,1,0)==8);
    RECT r;assert(SvStatusProc(w,WM_USER+10,2,(LPARAM)&r)&&r.left==400&&r.right==800);
    int got[4]={0,0,0,123};assert(SvStatusProc(w,WM_USER+6,3,(LPARAM)got)==3&&got[2]==-1&&got[3]==123);
    assert(SvStatusProc(w,SB_SETTEXTW,255,(LPARAM)L"Simple"));
    SvStatusProc(w,SB_SIMPLE,1,0);SvStatusProc(w,WM_USER+13,255,(LPARAM)text);assert(equal(text,L"Simple"));
    SvStatusProc(w,SB_SIMPLE,0,0);
    fail_alloc=1;assert(!SvStatusProc(w,SB_SETPARTS,1,(LPARAM)edges));fail_alloc=0;
    SvStatusProc(w,WM_USER+13,2,(LPARAM)text);assert(equal(text,L"Memory: 25%"));
    assert(SvStatusProc(w,SB_SETPARTS,1,(LPARAM)edges));
    SvStatusProc(w,WM_USER+13,0,(LPARAM)text);assert(equal(text,L"Processes: 6"));
    assert(!SvStatusProc(w,SB_SETPARTS,257,(LPARAM)edges));
    SvStatusProc(w,WM_NCDESTROY,0,0);assert(!states&&allocs==frees);
    puts("PASS status initial/parent resize, reentrant WM_SIZE, minimum height, parts, text ownership and allocation rollback");
}
