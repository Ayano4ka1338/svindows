/* Exercise the actual common-control procedure against independent window
 * and scrollbar fixtures, including destruction from a selection callback. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/w/comctl32/ListView.c"

typedef struct { RECT rect;LV_SCROLL_INFO scroll;BOOL visible,alive;HWND parent;WNDPROC procedure;WCHAR text[256]; } WINDOW_FIXTURE;
static WINDOW_FIXTURE root,owner,bars[8];
static int bar_count;
static unsigned allocations,frees;
static BOOL destroy_on_change;
static BOOL control_down,shift_down,veto_selection;
static UINT notification_count;
static HWND focus;
static BOOL accept_label;
static LRESULT editor_fixture_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{(void)hwnd;(void)message;(void)wp;(void)lp;return 0;}
static WINDOW_FIXTURE *fixture(HWND hwnd){return (WINDOW_FIXTURE *)hwnd;}
void *SvComctlAlloc(ULONG_PTR size){++allocations;return calloc(1,(size_t)size);}
void SvComctlFree(void *memory){if(memory){++frees;free(memory);}}
BOOL GetClientRect(HWND hwnd,RECT *out){WINDOW_FIXTURE *w=fixture(hwnd);if(!w||!w->alive)return FALSE;*out=(RECT){0,0,w->rect.right-w->rect.left,w->rect.bottom-w->rect.top};return TRUE;}
HWND CreateWindowExW(DWORD ex,const WCHAR *cls,const WCHAR *title,DWORD style,int x,int y,int width,int height,HWND parent,HMENU menu,HINSTANCE instance,void *parameter)
{(void)ex;(void)cls;(void)style;(void)menu;(void)instance;(void)parameter;assert(bar_count<8);WINDOW_FIXTURE *w=&bars[bar_count++];w->alive=TRUE;w->rect=(RECT){x,y,x+width,y+height};w->parent=parent;w->procedure=editor_fixture_proc;unsigned n=0;while(title&&title[n]&&n<255){w->text[n]=title[n];n++;}w->text[n]=0;return w;}
BOOL MoveWindow(HWND hwnd,int x,int y,int width,int height,BOOL paint){(void)paint;WINDOW_FIXTURE *w=fixture(hwnd);assert(w&&w->alive);w->rect=(RECT){x,y,x+width,y+height};return TRUE;}
BOOL ShowWindow(HWND hwnd,int command){WINDOW_FIXTURE *w=fixture(hwnd);assert(w&&w->alive);w->visible=command!=SW_HIDE;return TRUE;}
int SetScrollInfo(HWND hwnd,int bar,const LV_SCROLL_INFO *info,BOOL redraw){(void)bar;(void)redraw;WINDOW_FIXTURE *w=fixture(hwnd);assert(w&&w->alive);if(info->mask&1){w->scroll.minimum=info->minimum;w->scroll.maximum=info->maximum;}if(info->mask&2)w->scroll.page=info->page;if(info->mask&4)w->scroll.position=info->position;return w->scroll.position;}
BOOL GetScrollInfo(HWND hwnd,int bar,LV_SCROLL_INFO *info){(void)bar;WINDOW_FIXTURE *w=fixture(hwnd);if(!w||!w->alive)return FALSE;if(info->mask&16)info->track=w->scroll.track;return TRUE;}
BOOL InvalidateRect(HWND hwnd,const RECT *rect,BOOL erase){(void)rect;(void)erase;return fixture(hwnd)&&fixture(hwnd)->alive;}
HWND SetFocus(HWND hwnd){HWND previous=focus;focus=hwnd;return previous;}
short GetKeyState(int key){return (key==VK_CONTROL&&control_down)||(key==VK_SHIFT&&shift_down)?(short)0x8000:0;}
BOOL IsWindow(HWND hwnd){return fixture(hwnd)&&fixture(hwnd)->alive;}
HWND GetParent(HWND hwnd){return fixture(hwnd)->parent;}
int GetWindowTextLengthW(HWND hwnd){unsigned n=0;while(fixture(hwnd)->text[n])n++;return (int)n;}
int GetWindowTextW(HWND hwnd,WCHAR *out,int count){int n=0;while(n+1<count&&fixture(hwnd)->text[n]){out[n]=fixture(hwnd)->text[n];n++;}out[n]=0;return n;}
LONG_PTR SetWindowLongPtrW(HWND hwnd,int index,LONG_PTR value){assert(index==-4);WNDPROC old=fixture(hwnd)->procedure;fixture(hwnd)->procedure=(WNDPROC)value;return (LONG_PTR)old;}
LRESULT CallWindowProcW(WNDPROC procedure,HWND hwnd,UINT message,WPARAM wp,LPARAM lp){return procedure(hwnd,message,wp,lp);}
BOOL DestroyWindow(HWND hwnd){WINDOW_FIXTURE *w=fixture(hwnd);if(!w||!w->alive)return FALSE;if(w->procedure)w->procedure(hwnd,WM_NCDESTROY,0,0);w->alive=FALSE;return TRUE;}
LRESULT DefWindowProcW(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){(void)hwnd;(void)message;(void)wp;(void)lp;return 0;}
LRESULT SendMessageW(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    (void)wp;
    if(hwnd==&owner&&message==WM_NOTIFY){++notification_count;NMHDR *note=(NMHDR *)lp;
        if(veto_selection&&note->code==LVN_ITEMCHANGING&&((NMLISTVIEW *)note)->iItem==17&&(((NMLISTVIEW *)note)->uNewState&LVIS_SELECTED))return TRUE;
        if(note->code==LVN_ENDLABELEDITW)return accept_label;
        if(destroy_on_change&&note->code==LVN_ITEMCHANGED){destroy_on_change=FALSE;SvListViewProc(&root,WM_NCDESTROY,0,0);root.alive=FALSE;}return 0;}
    return 0;
}
ATOM RegisterClassExW(const WNDCLASSEXW *cls){assert(cls->style&8);return 1;}
void SvComctlFill(HDC dc,const RECT *rect,COLORREF color){(void)dc;(void)rect;(void)color;}
HDC BeginPaint(HWND hwnd,PAINTSTRUCT *paint){(void)hwnd;(void)paint;return NULL;}
BOOL EndPaint(HWND hwnd,const PAINTSTRUCT *paint){(void)hwnd;(void)paint;return TRUE;}
int SetBkMode(HDC dc,int mode){(void)dc;return mode;}
COLORREF SetTextColor(HDC dc,COLORREF color){(void)dc;return color;}
int DrawTextW(HDC dc,const WCHAR *text,int count,LPRECT rect,UINT flags){(void)dc;(void)text;(void)count;(void)rect;(void)flags;return 0;}
HPEN CreatePen(int style,int width,COLORREF color){(void)style;(void)width;(void)color;return NULL;}
HGDIOBJ SelectObject(HDC dc,HGDIOBJ object){(void)dc;return object;}
BOOL DeleteObject(HGDIOBJ object){(void)object;return TRUE;}
BOOL MoveToEx(HDC dc,int x,int y,LPPOINT previous){(void)dc;(void)x;(void)y;(void)previous;return TRUE;}
BOOL LineTo(HDC dc,int x,int y){(void)dc;(void)x;(void)y;return TRUE;}

int main(void)
{
    root=(WINDOW_FIXTURE){.rect={0,0,360,146},.alive=TRUE};owner.alive=TRUE;
    CREATESTRUCTW create={.hwndParent=&owner,.style=WS_CHILD|LVS_REPORT|LVS_EDITLABELS,.hMenu=(HMENU)7};
    assert(SvListViewProc(&root,WM_NCCREATE,0,(LPARAM)&create));
    assert(SvListViewProc(&root,WM_CREATE,0,(LPARAM)&create)==0);
    LVCOLUMNW column={.mask=LVCF_TEXT|LVCF_WIDTH,.pszText=L"Name",.cx=400};
    assert(SvListViewProc(&root,LVM_INSERTCOLUMNW,0,(LPARAM)&column)==0);
    for(int i=0;i<130;++i){LVITEMW item={.mask=LVIF_TEXT|LVIF_PARAM,.iItem=i,.pszText=L"Entry",.lParam=i};assert(SvListViewProc(&root,LVM_INSERTITEMW,0,(LPARAM)&item)==i);}
    assert(bars[0].visible&&bars[1].visible);
    assert(SvListViewProc(&root,LVM_GETCOUNTPERPAGE,0,0)==4);
    assert(SvListViewProc(&root,LVM_SCROLL,20,6*24));
    assert(SvListViewProc(&root,LVM_GETTOPINDEX,0,0)==6);
    RECT cell={.left=LVIR_BOUNDS,.top=0};
    assert(SvListViewProc(&root,LVM_GETSUBITEMRECT,6,(LPARAM)&cell));
    assert(cell.left==-20&&cell.top==26);
    assert(SvListViewProc(&root,WM_LBUTTONDOWN,0,((LPARAM)30<<16)|40)==0);
    assert(SvListViewProc(&root,LVM_GETNEXTITEM,(WPARAM)-1,LVNI_SELECTED)==6);
    SvListViewProc(&root,0x20a,((WPARAM)(WORD)60<<16),0);
    assert(SvListViewProc(&root,LVM_GETTOPINDEX,0,0)==6);
    SvListViewProc(&root,0x20a,((WPARAM)(WORD)60<<16),0);
    assert(SvListViewProc(&root,LVM_GETTOPINDEX,0,0)==3);
    SvListViewProc(&root,WM_VSCROLL,7,0);
    assert(SvListViewProc(&root,LVM_GETTOPINDEX,0,0)==126&&bars[0].scroll.position==126);
    root.rect.bottom=350;SvListViewProc(&root,WM_SIZE,0,0);
    assert(SvListViewProc(&root,LVM_GETTOPINDEX,0,0)==118);
    SvListViewProc(&root,LVM_SCROLL,(WPARAM)0x7fffffff,(LPARAM)0x7fffffff);
    assert(SvListViewProc(&root,LVM_GETTOPINDEX,0,0)==118);
    SvListViewProc(&root,LVM_SCROLL,(WPARAM)0x80000000u,(LPARAM)0x80000000u);
    assert(SvListViewProc(&root,LVM_GETTOPINDEX,0,0)==0);
    SvListViewProc(&root,WM_LBUTTONDOWN,0,((LPARAM)(26+6*24+2)<<16)|40);
    SvListViewProc(&root,WM_LBUTTONDOWN,8,((LPARAM)(26+8*24+2)<<16)|40);
    assert(SvListViewProc(&root,LVM_GETSELECTEDCOUNT,0,0)==2);
    assert(SvListViewProc(&root,LVM_GETNEXTITEM,6,LVNI_SELECTED)==8);
    SvListViewProc(&root,WM_LBUTTONDOWN,4,((LPARAM)(26+12*24+2)<<16)|40);
    assert(SvListViewProc(&root,LVM_GETSELECTEDCOUNT,0,0)==5);
    LVITEMW selected={.state=LVIS_SELECTED,.stateMask=LVIS_SELECTED};
    assert(SvListViewProc(&root,LVM_SETITEMSTATE,14,(LPARAM)&selected));
    assert(SvListViewProc(&root,LVM_GETSELECTEDCOUNT,0,0)==6);
    control_down=TRUE;SvListViewProc(&root,WM_KEYDOWN,'A',0);
    assert(SvListViewProc(&root,LVM_GETSELECTEDCOUNT,0,0)==130);
    SvListViewProc(&root,WM_KEYDOWN,VK_DOWN,0);
    assert(SvListViewProc(&root,LVM_GETSELECTEDCOUNT,0,0)==130);
    assert(SvListViewProc(&root,LVM_GETNEXTITEM,(WPARAM)-1,1)==13);
    control_down=FALSE;selected.state=0;
    assert(SvListViewProc(&root,LVM_SETITEMSTATE,(WPARAM)-1,(LPARAM)&selected));
    veto_selection=TRUE;selected.state=LVIS_SELECTED;
    assert(!SvListViewProc(&root,LVM_SETITEMSTATE,17,(LPARAM)&selected));
    assert(!SvListViewProc(&root,LVM_GETSELECTEDCOUNT,0,0));veto_selection=FALSE;
    HWND editor=(HWND)SvListViewProc(&root,LVM_EDITLABELW,17,0);assert(editor);
    assert(SvListViewProc(&root,LVM_GETEDITCONTROL,0,0)==(LRESULT)editor);
    WCHAR updated[]={'R','e','n','a','m','e','d',0};memcpy(fixture(editor)->text,updated,sizeof(updated));
    accept_label=TRUE;fixture(editor)->procedure(editor,WM_KEYDOWN,VK_RETURN,0);
    WCHAR text[32];LVITEMW item={.iSubItem=0,.pszText=text,.cchTextMax=32};
    assert(SvListViewProc(&root,LVM_GETITEMTEXTW,17,(LPARAM)&item)==7 && text[0]=='R');
    assert(!SvListViewProc(&root,LVM_GETEDITCONTROL,0,0));
    editor=(HWND)SvListViewProc(&root,LVM_EDITLABELW,17,0);assert(editor);
    fixture(editor)->text[0]='X';fixture(editor)->text[1]=0;
    fixture(editor)->procedure(editor,WM_KEYDOWN,VK_ESCAPE,0);
    assert(SvListViewProc(&root,LVM_GETITEMTEXTW,17,(LPARAM)&item)==7 && text[0]=='R');
    destroy_on_change=TRUE;SvListViewProc(&root,WM_KEYDOWN,VK_DOWN,0);
    assert(!root.alive&&notification_count>0&&states==NULL);
    assert(allocations==frees);
    assert(SvRegisterListView());
    puts("ListView scroll, viewport, hit-test and reentrant lifetime checks passed");
    return 0;
}
