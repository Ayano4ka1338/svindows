#include "Common.h"

typedef struct SHELL_DIALOG {
    struct SHELL_DIALOG *previous;
    HWND window,owner,edit,ok,cancel;
    const WCHAR *label,*caption;
    WCHAR *value;SIZE_T capacity;
    BOOL done,accepted,confirmation,information;
} SHELL_DIALOG;
static SHELL_DIALOG *active_dialog;

static LRESULT dialog_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    SHELL_DIALOG *state=(void *)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(message==WM_CREATE) {
        state=((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)state);state->window=hwnd;
        CreateWindowExW(0,L"STATIC",state->label,WS_CHILD|WS_VISIBLE|SS_LEFT,
            18,16,464,state->information?116:42,hwnd,NULL,ShellInstance,NULL);
        if(state->value) {
            state->edit=ShellEdit(hwnd,100,state->value);
            MoveWindow(state->edit,18,62,464,28,TRUE);
            SendMessageW(state->edit,EM_LIMITTEXT,state->capacity-1,0);
            SendMessageW(state->edit,EM_SETSEL,0,-1);
        }
        state->ok=ShellButton(hwnd,IDOK,state->confirmation?L"Yes":L"OK");
        int y=state->information?148:106;
        MoveWindow(state->ok,state->information?382:282,y,100,30,TRUE);
        if(!state->information) {state->cancel=ShellButton(hwnd,IDCANCEL,L"Cancel");MoveWindow(state->cancel,382,y,100,30,TRUE);}
        return state->ok&&(!state->value||state->edit)&&
            (state->information||state->cancel)?0:-1;
    }
    if(!state) return DefWindowProcW(hwnd,message,wp,lp);
    if(message==WM_COMMAND&&HIWORD(wp)==BN_CLICKED) {
        if(LOWORD(wp)==IDOK) {
            if(state->value) {
                int length=GetWindowTextLengthW(state->edit);
                if(length<0||(SIZE_T)length>=state->capacity) return 0;
                if(GetWindowTextW(state->edit,state->value,(int)state->capacity)!=length) return 0;
            }
            state->accepted=TRUE;state->done=TRUE;return 0;
        }
        if(LOWORD(wp)==IDCANCEL) {state->done=TRUE;return 0;}
    }
    if(message==WM_CLOSE) {state->done=TRUE;return 0;}
    if(message==WM_DESTROY) {state->window=NULL;state->done=TRUE;return 0;}
    if(message==WM_PAINT) {PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(dc){RECT rect;GetClientRect(hwnd,&rect);ShellPaintFill(dc,&rect,RGB(246,247,249));EndPaint(hwnd,&ps);}return 0;}
    return DefWindowProcW(hwnd,message,wp,lp);
}
void ShellRegisterDialogClass(void)
{
    WNDCLASSEXW cls;ShellZero(&cls,sizeof(cls));cls.cbSize=sizeof(cls);cls.lpfnWndProc=dialog_proc;
    cls.hInstance=ShellInstance;cls.hCursor=LoadCursorW(NULL,IDC_ARROW);cls.lpszClassName=L"SvShellDialog";
    RegisterClassExW(&cls);
}
BOOL ShellProcessDialogMessage(MSG *message)
{
    SHELL_DIALOG *state=active_dialog;
    if(!state||!state->window) return FALSE;
    HWND root=GetAncestor(message->hwnd,GA_ROOT);
    if(root!=state->window) {
        /* The shell's own modal loop prevents commands to its disabled owner;
         * unrelated applications retain their separate message queues. */
        if(message->message>=WM_KEYDOWN&&message->message<=0x0109) return TRUE;
        if(message->message>=WM_MOUSEMOVE&&message->message<=0x020e) return TRUE;
        return FALSE;
    }
    if(message->message!=WM_KEYDOWN) return FALSE;
    if(message->wParam==VK_ESCAPE) {state->done=TRUE;return TRUE;}
    if(message->wParam==VK_RETURN) {SendMessageW(state->window,WM_COMMAND,IDOK,0);return TRUE;}
    if(message->wParam==VK_TAB) {
        HWND controls[3]={state->edit,state->ok,state->cancel};int count=state->edit?3:2;
        if(!state->edit){controls[0]=state->ok;controls[1]=state->cancel;}
        if(state->information) count=1;
        int selected=0;HWND focus=GetFocus();for(int i=0;i<count;++i) if(controls[i]==focus) selected=i;
        selected=(selected+((GetKeyState(VK_SHIFT)&0x8000)?count-1:1))%count;
        SetFocus(controls[selected]);return TRUE;
    }
    return FALSE;
}
static BOOL show_dialog(SHELL_DIALOG *state)
{
    RECT bounds={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
    SystemParametersInfoW(SPI_GETWORKAREA,0,&bounds,0);
    int height=state->information?230:188;
    state->window=CreateWindowExW(WS_EX_TOOLWINDOW,L"SvShellDialog",state->caption,
        WS_POPUP|WS_CAPTION|WS_SYSMENU,(bounds.right+bounds.left-520)/2,
        (bounds.bottom+bounds.top-height)/2,520,height,state->owner,NULL,ShellInstance,state);
    if(!state->window) {ShellLog("[explorer] could not create shell dialog\r\n");return FALSE;}
    state->previous=active_dialog;active_dialog=state;
    if(state->owner) EnableWindow(state->owner,FALSE);
    ShowWindow(state->window,SW_SHOW);UpdateWindow(state->window);
    SetForegroundWindow(state->window);SetFocus(state->edit?state->edit:state->ok);
    MSG message;BOOL quit=FALSE;int code=0;
    while(!state->done) {
        BOOL result=GetMessageW(&message,NULL,0,0);
        if(result<=0) {quit=result==0;code=(int)message.wParam;break;}
        if(ShellProcessDialogMessage(&message)) continue;
        TranslateMessage(&message);DispatchMessageW(&message);
    }
    active_dialog=state->previous;
    if(state->window) DestroyWindow(state->window);
    if(state->owner&&IsWindow(state->owner)) {EnableWindow(state->owner,TRUE);SetForegroundWindow(state->owner);}
    if(quit) PostQuitMessage(code);
    return state->accepted;
}
BOOL ShellPrompt(HWND owner,const WCHAR *caption,const WCHAR *label,WCHAR *value,SIZE_T capacity)
{ SHELL_DIALOG state;ShellZero(&state,sizeof(state));state.owner=owner;state.caption=caption;state.label=label;state.value=value;state.capacity=capacity;return show_dialog(&state); }
BOOL ShellConfirm(HWND owner,const WCHAR *caption,const WCHAR *text)
{ SHELL_DIALOG state;ShellZero(&state,sizeof(state));state.owner=owner;state.caption=caption;state.label=text;state.confirmation=TRUE;return show_dialog(&state); }
void ShellError(HWND owner,const WCHAR *action,const WCHAR *path,DWORD error)
{
    WCHAR text[1024],number[24],system[384];SIZE_T used=0;
    ShellCopy(text,1024,path?path:L"");used=ShellLength(text);
    ShellCopy(text+used,1024-used,L"\r\n\r\n");used=ShellLength(text);
    DWORD n=FormatMessageW(0x1000|0x200,NULL,error,0,system,384,NULL);
    if(n) ShellCopy(text+used,1024-used,system);
    else {ShellCopy(text+used,1024-used,L"Windows error ");used=ShellLength(text);ShellUnsigned(number,24,error);ShellCopy(text+used,1024-used,number);}
    SHELL_DIALOG state;ShellZero(&state,sizeof(state));state.owner=owner;state.caption=action;state.label=text;state.information=TRUE;
    ShellLogPath("[explorer] operation failed: ",path);show_dialog(&state);
}
