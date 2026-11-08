#include "Internal.h"

__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(HANDLE, DWORD, ULONG_PTR);
__declspec(dllimport) BOOL HeapFree(HANDLE, DWORD, void *);

void *SvComctlAlloc(ULONG_PTR bytes)
{ return HeapAlloc(GetProcessHeap(), 8, bytes ? bytes : 1); }
void SvComctlFree(void *memory)
{ if (memory) HeapFree(GetProcessHeap(), 0, memory); }

void SvComctlFill(HDC dc, const RECT *rect, COLORREF color)
{
    if (!dc || !rect || rect->right <= rect->left || rect->bottom <= rect->top) return;
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush) return;
    HGDIOBJ old = SelectObject(dc, brush);
    PatBlt(dc, rect->left, rect->top, rect->right - rect->left,
           rect->bottom - rect->top, PATCOPY);
    SelectObject(dc, old);
    DeleteObject(brush);
}

static BOOL date_registered,updown_registered;
static BOOL progress_registered, status_registered, trackbar_registered, tab_registered, listview_registered, toolbar_registered;
COMCTL32_API BOOL InitCommonControlsEx(const INITCOMMONCONTROLSEX *controls)
{
    if (!controls || controls->dwSize != sizeof(*controls)) return FALSE;
    DWORD flags = controls->dwICC;
    if((flags&ICC_DATE_CLASSES)&&!date_registered){if(!SvRegisterDateTime())return FALSE;date_registered=TRUE;}
    if((flags&ICC_UPDOWN_CLASS)&&!updown_registered){if(!SvRegisterUpDown())return FALSE;updown_registered=TRUE;}
    if ((flags & ICC_PROGRESS_CLASS) && !progress_registered) {
        if (!SvRegisterProgress()) return FALSE;
        progress_registered = TRUE;
    }
    if ((flags & ICC_BAR_CLASSES) && !status_registered) {
        if (!SvRegisterStatus()) return FALSE;
        status_registered = TRUE;
    }
    if ((flags & ICC_BAR_CLASSES) && !trackbar_registered) {
        if (!SvRegisterTrackbar()) return FALSE;
        trackbar_registered = TRUE;
    }
    if ((flags & ICC_TAB_CLASSES) && !tab_registered) {
        if (!SvRegisterTab()) return FALSE;
        tab_registered = TRUE;
    }
    if ((flags & ICC_LISTVIEW_CLASSES) && !listview_registered) {
        if (!SvRegisterListView()) return FALSE;
        listview_registered = TRUE;
    }
    if ((flags & ICC_COOL_CLASSES) && !toolbar_registered) {
        if (!SvRegisterToolbar()) return FALSE;
        toolbar_registered = TRUE;
    }
    return TRUE;
}

COMCTL32_API void InitCommonControls(void)
{
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_COOL_CLASSES };
    (void)InitCommonControlsEx(&controls);
}

COMCTL32_API HWND CreateStatusWindowW(LONG style, const WCHAR *text,
                                      HWND parent, UINT id)
{
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_BAR_CLASSES };
    if (!parent || !InitCommonControlsEx(&controls)) return NULL;
    return CreateWindowExW(0, STATUSCLASSNAMEW, text ? text : L"",
                           WS_CHILD | WS_VISIBLE | (DWORD)style,
                           0, 0, 0, 0, parent, (HMENU)(UINT_PTR)id, NULL, NULL);
}

#include <SvImageList.h>
static SV_IMAGELIST g_image_lists[256];
__declspec(dllimport) HANDLE GetModuleHandleA(const char*);
__declspec(dllimport) void *GetProcAddress(HANDLE,const char*);
static void sv_image_trace(const char *message)
{
    static UINT count;
    if(count++>=12)return;
    long (*print)(const char*)=(long(*)(const char*))GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtDisplayString");
    if(print)print(message);
}

static void sv_image_list_clear(SV_IMAGELIST *list)
{ volatile unsigned char *p=(volatile unsigned char*)list; for(UINT i=0;i<sizeof(*list);i++)p[i]=0; }
__declspec(dllexport) HANDLE ImageList_Create(int width,int height,UINT flags,int initial,int grow)
{
    if(width<=0||height<=0||initial<0||grow<0)return 0;
    for(UINT i=0;i<256;i++)if(!g_image_lists[i].Magic){
        SV_IMAGELIST *list=&g_image_lists[i];sv_image_list_clear(list);
        list->Magic=SV_IMAGE_LIST_MAGIC;list->Width=width;list->Height=height;
        list->Flags=flags;list->Capacity=SV_IMAGE_LIST_MAX;return list;
    }
    return 0;
}
__declspec(dllexport) int ImageList_Add(HANDLE handle,HBITMAP image,HBITMAP mask)
{
    (void)mask;SV_IMAGELIST *list=(SV_IMAGELIST*)handle;
    if(!list||list->Magic!=SV_IMAGE_LIST_MAGIC||!image){sv_image_trace(!list?"[imagelist] NULL list":!image?"[imagelist] NULL bitmap":"[imagelist] invalid list");return -1;}
    BITMAP bm={0};if(!GetObjectW(image,sizeof(bm),&bm)){sv_image_trace("[imagelist] GetObject failed");return -1;}
    int count=bm.bmWidth/list->Width;
    if(count<1||list->Count+count>SV_IMAGE_LIST_MAX){sv_image_trace("[imagelist] dimensions/capacity failed");return -1;}
    HDC src=CreateCompatibleDC(0),dst=CreateCompatibleDC(0);
    if(!src||!dst){if(src)DeleteDC(src);if(dst)DeleteDC(dst);return -1;}
    HGDIOBJ oldsrc=SelectObject(src,image),olddst=0;
    int first=list->Count;
    for(int i=0;i<count;i++){
        HBITMAP copy=CreateCompatibleBitmap(src,list->Width,list->Height);
        if(!copy)break;
        HGDIOBJ old=SelectObject(dst,copy);if(!olddst)olddst=old;
        BitBlt(dst,0,0,list->Width,list->Height,src,i*list->Width,0,SRCCOPY);
        list->Images[list->Count++]=copy;
    }
    if(oldsrc)SelectObject(src,oldsrc);if(olddst)SelectObject(dst,olddst);
    DeleteDC(src);DeleteDC(dst);sv_image_trace(list->Count>first?"[imagelist] added pixels":"[imagelist] bitmap allocation failed");return list->Count>first?first:-1;
}
__declspec(dllexport) BOOL ImageList_Destroy(HANDLE handle)
{
    SV_IMAGELIST *list=(SV_IMAGELIST*)handle;
    if(!list||list->Magic!=SV_IMAGE_LIST_MAGIC)return FALSE;
    for(int i=0;i<list->Count;i++)DeleteObject(list->Images[i]);
    sv_image_list_clear(list);return TRUE;
}

