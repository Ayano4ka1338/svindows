#include "Control.h"
#include <SvImageList.h>
static int inside(SV_CONTROL *c, LPARAM lp)
{
    RECT r; GetClientRect(c->hwnd, &r);
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    return x >= 0 && y >= 0 && x < r.right && y < r.bottom;
}
static void click(SV_CONTROL *c)
{
    if (c->style & WS_DISABLED) return;
    UINT type=c->style&BS_TYPEMASK;
    if (type == BS_AUTOCHECKBOX) c->checked = !c->checked;
    if (type == BS_AUTORADIOBUTTON) SvControlSelectAutoRadio(c);
    SvControlInvalidate(c); SvControlNotify(c, BN_CLICKED);
}
typedef struct {
    struct {HWND Window; UINT_PTR Id; UINT Code;} Header;
    DWORD Stage;HDC Dc;RECT Rect;UINT_PTR Item;UINT State;LPARAM Param;
} SV_CUSTOMDRAW;
static LRESULT custom_draw(SV_CONTROL*c,HDC dc,RECT r,DWORD stage)
{
    SV_CUSTOMDRAW n={0};n.Header.Window=c->hwnd;n.Header.Id=c->id;n.Header.Code=(UINT)-12;
    n.Stage=stage;n.Dc=dc;n.Rect=r;n.State=(c->pressed?1:0)|(c->focused?16:0)|(c->hot?64:0)|((c->style&WS_DISABLED)?4:0);
    return c->parent?SendMessageW(c->parent,0x4e,c->id,(LPARAM)&n):0;
}
static void paint(SV_CONTROL *c)
{
    PAINTSTRUCT ps; HDC dc = BeginPaint(c->hwnd, &ps);
    if (!dc) return;
    RECT r; GetClientRect(c->hwnd, &r);
    LRESULT custom=custom_draw(c,dc,r,1);
    if(custom&4){EndPaint(c->hwnd,&ps);return;}
    int state = c->style & WS_DISABLED ? PBS_DISABLED : c->pressed ? PBS_PRESSED :
                c->hot ? PBS_HOT : (c->style & 15) == BS_DEFPUSHBUTTON ? PBS_DEFAULTED : PBS_NORMAL;
    int type = c->style & 15;
    SelectObject(dc, c->font); SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c->style & WS_DISABLED ? RGB(131,131,131) : RGB(0,0,0));
    if (type == BS_GROUPBOX) {
        SIZE extent; extent.cx=0; extent.cy=0;
        if(c->model.length)GetTextExtentPoint32W(dc,c->model.text,c->model.length,&extent);
        int gap=extent.cx+12, mid=7;
        SvControlFill(dc,0,mid,r.right>8?8:r.right,1,RGB(136,136,136));
        SvControlFill(dc,gap,r.top+mid,r.right-gap,1,RGB(136,136,136));
        SvControlFill(dc,0,mid,1,r.bottom-mid,RGB(136,136,136));
        SvControlFill(dc,r.right-1,mid,1,r.bottom-mid,RGB(136,136,136));
        SvControlFill(dc,0,r.bottom-1,r.right,1,RGB(136,136,136));
        RECT caption={10,0,r.right-4,16};
        SvControlFill(dc,8,0,gap-8,14,SV_CONTROL_SURFACE);
        SvGdiDrawTextW(dc,c->model.text,c->model.length,&caption,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
        EndPaint(c->hwnd,&ps);return;
    }
    if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX) {
        SvControlFill(dc, 0, 0, r.right, r.bottom, SV_CONTROL_SURFACE);
        RECT check = {0, (r.bottom-13)/2, 13, (r.bottom-13)/2+13};
        int s = state == PBS_DEFAULTED ? PBS_NORMAL : state;
        DrawThemeBackground(c->theme, dc, BP_CHECKBOX, s + (c->checked ? 4 : 0), &check, NULL);
        r.left = 19;
        SvGdiDrawTextW(dc, c->model.text, c->model.length, &r, DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    } else if (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON) {
        SvControlFill(dc,0,0,r.right,r.bottom,SV_CONTROL_SURFACE);
        RECT radio={0,(r.bottom-13)/2,13,(r.bottom-13)/2+13};
        DrawThemeBackground(c->theme,dc,BP_RADIOBUTTON,state+(c->checked?4:0),&radio,NULL);
        r.left=19;
        SvGdiDrawTextW(dc,c->model.text,c->model.length,&r,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    } else {
        DrawThemeBackground(c->theme, dc, BP_PUSHBUTTON, state, &r, NULL);
        SV_IMAGELIST *list=(SV_IMAGELIST*)c->image_list;
        if(list&&list->Magic==SV_IMAGE_LIST_MAGIC&&list->Count){
            int index=state-1;if(index>=list->Count)index=0;
            HDC src=CreateCompatibleDC(dc);
            HGDIOBJ old=SelectObject(src,list->Images[index]);
            int x=(r.right-list->Width)/2,y=(r.bottom-list->Height)/2;
            BitBlt(dc,x,y,list->Width,list->Height,src,0,0,SRCCOPY);
            if(old)SelectObject(src,old);DeleteDC(src);
        }
        r.left += 4; r.right -= 4;
        SvGdiDrawTextW(dc, c->model.text, c->model.length, &r, DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    }
    if (c->focused) {
        int y0 = 3, y1 = ps.rcPaint.bottom - 4;
        for (int x = 3; x < ps.rcPaint.right-3; x += 2) {
            SetPixel(dc, x, y0, RGB(0,0,0)); SetPixel(dc, x, y1, RGB(0,0,0));
        }
        for (int y = y0; y <= y1; y += 2) {
            SetPixel(dc, 3, y, RGB(0,0,0)); SetPixel(dc, ps.rcPaint.right-4, y, RGB(0,0,0));
        }
    }
    if(custom&0x10){RECT bounds;GetClientRect(c->hwnd,&bounds);custom_draw(c,dc,bounds,2);}
    EndPaint(c->hwnd, &ps);
}
LRESULT SvButtonMessage(SV_CONTROL *c, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case 0x1602: { /* BCM_SETIMAGELIST */
        typedef struct { HANDLE Images; RECT Margin; UINT Align; } BUTTON_IMAGES;
        const BUTTON_IMAGES *images=(const BUTTON_IMAGES*)lp;
        if(!images)return FALSE;
        c->image_list=images->Images;c->image_margin=images->Margin;c->image_align=images->Align;
        SvControlInvalidate(c);return TRUE;
    }
    case WM_PAINT: paint(c); return 0;
    case BM_GETCHECK: return c->checked ? BST_CHECKED : BST_UNCHECKED;
    case BM_SETCHECK: c->checked = wp == BST_CHECKED; SvControlInvalidate(c); return 0;
    case BM_GETSTATE: return (c->checked ? BST_CHECKED : 0) | (c->pressed ? BST_PUSHED : 0) | (c->focused ? BST_FOCUS : 0);
    case BM_SETSTATE: c->pressed = !!wp; SvControlInvalidate(c); return 0;
    case BM_CLICK: click(c); return 0;
    case WM_MOUSEMOVE: {
        int hot = inside(c, lp);
        if (hot) { TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, c->hwnd, 0}; TrackMouseEvent(&track); }
        if (c->hot != hot) { c->hot = hot; SvControlInvalidate(c); }
        return 0;
    }
    case WM_MOUSELEAVE: c->hot = 0; SvControlInvalidate(c); return 0;
    case WM_LBUTTONDOWN:
        if (!(c->style & WS_DISABLED)) { c->pressed = 1; SetFocus(c->hwnd); SetCapture(c->hwnd); SvControlInvalidate(c); }
        return 0;
    case WM_LBUTTONUP: {
        int activate = c->pressed && inside(c, lp);
        c->pressed = 0; ReleaseCapture(); SvControlInvalidate(c);
        if (activate) click(c);
        return 0;
    }
    case WM_CAPTURECHANGED: c->pressed = 0; SvControlInvalidate(c); return 0;
    case WM_KEYDOWN:
        if (wp == VK_SPACE && !(c->style & WS_DISABLED)) { c->pressed = 1; SvControlInvalidate(c); }
        return 0;
    case WM_KEYUP:
        if (wp == VK_SPACE && c->pressed) { c->pressed = 0; click(c); }
        return 0;
    }
    return DefWindowProcW(c->hwnd, msg, wp, lp);
}
