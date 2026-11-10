/* Verify docked-control exclusion while the parent itself is hidden. */
#include <assert.h>
#include <stdio.h>
#define __declspec(x)
#include "../../user/w/comctl32/Internal.h"
static RECT children[4];
static DWORD styles[4];
BOOL GetClientRect(HWND window, RECT *rect)
{ (void)window; *rect = (RECT){0,0,800,600}; return TRUE; }
HWND GetDlgItem(HWND window, int id)
{ (void)window; return id > 0 && id < 4 ? (HWND)(UINT_PTR)id : 0; }
LONG_PTR GetWindowLongPtrW(HWND window, int index)
{ (void)index; return styles[(UINT_PTR)window]; }
BOOL GetWindowRect(HWND window, RECT *rect)
{ *rect = children[(UINT_PTR)window]; return TRUE; }
int MapWindowPoints(HWND from, HWND to, POINT *points, UINT count)
{
    (void)from; (void)to;
    for (UINT i=0; i<count; i++) { points[i].x -= 100; points[i].y -= 200; }
    return 0;
}
#include "../../user/w/comctl32/ClientRect.inc"
int main(void)
{
    RECT result;
    int controls[] = {1,100,1,1,1,2,1,3,0};
    children[1] = (RECT){100,780,900,800}; styles[1] = WS_VISIBLE;
    children[2] = (RECT){100,200,900,230}; styles[2] = WS_VISIBLE;
    children[3] = (RECT){100,230,300,780}; styles[3] = 0;
    GetEffectiveClientRect((HWND)9, &result, controls);
    assert(result.left==0 && result.top==30 && result.right==800 && result.bottom==580);
    styles[3] = WS_VISIBLE;
    GetEffectiveClientRect((HWND)9, &result, controls);
    assert(result.left==200 && result.top==30 && result.right==800 && result.bottom==580);
    GetEffectiveClientRect((HWND)9, &result, 0);
    assert(result.left==0 && result.top==0 && result.right==800 && result.bottom==600);
    puts("PASS client area excludes visible status, toolbar and side panel in client coordinates");
}