typedef LRESULT (*SV_SUBCLASSPROC)(HWND, UINT, WPARAM, LPARAM,
                                   UINT_PTR, ULONG_PTR);
__declspec(dllimport) LONG_PTR SetWindowLongPtrW(HWND, int, LONG_PTR);
typedef struct {
    HWND Window;
    WNDPROC Original;
    UINT Count;
    struct { SV_SUBCLASSPROC Proc; UINT_PTR Id; ULONG_PTR Ref; } Items[8];
} SV_SUBCLASS_WINDOW;
static SV_SUBCLASS_WINDOW g_subclass_windows[128];
/* Each synchronous call keeps its own continuation, including nested sends. */
static struct { SV_SUBCLASS_WINDOW *Window; int Index; } g_subclass_calls[32];
static UINT g_subclass_depth;
static LRESULT sv_subclass_call(SV_SUBCLASS_WINDOW *w, int index,
                                UINT message, WPARAM wp, LPARAM lp)
{
    if (index < 0)
        return w->Original ? w->Original(w->Window,message,wp,lp) :
                            DefWindowProcW(w->Window,message,wp,lp);
    if (g_subclass_depth >= 32) return 0;
    UINT depth=g_subclass_depth++;
    g_subclass_calls[depth].Window=w;
    g_subclass_calls[depth].Index=index;
    LRESULT result=w->Items[index].Proc(w->Window,message,wp,lp,
                        w->Items[index].Id,w->Items[index].Ref);
    g_subclass_depth=depth;
    return result;
}
static LRESULT sv_subclass_dispatch(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    for (UINT i=0;i<128;i++) {
        SV_SUBCLASS_WINDOW *w=&g_subclass_windows[i];
        if (w->Window != hwnd) continue;
        LRESULT result=sv_subclass_call(w,(int)w->Count-1,message,wp,lp);
        if (message==0x82) *w=(SV_SUBCLASS_WINDOW){0};
        return result;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}
__declspec(dllexport) BOOL SetWindowSubclass(HWND hwnd,
        SV_SUBCLASSPROC callback, UINT_PTR id, ULONG_PTR reference)
{
    if (!hwnd || !callback) return FALSE;
    SV_SUBCLASS_WINDOW *w=0;
    for (UINT i=0;i<128;i++)
        if (g_subclass_windows[i].Window==hwnd) {w=&g_subclass_windows[i];break;}
    if (!w) {
        for (UINT i=0;i<128;i++) if (!g_subclass_windows[i].Window) {
            w=&g_subclass_windows[i];w->Window=hwnd;
            w->Original=(WNDPROC)SetWindowLongPtrW(hwnd,-4,(LONG_PTR)sv_subclass_dispatch);
            break;
        }
    }
    if (!w) return FALSE;
    for (UINT i=0;i<w->Count;i++) if (w->Items[i].Proc==callback && w->Items[i].Id==id) {
        w->Items[i].Ref=reference;return TRUE;
    }
    if(w->Count==8)return FALSE;
    UINT i=w->Count++; w->Items[i].Proc=callback;
    w->Items[i].Id=id;w->Items[i].Ref=reference;return TRUE;
}
__declspec(dllexport) LRESULT DefSubclassProc(HWND hwnd, UINT message,
                                               WPARAM wp, LPARAM lp)
{
    if (g_subclass_depth) {
        UINT depth=g_subclass_depth-1;
        SV_SUBCLASS_WINDOW *w=g_subclass_calls[depth].Window;
        if(w->Window==hwnd) return sv_subclass_call(w,g_subclass_calls[depth].Index-1,message,wp,lp);
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}

/* Property sheets are not implemented yet. Export the entry points so the
 * calculator can bind its optional options/about pages and handle failure. */
__declspec(dllexport) HANDLE CreatePropertySheetPageW(const void *page)
{ (void)page; return NULL; }
__declspec(dllexport) int PropertySheetW(const void *header)
{ (void)header; return -1; }

/* Ordinals 380 and 392 are private common-controls imports in the Windows 7
 * calculator. Keep their calls harmless while the corresponding private
 * callbacks remain unsupported. */
__declspec(dllexport) ULONG_PTR SvOrdinal380(void) { return 0; }
__declspec(dllexport) ULONG_PTR SvOrdinal392(void) { return 0; }
