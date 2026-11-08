#ifndef SVWINDOWS_CONTROL_H
#define SVWINDOWS_CONTROL_H
#include "../User32.h"
#include <Uxtheme.h>
#include "EditModel.h"
enum { SV_BUTTON = 1, SV_EDIT, SV_STATIC, SV_COMBO };
#define SV_CONTROL_SURFACE RGB(255,255,255)
typedef struct SV_CONTROL {
    struct SV_CONTROL *next;
    HWND hwnd, parent;
    UINT id, kind, refs;
    DWORD style, ex_style;
    HANDLE image_list;
    RECT image_margin;
    UINT image_align;
    HBITMAP image;
    HFONT font;
    HTHEME theme;
    EDIT_MODEL model;
    void *private_data;
    int dead, focused, hot, pressed, checked, dragging;
    int first_line, scroll_x;
} SV_CONTROL;
int SvRegisterControl(const WCHAR *name);
LRESULT SvComboMessage(SV_CONTROL*,UINT,WPARAM,LPARAM);
void SvComboDispose(SV_CONTROL*);
void SvControlNotify(SV_CONTROL *c, UINT code);
void SvControlSelectAutoRadio(SV_CONTROL *c);
void SvControlInvalidate(SV_CONTROL *c);
void SvControlFill(HDC dc, int x, int y, int w, int h, COLORREF color);
LRESULT SvButtonMessage(SV_CONTROL *c, UINT msg, WPARAM wp, LPARAM lp);
LRESULT SvEditMessage(SV_CONTROL *c, UINT msg, WPARAM wp, LPARAM lp);
LRESULT SvStaticMessage(SV_CONTROL *c, UINT msg, WPARAM wp, LPARAM lp);
void SvEditEnsureVisible(SV_CONTROL *c);
#endif
