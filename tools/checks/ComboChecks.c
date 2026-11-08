#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define __declspec(x)
#define SV_USER32_BUILD
#include "../../user/w/user32/controls/Control.h"
static int allocations,frees,budget=-1,notification_count;static UINT notices[20],chosen;
void *SvControlAlloc(unsigned long long n){if(budget==0)return 0;if(budget>0)budget--;void *p=calloc(1,n);if(p)allocations++;return p;}
void SvControlFree(void *p){if(p){frees++;free(p);}}
void SvControlInvalidate(SV_CONTROL *c){(void)c;}
void SvControlNotify(SV_CONTROL *c,UINT code){(void)c;notices[notification_count++]=code;}
void SvControlFill(HDC dc,int x,int y,int w,int h,COLORREF c){(void)dc;(void)x;(void)y;(void)w;(void)h;(void)c;}
HMENU CreatePopupMenu(void){return (HMENU)1;}
BOOL AppendMenuW(HMENU m,UINT f,UINT_PTR id,const WCHAR *t){(void)m;(void)f;(void)id;(void)t;return TRUE;}
BOOL DestroyMenu(HMENU m){(void)m;return TRUE;}
BOOL TrackPopupMenu(HMENU m,UINT f,int x,int y,int r,HWND w,const RECT *rect){(void)m;(void)f;(void)x;(void)y;(void)r;(void)w;(void)rect;return chosen;}
HWND SetFocus(HWND h){return h;}
BOOL GetWindowRect(HWND w,LPRECT r){(void)w;*r=(RECT){0,0,100,24};return TRUE;}
BOOL GetClientRect(HWND w,LPRECT r){return GetWindowRect(w,r);}
LRESULT DefWindowProcW(HWND w,UINT m,WPARAM p,LPARAM l){(void)w;(void)m;(void)p;(void)l;return 0;}
HDC BeginPaint(HWND w,PAINTSTRUCT *p){(void)w;(void)p;return 0;}
BOOL EndPaint(HWND w,const PAINTSTRUCT *p){(void)w;(void)p;return TRUE;}
HDC GetDC(HWND w){return (HDC)w;}
int ReleaseDC(HWND w,HDC dc){(void)w;(void)dc;return 1;}
HGDIOBJ SelectObject(HDC dc,HGDIOBJ o){(void)dc;return o;}
BOOL GetTextMetricsW(HDC dc,LPTEXTMETRICW m){(void)dc;m->tmHeight=18;return TRUE;}
int SetBkMode(HDC dc,int m){(void)dc;return m;}
COLORREF SetTextColor(HDC dc,COLORREF c){(void)dc;return c;}
int SvGdiDrawTextW(HDC dc,const WCHAR *t,int n,LPRECT r,UINT f){(void)dc;(void)t;(void)n;(void)r;(void)f;return 0;}
static int sized_height;
BOOL SetWindowPos(HWND w,HWND after,int x,int y,int width,int height,UINT flags){(void)w;(void)after;(void)x;(void)y;(void)width;(void)flags;sized_height=height;return TRUE;}
#include "../../user/w/user32/controls/EditModel.c"
#include "../../user/w/user32/controls/ComboBox.c"
int main(void)
{
    SV_CONTROL c={0};c.kind=SV_COMBO;c.model.limit=32767;c.style=3;
    assert(SvComboMessage(&c,0x143,0,(LPARAM)L"Difference")==0);
    assert(SvComboMessage(&c,0x143,0,(LPARAM)L"Add or subtract")==1);
    assert(SvComboMessage(&c,0x146,0,0)==2&&SvComboMessage(&c,0x147,0,0)==-1);
    assert(SvComboMessage(&c,0x14e,0,0)==0&&notification_count==0&&c.model.text[0]=='D');
    assert(SvComboMessage(&c,0x151,1,0x123456789LL)==0&&SvComboMessage(&c,0x150,1,0)==0x123456789LL);
    assert(SvComboMessage(&c,0x158,(WPARAM)-1,(LPARAM)L"difference")==0);
    assert(SvComboMessage(&c,0x14c,(WPARAM)-1,(LPARAM)L"add")==1);
    WCHAR text[40]={0};assert(SvComboMessage(&c,0x148,1,(LPARAM)text)==15&&text[0]=='A');
    SvComboMessage(&c,WM_KEYDOWN,VK_DOWN,0);assert(SvComboMessage(&c,0x147,0,0)==1&&notification_count==1&&notices[0]==1);
    notification_count=0;chosen=1;SvComboMessage(&c,0x14f,1,0);
    assert(notification_count==4&&notices[0]==7&&notices[1]==1&&notices[2]==9&&notices[3]==8);
    assert(!SvComboMessage(&c,0x157,0,0));
    notification_count=0;chosen=0;SvComboMessage(&c,0x14f,1,0);assert(notification_count==3&&notices[1]==10);
    SvComboMessage(&c,WM_SIZE,0,((LPARAM)65<<16)|100);assert(sized_height==24);
    assert(SvComboMessage(&c,0x144,0,0)==1&&SvComboMessage(&c,0x147,0,0)==-1);
    SvComboMessage(&c,0x14b,0,0);assert(SvComboMessage(&c,0x146,0,0)==0);
    SvComboDispose(&c);SvEditDispose(&c.model);
    c=(SV_CONTROL){0};c.style=0x103;c.model.limit=32767;
    assert(SvComboMessage(&c,0x143,0,(LPARAM)L"Zebra")==0);
    assert(SvComboMessage(&c,0x143,0,(LPARAM)L"alpha")==0);
    assert(SvComboMessage(&c,0x158,(WPARAM)-1,(LPARAM)L"ZEBRA")==1);
    SvComboDispose(&c);SvEditDispose(&c.model);
    c=(SV_CONTROL){0};c.model.limit=32767;assert(combo_state(&c));budget=1;
    assert(SvComboMessage(&c,0x143,0,(LPARAM)L"allocation rollback")==-1);budget=-1;
    assert(SvComboMessage(&c,0x146,0,0)==0);SvComboDispose(&c);SvEditDispose(&c.model);
    assert(allocations==frees);
    puts("PASS combo strings, sorting, selection, item data, notifications, closed height and allocation rollback");return 0;
}
