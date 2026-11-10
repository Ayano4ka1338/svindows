/* Actual common-control procedures; native UI calls are mocked only at the
 * boundary. Run with ASan/UBSan to exercise notification-driven destruction. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define __declspec(x)
#include "../../user/w/comctl32/Internal.h"
static int notifications,allocations,frees,allocation_fail,veto,destroy_on_notify;
static UINT last_code;static SV_NMDATETIMECHANGE last_date;
void *SvComctlAlloc(ULONG_PTR n){if(allocation_fail)return 0;void *p=calloc(1,n);if(p)allocations++;return p;}
void SvComctlFree(void *p){if(p){frees++;free(p);}}
void SvComctlFill(HDC dc,const RECT *r,COLORREF c){(void)dc;(void)r;(void)c;}
HGDIOBJ GetStockObject(int i){return (HGDIOBJ)(ULONG_PTR)i;}
HGDIOBJ SelectObject(HDC dc,HGDIOBJ obj){(void)dc;return obj;}
int SetBkMode(HDC dc,int m){(void)dc;return m;}
COLORREF SetTextColor(HDC dc,COLORREF c){(void)dc;return c;}
int SvGdiDrawTextW(HDC dc,const WCHAR *t,int n,LPRECT r,UINT f){(void)dc;(void)t;(void)n;(void)r;(void)f;return 0;}

BOOL InvalidateRect(HWND w,const RECT *r,BOOL e){(void)w;(void)r;(void)e;return TRUE;}
HDC BeginPaint(HWND w,PAINTSTRUCT *p){(void)w;(void)p;return 0;}
BOOL EndPaint(HWND w,const PAINTSTRUCT *p){(void)w;(void)p;return TRUE;}
BOOL GetClientRect(HWND w,LPRECT r){(void)w;*r=(RECT){0,0,200,24};return TRUE;}
HWND SetFocus(HWND w){return w;}
ATOM RegisterClassExW(const WNDCLASSEXW *c){(void)c;return 1;}
LRESULT DefWindowProcW(HWND w,UINT m,WPARAM p,LPARAM l){(void)w;(void)m;(void)p;(void)l;return 0;}
LRESULT SendMessageW(HWND w,UINT msg,WPARAM wp,LPARAM lp);

static LONG_PTR states[16];
LONG_PTR GetWindowLongPtrW(HWND w,int index){(void)index;return states[(ULONG_PTR)w];}
LONG_PTR SetWindowLongPtrW(HWND w,int index,LONG_PTR value){(void)index;LONG_PTR old=states[(ULONG_PTR)w];states[(ULONG_PTR)w]=value;return old;}
int CompareStringOrdinal(const WCHAR *a,int an,const WCHAR *b,int bn,BOOL ignore){(void)an;(void)bn;(void)ignore;while(*a&&*a==*b){a++;b++;}return *a<*b?1:*a>*b?3:2;}
#include "../../user/w/comctl32/TreeView.c"
static TREE_NODE *delete_parent;
LRESULT SendMessageW(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    (void)w;(void)wp;
    if(msg==WM_NOTIFY){NMHDR *hdr=(NMHDR*)lp;last_code=hdr->code;notifications++;
        if(destroy_on_notify)tree_proc(hdr->hwndFrom,WM_NCDESTROY,0,0);
        if(hdr->code==(UINT)-458&&delete_parent){TREE_NODE *p=delete_parent;delete_parent=0;tree_proc(hdr->hwndFrom,0x1101,0,(LPARAM)p);}
        return veto;
    }return 0;
}
static TREE_NODE *insert(HWND w,TREE_NODE *parent,const WCHAR *text){
    TREE_INSERT in={parent,(TREE_NODE*)(UINT_PTR)0xffff0002u,{0}};
    in.Item.Mask=1;in.Item.Text=(WCHAR*)text;
    return (TREE_NODE*)tree_proc(w,0x1132,0,(LPARAM)&in);
}
int main(void){
    HWND w=(HWND)1;CREATESTRUCTW cs={0};cs.hwndParent=(HWND)9;
    assert(tree_proc(w,WM_NCCREATE,0,(LPARAM)&cs));
    TREE_NODE *root=insert(w,0,L"Computer"),*child=insert(w,root,L"Registry");
    assert(root&&child&&tree_proc(w,0x1105,0,0)==2);
    assert(!tree_proc(w,0x110a,7,(LPARAM)root));
    veto=1;assert(!tree_proc(w,0x1102,2,(LPARAM)root));veto=0;
    assert(tree_proc(w,0x1102,2,(LPARAM)root));
    assert(tree_proc(w,0x110a,6,(LPARAM)root)==(LRESULT)child);
    assert(tree_proc(w,0x110b,9,(LPARAM)child));
    assert(tree_proc(w,0x110a,9,0)==(LRESULT)child);
    delete_parent=root;assert(tree_proc(w,0x1101,0,(LPARAM)child));
    assert(tree_proc(w,0x1105,0,0)==0);
    assert(!tree_proc(w,0x110b,9,(LPARAM)child));
    root=insert(w,0,L"Root");destroy_on_notify=1;
    assert(!tree_proc(w,0x110b,9,(LPARAM)root));
    assert(!states[1]&&allocations==frees);
    puts("PASS tree navigation, notification veto, stale handles, recursive deletion and destruction during selection");
}
