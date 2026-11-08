#include "Control.h"
LRESULT SvStaticMessage(SV_CONTROL *c, UINT msg, WPARAM wp, LPARAM lp)
{
    if((msg==WM_LBUTTONDOWN||msg==0x203)&&(c->style&0x100)){SvControlNotify(c,msg==WM_LBUTTONDOWN?0:1);return 0;}
    if(msg==0x172 && wp==0){HBITMAP old=c->image;c->image=(HBITMAP)lp;SvControlInvalidate(c);return (LRESULT)old;}
    if(msg==0x173 && wp==0)return (LRESULT)c->image;
    if ((c->style & 31) == SS_BITMAP) {
        if(msg!=WM_PAINT)return DefWindowProcW(c->hwnd,msg,wp,lp);
        PAINTSTRUCT ps;HDC dc=BeginPaint(c->hwnd,&ps);
        if(dc&&c->image){BITMAP bm={0};GetObjectW(c->image,sizeof(bm),&bm);
            HDC src=CreateCompatibleDC(dc);HGDIOBJ old=SelectObject(src,c->image);
            BitBlt(dc,0,0,bm.bmWidth,bm.bmHeight,src,0,0,SRCCOPY);
            if(old)SelectObject(src,old);DeleteDC(src);}
        if(dc)EndPaint(c->hwnd,&ps);return 0;
    }
    if ((c->style & 31) == SS_ENHMETAFILE) return DefWindowProcW(c->hwnd,msg,wp,lp);
    if (msg != WM_PAINT) return DefWindowProcW(c->hwnd, msg, wp, lp);
    PAINTSTRUCT ps; HDC dc = BeginPaint(c->hwnd, &ps);
    if (!dc) return 0;
    RECT r; GetClientRect(c->hwnd, &r);
    SvControlFill(dc, 0, 0, r.right, r.bottom, SV_CONTROL_SURFACE);
    SelectObject(dc, c->font); SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c->style & WS_DISABLED ? RGB(131,131,131) : RGB(0,0,0));
    UINT flags = (c->style & 31) == SS_CENTER ? DT_CENTER : (c->style & 31) == SS_RIGHT ? DT_RIGHT : DT_LEFT;
    flags |= (c->style & 31) == SS_LEFTNOWORDWRAP || (c->style & 31) == SS_SIMPLE ? DT_SINGLELINE : DT_WORDBREAK;
    if (c->style & SS_NOPREFIX) flags |= DT_NOPREFIX;
    SvGdiDrawTextW(dc, c->model.text, c->model.length, &r, flags);
    EndPaint(c->hwnd, &ps); return 0;
}
