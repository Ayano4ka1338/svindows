#include "FileManager.h"

enum {
    ID_START=200,ID_WINDOWS,ID_PREVIOUS,ID_NEXT,ID_CLOCK,ID_DESKTOP_SCROLL,
    ID_SEARCH,ID_APPLICATIONS,ID_RUN,ID_COMPUTER,ID_DOCUMENTS,ID_RESCAN,
    WM_DESKTOP_REFRESH=WM_APP+20,
    TASK_RESTORE=1,TASK_MINIMIZE,TASK_MAXIMIZE,TASK_CLOSE
};
typedef struct DESKTOP_ITEM {
    HWND window;WCHAR name[SHELL_PATH_CAPACITY],path[SHELL_PATH_CAPACITY];
    DWORD attributes;BOOL shortcut;
} DESKTOP_ITEM;
typedef struct { WCHAR name[SHELL_PATH_CAPACITY],path[SHELL_PATH_CAPACITY]; } APP_ITEM;
typedef struct TASK_ITEM {
    HWND target,button;DWORD process;WCHAR title[SHELL_PATH_CAPACITY];
} TASK_ITEM;
typedef struct { WCHAR path[SHELL_PATH_CAPACITY]; } APP_DIRECTORY;
static HWND desktop,taskbar,start_panel,start_button,windows_button,previous_button,next_button,clock_control,desktop_scroll;
static HWND search_control,applications_control,run_button,computer_button,documents_button,rescan_button;
static DESKTOP_ITEM **desktop_items;static SIZE_T desktop_count,desktop_capacity;
static APP_ITEM *applications;static SIZE_T application_count,application_capacity;
static TASK_ITEM **tasks;static SIZE_T task_count,task_capacity,task_page;
static HWND last_application;
static UINT shell_message;
__declspec(dllimport) BOOL RegisterHotKey(HWND window, int id, UINT modifiers, UINT key);
__declspec(dllimport) BOOL UnregisterHotKey(HWND window, int id);
__declspec(dllimport) HANDLE CreateEventW(void *security, BOOL manual, BOOL initial, const WCHAR *name);
__declspec(dllimport) BOOL SetEvent(HANDLE event);
static int desktop_row;
static BOOL refresh_tasks_pending,desktop_dirty,closing_shell;
static RECT initial_work_area;

static void task_refresh(void);
static void desktop_layout(void);
static void taskbar_layout(void);
static BOOL catalog_scan(void);
static void start_show(BOOL show);

static void paint_text(HDC dc,const WCHAR *text,RECT rect,COLORREF color,UINT flags)
{SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);DrawTextW(dc,text,-1,&rect,flags|DT_NOPREFIX);}
static BOOL add_application(APP_ITEM **items,SIZE_T *count,SIZE_T *capacity,const WCHAR *path)
{
    for(SIZE_T i=0;i<*count;++i)if(!ShellCompare((*items)[i].path,path))return TRUE;
    if(!ShellReserve((void **)items,capacity,*count+1,sizeof(**items)))return FALSE;
    APP_ITEM *app=*items+(*count)++;ShellCopy(app->path,SHELL_PATH_CAPACITY,path);
    ShellCopy(app->name,SHELL_PATH_CAPACITY,ShellLeaf(path));
    SIZE_T n=ShellLength(app->name);if(n>4&&!ShellCompare(app->name+n-4,L".exe"))app->name[n-4]=0;
    return TRUE;
}
static BOOL catalog_scan(void)
{
    APP_ITEM *fresh=NULL;SIZE_T count=0,capacity=0;
    APP_DIRECTORY *queue=NULL;SIZE_T queue_count=0,queue_capacity=0;
    const WCHAR *roots[]={L"C:\\WINDOWS7",L"C:\\WINDOWS\\SYSTEM32",L"C:\\Program Files"};
    if(GetFileAttributesW(L"C:\\CMD.EXE")!=INVALID_FILE_ATTRIBUTES&&!add_application(&fresh,&count,&capacity,L"C:\\CMD.EXE"))goto memory_error;
    for(UINT i=0;i<sizeof(roots)/sizeof(*roots);++i) {
        DWORD attr=GetFileAttributesW(roots[i]);if(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_DIRECTORY))continue;
        if(!ShellReserve((void **)&queue,&queue_capacity,queue_count+1,sizeof(*queue)))goto memory_error;
        ShellCopy(queue[queue_count++].path,SHELL_PATH_CAPACITY,roots[i]);
    }
    for(SIZE_T pos=0;pos<queue_count;++pos) {
        WCHAR directory[SHELL_PATH_CAPACITY],pattern[SHELL_PATH_CAPACITY];ShellCopy(directory,SHELL_PATH_CAPACITY,queue[pos].path);
        if(!ShellJoin(pattern,SHELL_PATH_CAPACITY,directory,L"*"))continue;
        SHELL_FIND_DATA data;HANDLE find=FindFirstFileW(pattern,&data);if(find==INVALID_HANDLE_VALUE)continue;
        do {
            if(data.name[0]=='.'&&(!data.name[1]||(data.name[1]=='.'&&!data.name[2])))continue;
            WCHAR path[SHELL_PATH_CAPACITY];if(!ShellJoin(path,SHELL_PATH_CAPACITY,directory,data.name))continue;
            if(data.attributes&FILE_ATTRIBUTE_DIRECTORY) {
                /* Program directories may contain nested installed apps. System
                 * directory descendants are assets, not a second catalog root. */
                if(ShellPathWithin(directory,L"C:\\Program Files")) {
                    if(!ShellReserve((void **)&queue,&queue_capacity,queue_count+1,sizeof(*queue))){FindClose(find);goto memory_error;}
                    ShellCopy(queue[queue_count++].path,SHELL_PATH_CAPACITY,path);
                }
            } else if(!ShellCompare(ShellExtension(path),L".exe")) {
                if(!add_application(&fresh,&count,&capacity,path)){FindClose(find);goto memory_error;}
            }
        }while(FindNextFileW(find,&data));FindClose(find);
    }
    ShellFree(queue);
    /* Stable alphabetic ordering keeps keyboard selection reproducible while
     * duplicate names retain distinct actual executable paths. */
    for(SIZE_T i=1;i<count;++i){APP_ITEM value=fresh[i];SIZE_T j=i;while(j&&ShellCompare(fresh[j-1].name,value.name)>0){fresh[j]=fresh[j-1];--j;}fresh[j]=value;}
    ShellFree(applications);applications=fresh;application_count=count;application_capacity=capacity;return TRUE;
memory_error:
    ShellFree(queue);ShellFree(fresh);ShellError(taskbar,L"Discover applications",L"Installed applications",8);return FALSE;
}
static BOOL matches_filter(const WCHAR *name,const WCHAR *filter)
{
    SIZE_T length=ShellLength(filter),n=ShellLength(name);if(!length)return TRUE;
    if(length>n)return FALSE;
    for(SIZE_T i=0;i<=n-length;++i)if(CompareStringOrdinal(name+i,(int)length,filter,(int)length,TRUE)==2)return TRUE;
    return FALSE;
}
static void start_populate(void)
{
    WCHAR filter[SHELL_PATH_CAPACITY];filter[0]=0;GetWindowTextW(search_control,filter,SHELL_PATH_CAPACITY);
    SendMessageW(applications_control,LVM_DELETEALLITEMS,0,0);int row=0;
    for(SIZE_T i=0;i<application_count;++i) {
        if(!matches_filter(applications[i].name,filter))continue;
        LVITEMW item;ShellZero(&item,sizeof(item));item.mask=LVIF_TEXT|LVIF_PARAM;item.iItem=row++;
        item.pszText=applications[i].name;item.lParam=(LPARAM)i;
        if(SendMessageW(applications_control,LVM_INSERTITEMW,0,(LPARAM)&item)<0)break;
        item.iSubItem=1;item.pszText=applications[i].path;SendMessageW(applications_control,LVM_SETITEMTEXTW,item.iItem,(LPARAM)&item);
    }
    if(row){LVITEMW selected;ShellZero(&selected,sizeof(selected));selected.stateMask=LVIS_SELECTED|LVIS_FOCUSED;selected.state=selected.stateMask;SendMessageW(applications_control,LVM_SETITEMSTATE,0,(LPARAM)&selected);}
}
static void start_launch(void)
{
    int selected=(int)SendMessageW(applications_control,LVM_GETNEXTITEM,(WPARAM)-1,LVNI_SELECTED);if(selected<0)return;
    LVITEMW item;ShellZero(&item,sizeof(item));item.mask=LVIF_PARAM;item.iItem=selected;
    if(!SendMessageW(applications_control,LVM_GETITEMW,0,(LPARAM)&item)||(SIZE_T)item.lParam>=application_count)return;
    WCHAR path[SHELL_PATH_CAPACITY];ShellCopy(path,SHELL_PATH_CAPACITY,applications[item.lParam].path);
    start_show(FALSE);ShellLaunch(taskbar,path,NULL);
}
static void run_dialog(void)
{
    start_show(FALSE);WCHAR command[1024];command[0]=0;
    if(ShellPrompt(taskbar,L"Run",L"Program and arguments",command,1024)&&command[0])ShellRunCommand(taskbar,command);
}
static LRESULT start_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    if(message==WM_CREATE) {
        start_panel=hwnd;
        CreateWindowExW(0,L"STATIC",L"Applications",WS_CHILD|WS_VISIBLE,16,12,300,24,hwnd,NULL,ShellInstance,NULL);
        search_control=ShellEdit(hwnd,ID_SEARCH,L"");
        if(search_control)SendMessageW(search_control,EM_LIMITTEXT,SHELL_PATH_CAPACITY-1,0);
        applications_control=CreateWindowExW(0,WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|LVS_REPORT|LVS_SINGLESEL,
            0,0,1,1,hwnd,(HMENU)(UINT_PTR)ID_APPLICATIONS,ShellInstance,NULL);
        run_button=ShellButton(hwnd,ID_RUN,L"Run…");computer_button=ShellButton(hwnd,ID_COMPUTER,L"Computer");
        documents_button=ShellButton(hwnd,ID_DOCUMENTS,L"Documents");rescan_button=ShellButton(hwnd,ID_RESCAN,L"Refresh apps");
        if(!search_control||!applications_control||!run_button||!computer_button||!documents_button||!rescan_button)return -1;
        const WCHAR *labels[]={L"Application",L"Executable"};const int widths[]={190,356};
        for(int i=0;i<2;++i){LVCOLUMNW column;ShellZero(&column,sizeof(column));column.mask=LVCF_TEXT|LVCF_WIDTH;column.pszText=(WCHAR *)labels[i];column.cx=widths[i];SendMessageW(applications_control,LVM_INSERTCOLUMNW,i,(LPARAM)&column);}
        return 0;
    }
    if(message==WM_SIZE) {
        RECT client;GetClientRect(hwnd,&client);MoveWindow(search_control,16,42,client.right-32,28,TRUE);
        MoveWindow(applications_control,16,82,client.right-32,client.bottom-140,TRUE);
        HWND controls[]={run_button,computer_button,documents_button,rescan_button};int width=(client.right-44)/4;
        for(int i=0;i<4;++i)MoveWindow(controls[i],16+i*(width+4),client.bottom-42,width,30,TRUE);return 0;
    }
    if(message==WM_COMMAND) {
        UINT id=LOWORD(wp),notification=HIWORD(wp);
        if(id==ID_SEARCH&&notification==EN_CHANGE){start_populate();return 0;}
        if(notification==BN_CLICKED) {
            if(id==ID_RUN)run_dialog();
            else if(id==ID_COMPUTER){start_show(FALSE);ShellOpenFolder(L"C:\\");}
            else if(id==ID_DOCUMENTS){start_show(FALSE);ShellOpenFolder(ShellDocumentsPath);}
            else if(id==ID_RESCAN){catalog_scan();start_populate();}
            return 0;
        }
    }
    if(message==WM_NOTIFY){NMHDR *note=(void *)lp;if(note&&note->hwndFrom==applications_control&&note->code==LVN_ITEMACTIVATE){start_launch();return 0;}}
    if(message==WM_ACTIVATE&&LOWORD(wp)==0){ShowWindow(hwnd,SW_HIDE);return 0;}
    if(message==WM_CLOSE){start_show(FALSE);return 0;}
    if(message==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(dc){RECT rect;GetClientRect(hwnd,&rect);ShellPaintFill(dc,&rect,RGB(247,249,252));EndPaint(hwnd,&ps);}return 0;}
    return DefWindowProcW(hwnd,message,wp,lp);
}
static void start_show(BOOL show)
{
    if(!show){if(start_panel)ShowWindow(start_panel,SW_HIDE);return;}
    catalog_scan();
    int height=GetSystemMetrics(SM_CYSCREEN)-80;if(height>610)height=610;if(height<300)height=300;
    if(!start_panel)start_panel=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,L"SvStartPanel",L"Start",WS_POPUP|WS_BORDER,
        0,GetSystemMetrics(SM_CYSCREEN)-44-height,590,height,taskbar,NULL,ShellInstance,NULL);
    if(!start_panel){ShellError(taskbar,L"Open Start",L"Applications",GetLastError());return;}
    SetWindowTextW(search_control,L"");start_populate();
    SetWindowPos(start_panel,HWND_TOPMOST,0,GetSystemMetrics(SM_CYSCREEN)-44-height,590,height,SWP_SHOWWINDOW);
    ShowWindow(start_panel,SW_SHOW);UpdateWindow(start_panel);SetForegroundWindow(start_panel);SetFocus(search_control);
}
static void task_action(HWND target,UINT action)
{
    if(!IsWindow(target))return;
    if(action==TASK_CLOSE)PostMessageW(target,WM_CLOSE,0,0);
    else if(action==TASK_MINIMIZE)ShowWindow(target,SW_MINIMIZE);
    else {ShowWindow(target,action==TASK_MAXIMIZE?SW_SHOWMAXIMIZED:SW_RESTORE);SetForegroundWindow(target);last_application=target;}
}
static void task_context(TASK_ITEM *task,LPARAM lp)
{
    HMENU menu=CreatePopupMenu();if(!menu)return;
    AppendMenuW(menu,MF_STRING,TASK_RESTORE,L"Restore");AppendMenuW(menu,MF_STRING,TASK_MINIMIZE,L"Minimize");
    AppendMenuW(menu,MF_STRING,TASK_MAXIMIZE,L"Maximize");AppendMenuW(menu,MF_SEPARATOR,0,NULL);AppendMenuW(menu,MF_STRING,TASK_CLOSE,L"Close");
    UINT action=(UINT)TrackPopupMenu(menu,TPM_RETURNCMD,(short)LOWORD(lp),(short)HIWORD(lp),0,taskbar,NULL);DestroyMenu(menu);task_action(task->target,action);
}
static LRESULT task_button_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    TASK_ITEM *task=(void *)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(message==WM_CREATE){task=((CREATESTRUCTW *)lp)->lpCreateParams;SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)task);return 0;}
    if(!task)return DefWindowProcW(hwnd,message,wp,lp);
    if(message==WM_LBUTTONDOWN||(message==WM_KEYDOWN&&(wp==VK_RETURN||wp==VK_SPACE))) {
        if(last_application==task->target&&!IsIconic(task->target))task_action(task->target,TASK_MINIMIZE);
        else task_action(task->target,TASK_RESTORE);InvalidateRect(hwnd,NULL,FALSE);return 0;
    }
    if(message==WM_CONTEXTMENU){task_context(task,lp);return 0;}
    if(message==WM_PAINT) {
        PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(dc){RECT rect;GetClientRect(hwnd,&rect);
            BOOL active=GetAncestor(GetForegroundWindow(),GA_ROOTOWNER)==task->target;
            ShellPaintFill(dc,&rect,active?RGB(47,79,108):RGB(31,42,57));
            if(active){RECT line={rect.left,rect.bottom-3,rect.right,rect.bottom};ShellPaintFill(dc,&line,RGB(84,177,238));}
            rect.left+=9;rect.right-=8;paint_text(dc,task->title,rect,RGB(246,248,252),DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);EndPaint(hwnd,&ps);}return 0;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}
typedef struct { HWND *windows;SIZE_T count,capacity;BOOL failed; } TASK_SNAPSHOT;
static BOOL collect_task(HWND hwnd,LPARAM context)
{
    TASK_SNAPSHOT *snapshot=(void *)context;
    if(hwnd==desktop||hwnd==taskbar||hwnd==start_panel||!IsWindowVisible(hwnd))return TRUE;
    DWORD style=(DWORD)GetWindowLongPtrW(hwnd,GWL_STYLE),exstyle=(DWORD)GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
    if((style&WS_CHILD)||(exstyle&WS_EX_TOOLWINDOW)||GetWindow(hwnd,4))return TRUE;
    if(!GetWindowThreadProcessId(hwnd,NULL))return TRUE;
    if(!ShellReserve((void **)&snapshot->windows,&snapshot->capacity,snapshot->count+1,sizeof(HWND))){snapshot->failed=TRUE;return FALSE;}
    snapshot->windows[snapshot->count++]=hwnd;return TRUE;
}
static void task_refresh(void)
{
    TASK_SNAPSHOT snapshot;ShellZero(&snapshot,sizeof(snapshot));BOOL enumerated=EnumWindows(collect_task,(LPARAM)&snapshot);
    if(!enumerated||snapshot.failed){ShellFree(snapshot.windows);return;}
    for(SIZE_T i=task_count;i>0;--i) {
        TASK_ITEM *task=tasks[i-1];BOOL found=FALSE;
        for(SIZE_T j=0;j<snapshot.count;++j)if(snapshot.windows[j]==task->target){found=TRUE;break;}
        if(!found){DestroyWindow(task->button);ShellFree(task);for(SIZE_T j=i;j<task_count;++j)tasks[j-1]=tasks[j];--task_count;}
    }
    for(SIZE_T i=0;i<snapshot.count;++i) {
        TASK_ITEM *task=NULL;for(SIZE_T j=0;j<task_count;++j)if(tasks[j]->target==snapshot.windows[i]){task=tasks[j];break;}
        if(!task) {
            if(!ShellReserve((void **)&tasks,&task_capacity,task_count+1,sizeof(*tasks)))break;
            task=ShellAlloc(sizeof(*task));if(!task)break;
            task->target=snapshot.windows[i];GetWindowThreadProcessId(task->target,&task->process);
            task->button=CreateWindowExW(0,L"SvTaskButton",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,1,1,taskbar,NULL,ShellInstance,task);
            if(!task->button){ShellFree(task);continue;}tasks[task_count++]=task;
        }
        WCHAR title[SHELL_PATH_CAPACITY];if(!GetWindowTextW(task->target,title,SHELL_PATH_CAPACITY))ShellCopy(title,SHELL_PATH_CAPACITY,L"Untitled");
        if(ShellCompare(task->title,title)){ShellCopy(task->title,SHELL_PATH_CAPACITY,title);SetWindowTextW(task->button,title);InvalidateRect(task->button,NULL,FALSE);}
    }
    ShellFree(snapshot.windows);taskbar_layout();
    for(SIZE_T i=0;i<task_count;++i)InvalidateRect(tasks[i]->button,NULL,FALSE);
    refresh_tasks_pending=FALSE;
}
static void windows_menu(void)
{
    task_refresh();HMENU menu=CreatePopupMenu();if(!menu)return;
    for(SIZE_T i=0;i<task_count;++i)AppendMenuW(menu,MF_STRING,(UINT_PTR)i+1,tasks[i]->title);
    if(!task_count)AppendMenuW(menu,MF_GRAYED,0,L"No open windows");
    RECT rect;GetWindowRect(windows_button,&rect);
    UINT choice=(UINT)TrackPopupMenu(menu,TPM_RETURNCMD,rect.left,rect.top,0,taskbar,NULL);DestroyMenu(menu);
    if(choice&&choice<=task_count)task_action(tasks[choice-1]->target,TASK_RESTORE);
}
static void taskbar_layout(void)
{
    if(!taskbar)return;RECT rect;GetClientRect(taskbar,&rect);int width=rect.right;
    MoveWindow(start_button,4,4,76,34,TRUE);MoveWindow(windows_button,84,4,84,34,TRUE);
    int room=width-302;SIZE_T visible=room>130?(SIZE_T)(room/136):1;
    SIZE_T pages=(task_count+visible-1)/visible;if(task_page>=pages)task_page=pages?pages-1:0;
    for(SIZE_T i=0;i<task_count;++i) {
        BOOL shown=i>=task_page*visible&&i<(task_page+1)*visible;
        if(shown)MoveWindow(tasks[i]->button,174+(int)(i-task_page*visible)*136,4,132,34,TRUE);
        ShowWindow(tasks[i]->button,shown?SW_SHOW:SW_HIDE);
    }
    MoveWindow(previous_button,width-120,4,26,34,TRUE);MoveWindow(next_button,width-90,4,26,34,TRUE);
    ShowWindow(previous_button,pages>1?SW_SHOW:SW_HIDE);ShowWindow(next_button,pages>1?SW_SHOW:SW_HIDE);
    EnableWindow(previous_button,task_page>0);EnableWindow(next_button,task_page+1<pages);
    MoveWindow(clock_control,width-59,10,55,24,TRUE);
}
static void update_clock(void)
{
    SHELL_SYSTEMTIME time;ShellZero(&time,sizeof(time));GetLocalTime(&time);
    WCHAR text[6]={(WCHAR)('0'+time.hour/10),(WCHAR)('0'+time.hour%10),':',(WCHAR)('0'+time.minute/10),(WCHAR)('0'+time.minute%10),0};SetWindowTextW(clock_control,text);
}
static LRESULT taskbar_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    if(message==WM_CREATE) {
        taskbar=hwnd;start_button=ShellButton(hwnd,ID_START,L"Start");windows_button=ShellButton(hwnd,ID_WINDOWS,L"Windows");
        previous_button=ShellButton(hwnd,ID_PREVIOUS,L"<");next_button=ShellButton(hwnd,ID_NEXT,L">");
        clock_control=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_CENTER,0,0,1,1,hwnd,NULL,ShellInstance,NULL);
        if(!start_button||!windows_button||!previous_button||!next_button||!clock_control)return -1;
        SetTimer(hwnd,1,500,NULL);RegisterShellHookWindow(hwnd);taskbar_layout();update_clock();return 0;
    }
    if(shell_message&&message==shell_message) {
        if((wp==4||wp==0x8004)&&lp) {
            HWND active=(HWND)lp;
            if(active!=taskbar&&active!=desktop&&active!=start_panel)last_application=GetAncestor(active,GA_ROOTOWNER);
        }
        refresh_tasks_pending=TRUE;return 0;
    }
    if(message==WM_COMMAND&&HIWORD(wp)==BN_CLICKED) {
        switch(LOWORD(wp)) {
        case ID_START:start_show(!start_panel||!IsWindowVisible(start_panel));break;
        case ID_WINDOWS:windows_menu();break;
        case ID_PREVIOUS:if(task_page)--task_page;taskbar_layout();break;
        case ID_NEXT:++task_page;taskbar_layout();break;
        }return 0;
    }
    if(message==WM_TIMER){task_refresh();update_clock();return 0;}
    if(message==WM_SIZE){taskbar_layout();return 0;}
    if(message==WM_DISPLAYCHANGE){PostMessageW(desktop,WM_DISPLAYCHANGE,wp,lp);return 0;}
    if(message==WM_CLOSE){if(closing_shell)DestroyWindow(hwnd);return 0;}
    if(message==WM_DESTROY){KillTimer(hwnd,1);DeregisterShellHookWindow(hwnd);taskbar=NULL;return 0;}
    if(message==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(dc){RECT rect;GetClientRect(hwnd,&rect);ShellPaintFill(dc,&rect,RGB(23,32,45));EndPaint(hwnd,&ps);}return 0;}
    return DefWindowProcW(hwnd,message,wp,lp);
}
static LRESULT desktop_icon_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    DESKTOP_ITEM *item=(void *)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(message==WM_CREATE){item=((CREATESTRUCTW *)lp)->lpCreateParams;SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)item);return 0;}
    if(!item)return DefWindowProcW(hwnd,message,wp,lp);
    if(message==WM_LBUTTONDOWN){SetFocus(hwnd);InvalidateRect(hwnd,NULL,FALSE);return 0;}
    if(message==WM_LBUTTONDBLCLK||(message==WM_KEYDOWN&&(wp==VK_RETURN||wp==VK_SPACE))){ShellOpenPath(desktop,item->path);return 0;}
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS){InvalidateRect(hwnd,NULL,FALSE);return 0;}
    if(message==WM_MOUSEWHEEL){PostMessageW(desktop,WM_MOUSEWHEEL,wp,lp);return 0;}
    if(message==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(dc){RECT rect;GetClientRect(hwnd,&rect);
        BOOL focused=GetFocus()==hwnd;ShellPaintFill(dc,&rect,focused?RGB(38,80,119):RGB(22,48,74));
        ShellPaintIcon(dc,(rect.right-42)/2,7,42,!!(item->attributes&FILE_ATTRIBUTE_DIRECTORY),!ShellCompare(ShellExtension(item->path),L".exe"),item->shortcut);
        RECT text={3,55,rect.right-3,rect.bottom-3};paint_text(dc,item->name,text,RGB(248,250,253),DT_CENTER|DT_WORDBREAK|DT_END_ELLIPSIS);EndPaint(hwnd,&ps);}return 0;}
    return DefWindowProcW(hwnd,message,wp,lp);
}
static BOOL add_desktop_item(DESKTOP_ITEM ***items,SIZE_T *count,SIZE_T *capacity,const WCHAR *name,const WCHAR *path,DWORD attributes)
{
    if(!ShellReserve((void **)items,capacity,*count+1,sizeof(**items)))return FALSE;
    DESKTOP_ITEM *item=ShellAlloc(sizeof(*item));if(!item)return FALSE;
    ShellCopy(item->name,SHELL_PATH_CAPACITY,name);ShellCopy(item->path,SHELL_PATH_CAPACITY,path);item->attributes=attributes;
    item->shortcut=!ShellCompare(ShellExtension(path),L".url");
    if(item->shortcut){SIZE_T n=ShellLength(item->name);if(n>4)item->name[n-4]=0;}
    (*items)[(*count)++]=item;return TRUE;
}
static void free_desktop_items(DESKTOP_ITEM **items,SIZE_T count)
{for(SIZE_T i=0;i<count;++i){if(items[i]->window)DestroyWindow(items[i]->window);ShellFree(items[i]);}ShellFree(items);}
static void desktop_scan(void)
{
    DESKTOP_ITEM **fresh=NULL;SIZE_T count=0,capacity=0;
    if(!add_desktop_item(&fresh,&count,&capacity,L"Computer",L"C:\\",FILE_ATTRIBUTE_DIRECTORY))goto failed;
    DWORD attributes=GetFileAttributesW(ShellDocumentsPath);
    if(attributes!=INVALID_FILE_ATTRIBUTES&&!add_desktop_item(&fresh,&count,&capacity,L"Documents",ShellDocumentsPath,attributes))goto failed;
    WCHAR pattern[SHELL_PATH_CAPACITY];SHELL_FIND_DATA data;
    if(!ShellJoin(pattern,SHELL_PATH_CAPACITY,ShellDesktopPath,L"*"))goto failed;
    HANDLE find=FindFirstFileW(pattern,&data);
    if(find!=INVALID_HANDLE_VALUE) {
        do {
            if(data.name[0]=='.'&&(!data.name[1]||(data.name[1]=='.'&&!data.name[2])))continue;
            if(data.attributes&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM))continue;
            WCHAR path[SHELL_PATH_CAPACITY];if(!ShellJoin(path,SHELL_PATH_CAPACITY,ShellDesktopPath,data.name))continue;
            if(!add_desktop_item(&fresh,&count,&capacity,data.name,path,data.attributes)){FindClose(find);goto failed;}
        }while(FindNextFileW(find,&data));FindClose(find);
    }
    BOOL same=count==desktop_count;
    for(SIZE_T i=0;same&&i<count;++i)same=!ShellCompare(fresh[i]->path,desktop_items[i]->path)&&!ShellCompare(fresh[i]->name,desktop_items[i]->name)&&fresh[i]->attributes==desktop_items[i]->attributes;
    if(same){free_desktop_items(fresh,count);desktop_dirty=FALSE;return;}
    /* Create all replacement HWNDs before publishing the new view. A failure
     * leaves the old desktop intact and cleans up only the new controls. */
    for(SIZE_T i=0;i<count;++i) {
        fresh[i]->window=CreateWindowExW(0,L"SvDesktopIcon",fresh[i]->name,WS_CHILD|WS_TABSTOP,
            0,0,104,98,desktop,NULL,ShellInstance,fresh[i]);
        if(!fresh[i]->window)goto failed;
    }
    free_desktop_items(desktop_items,desktop_count);desktop_items=fresh;desktop_count=count;desktop_capacity=capacity;
    desktop_dirty=FALSE;desktop_layout();InvalidateRect(desktop,NULL,FALSE);return;
failed:
    free_desktop_items(fresh,count);ShellLog("[explorer] desktop refresh failed\r\n");
}
static void desktop_layout(void)
{
    if(!desktop)return;RECT rect;GetClientRect(desktop,&rect);
    int columns=(rect.right-32)/112;if(columns<1)columns=1;
    int visible_rows=(rect.bottom-16)/106;if(visible_rows<1)visible_rows=1;
    int rows=(int)((desktop_count+(SIZE_T)columns-1)/(SIZE_T)columns),max=rows-visible_rows;if(max<0)max=0;
    if(desktop_row>max)desktop_row=max;if(desktop_row<0)desktop_row=0;
    for(SIZE_T i=0;i<desktop_count;++i){MoveWindow(desktop_items[i]->window,12+(int)(i%columns)*112,12+((int)(i/columns)-desktop_row)*106,104,98,TRUE);ShowWindow(desktop_items[i]->window,SW_SHOW);}
    MoveWindow(desktop_scroll,rect.right-18,0,18,rect.bottom,TRUE);
    SHELL_SCROLL_INFO info={sizeof(info),0x1|0x2|0x4,0,rows>0?rows-1:0,(UINT)visible_rows,desktop_row,0};SetScrollInfo(desktop_scroll,2,&info,TRUE);
    ShowWindow(desktop_scroll,max>0?SW_SHOW:SW_HIDE);
}
void ShellRefreshDesktop(void)
{desktop_dirty=TRUE;if(desktop)PostMessageW(desktop,WM_DESKTOP_REFRESH,0,0);}
static void desktop_scroll_by(int rows)
{desktop_row+=rows;desktop_layout();}
static LRESULT desktop_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    if(message==WM_CREATE) {
        desktop=hwnd;desktop_scroll=CreateWindowExW(0,L"SCROLLBAR",L"",WS_CHILD|1,0,0,18,1,hwnd,(HMENU)(UINT_PTR)ID_DESKTOP_SCROLL,ShellInstance,NULL);
        if(!desktop_scroll)return -1;SetTimer(hwnd,1,2000,NULL);return 0;
    }
    if(message==WM_TIMER||message==WM_DESKTOP_REFRESH){desktop_scan();return 0;}
    if(message==WM_SIZE){desktop_layout();return 0;}
    if(message==WM_DISPLAYCHANGE){int width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);RECT work={0,0,width,height-44};
        SystemParametersInfoW(SPI_SETWORKAREA,0,&work,SPIF_SENDCHANGE);MoveWindow(desktop,0,0,width,height-44,TRUE);SetWindowPos(taskbar,HWND_TOPMOST,0,height-44,width,44,SWP_NOACTIVATE);desktop_layout();return 0;}
    if(message==WM_VSCROLL){UINT code=LOWORD(wp);if(code==0)desktop_scroll_by(-1);else if(code==1)desktop_scroll_by(1);
        else if(code==2||code==3){RECT rect;GetClientRect(hwnd,&rect);desktop_scroll_by((code==2?-1:1)*(rect.bottom/106));}
        else if(code==4||code==5){SHELL_SCROLL_INFO info={sizeof(info),0x10,0,0,0,0,0};if(GetScrollInfo(desktop_scroll,2,&info))desktop_row=info.track;else desktop_row=HIWORD(wp);desktop_layout();}
        else if(code==6){desktop_row=0;desktop_layout();}else if(code==7){desktop_row=0x7fffffff;desktop_layout();}return 0;}
    if(message==WM_MOUSEWHEEL){desktop_scroll_by(-((short)HIWORD(wp)/120)*3);return 0;}
    if(message==WM_CONTEXTMENU){HMENU menu=CreatePopupMenu();if(menu){AppendMenuW(menu,MF_STRING,ID_COMPUTER,L"Open Computer");AppendMenuW(menu,MF_STRING,ID_DOCUMENTS,L"Open Documents");AppendMenuW(menu,MF_STRING,ID_RESCAN,L"Refresh");AppendMenuW(menu,MF_STRING,ID_RUN,L"Run…");
        UINT id=(UINT)TrackPopupMenu(menu,TPM_RETURNCMD,(short)LOWORD(lp),(short)HIWORD(lp),0,hwnd,NULL);DestroyMenu(menu);
        if(id==ID_COMPUTER)ShellOpenFolder(L"C:\\");else if(id==ID_DOCUMENTS)ShellOpenFolder(ShellDocumentsPath);else if(id==ID_RESCAN)desktop_scan();else if(id==ID_RUN)run_dialog();}return 0;}
    if(message==WM_CLOSE){if(ShellConfirm(hwnd,L"End desktop session",L"Close the desktop shell? Running applications remain open.")){closing_shell=TRUE;DestroyWindow(hwnd);}return 0;}
    if(message==WM_DESTROY){KillTimer(hwnd,1);desktop=NULL;PostQuitMessage(0);return 0;}
    if(message==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(dc){RECT rect;GetClientRect(hwnd,&rect);ShellPaintFill(dc,&rect,RGB(22,48,74));EndPaint(hwnd,&ps);}return 0;}
    return DefWindowProcW(hwnd,message,wp,lp);
}
static BOOL register_class(const WCHAR *name,WNDPROC proc,UINT style)
{
    WNDCLASSEXW cls;ShellZero(&cls,sizeof(cls));cls.cbSize=sizeof(cls);cls.style=style;cls.lpfnWndProc=proc;
    cls.hInstance=ShellInstance;cls.hCursor=LoadCursorW(NULL,IDC_ARROW);cls.lpszClassName=name;return RegisterClassExW(&cls)!=0;
}
static BOOL shell_keyboard(MSG *message)
{
    if(message->message==WM_MOUSEWHEEL){HWND root=GetAncestor(message->hwnd,GA_ROOT);if(root==desktop){desktop_scroll_by(-((short)HIWORD(message->wParam)/120)*3);return TRUE;}}
    if(message->message!=WM_KEYDOWN&&message->message!=WM_SYSKEYDOWN)return FALSE;
    if((GetKeyState(VK_CONTROL)&0x8000)&&message->wParam==VK_ESCAPE){start_show(!start_panel||!IsWindowVisible(start_panel));return TRUE;}
    HWND root=GetAncestor(message->hwnd,GA_ROOT);
    if(root==start_panel){if(message->wParam==VK_ESCAPE){start_show(FALSE);return TRUE;}
        if(message->wParam==VK_RETURN){start_launch();return TRUE;}
        if(message->wParam==VK_TAB){HWND controls[]={search_control,applications_control,run_button,computer_button,documents_button,rescan_button};int i=0;for(int j=0;j<6;++j)if(controls[j]==GetFocus())i=j;
            i=(i+((GetKeyState(VK_SHIFT)&0x8000)?5:1))%6;SetFocus(controls[i]);return TRUE;}
        if(message->hwnd==search_control&&message->wParam==VK_DOWN){SetFocus(applications_control);return TRUE;}}
    if(root==desktop&&desktop_count){SIZE_T selected=0;for(SIZE_T i=0;i<desktop_count;++i)if(desktop_items[i]->window==message->hwnd)selected=i;
        RECT rect;GetClientRect(desktop,&rect);int columns=(rect.right-32)/112;if(columns<1)columns=1;INT_PTR delta=0;
        if(message->wParam==VK_TAB)delta=(GetKeyState(VK_SHIFT)&0x8000)?-1:1;else if(message->wParam==VK_LEFT)delta=-1;else if(message->wParam==VK_RIGHT)delta=1;else if(message->wParam==VK_UP)delta=-columns;else if(message->wParam==VK_DOWN)delta=columns;
        else if(message->wParam==VK_F5){desktop_scan();return TRUE;}
        if(delta){INT_PTR next=(INT_PTR)selected+delta;if(next<0)next=0;if((SIZE_T)next>=desktop_count)next=(INT_PTR)desktop_count-1;
            int row=(int)((SIZE_T)next/(SIZE_T)columns),visible=rect.bottom/106;if(visible<1)visible=1;if(row<desktop_row)desktop_row=row;else if(row>=desktop_row+visible)desktop_row=row-visible+1;
            desktop_layout();SetFocus(desktop_items[next]->window);return TRUE;}}
    return FALSE;
}
static void initialize_paths(void)
{
    WCHAR profile[SHELL_PATH_CAPACITY];DWORD n=GetEnvironmentVariableW(L"USERPROFILE",profile,SHELL_PATH_CAPACITY);
    if(!n||n>=SHELL_PATH_CAPACITY)ShellCopy(profile,SHELL_PATH_CAPACITY,L"C:\\Users\\Default");
    ShellJoin(ShellDesktopPath,SHELL_PATH_CAPACITY,profile,L"Desktop");ShellJoin(ShellDocumentsPath,SHELL_PATH_CAPACITY,profile,L"Documents");
    ShellCopy(ShellExecutablePath,SHELL_PATH_CAPACITY,L"C:\\WINDOWS\\EXPLORER.EXE");
}
int wWinMain(HINSTANCE instance,HINSTANCE previous,WCHAR *command,int show)
{
    (void)previous;(void)show;ShellInstance=instance;initialize_paths();ShellRegisterDialogClass();
    INITCOMMONCONTROLSEX common={sizeof(common),ICC_LISTVIEW_CLASSES|ICC_BAR_CLASSES};
    if(!InitCommonControlsEx(&common)||!ShellRegisterBrowserClass())return 1;
    if(!register_class(L"Progman",desktop_proc,0)||!register_class(L"Shell_TrayWnd",taskbar_proc,0)||
        !register_class(L"SvDesktopIcon",desktop_icon_proc,CS_DBLCLKS)||!register_class(L"SvTaskButton",task_button_proc,0)||!register_class(L"SvStartPanel",start_proc,0))return 2;
    (void)command;
    int argc=0;WCHAR **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return 3;
    if(argc>1){int index=!ShellCompare(argv[1],L"/folder")?2:1;
        if(index>=argc){LocalFree(argv);return 3;}
        WCHAR path[SHELL_PATH_CAPACITY];BOOL copied=ShellCopy(path,SHELL_PATH_CAPACITY,argv[index]);
        const WCHAR *ready_name=NULL;const WCHAR *const *selection=NULL;SIZE_T selected=0;BOOL edit=FALSE;
        for(int option=index+1;option<argc;) {
            if(!ShellCompare(argv[option],L"/ready-event")) {
                if(++option>=argc){LocalFree(argv);return 87;}ready_name=argv[option++];
            } else if(!ShellCompare(argv[option],L"/edit")){edit=TRUE;option++;}
            else if(!ShellCompare(argv[option],L"/select")) {
                if(selection){LocalFree(argv);return 87;}option++;selection=(const WCHAR *const *)(argv+option);
                while(option<argc&&ShellCompare(argv[option],L"/ready-event")&&ShellCompare(argv[option],L"/edit")){selected++;option++;}
            } else {LocalFree(argv);return 87;}
        }
        ShellStandalone=TRUE;HWND folder=copied?ShellOpenFolder(path):NULL;
        if(!folder){LocalFree(argv);return 3;}
        if(selection&&!ShellSelectFolderItems(folder,selection,selected,edit)) {
            DWORD error=GetLastError();DestroyWindow(folder);LocalFree(argv);return (int)(error?error:31);
        }
        if(ready_name) {
            HANDLE completed=OpenEventW(2,FALSE,ready_name);
            if(!completed){DWORD error=GetLastError();DestroyWindow(folder);LocalFree(argv);return (int)error;}
            BOOL signaled=SetEvent(completed);DWORD error=GetLastError();CloseHandle(completed);
            if(!signaled){DestroyWindow(folder);LocalFree(argv);return (int)error;}
        }
        LocalFree(argv);
        MSG message;while(GetMessageW(&message,NULL,0,0)>0){if(ShellProcessDialogMessage(&message)||ShellBrowserMessage(&message))continue;TranslateMessage(&message);DispatchMessageW(&message);}return (int)message.wParam;
    }
    LocalFree(argv);
    int width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);if(width<320||height<240)return 4;
    initial_work_area=(RECT){0,0,width,height};SystemParametersInfoW(SPI_GETWORKAREA,0,&initial_work_area,0);
    shell_message=RegisterWindowMessageW(L"SHELLHOOK");
    desktop=CreateWindowExW(0,L"Progman",L"Svindows Desktop",WS_POPUP|WS_VISIBLE|WS_CLIPCHILDREN,0,0,width,height-44,NULL,NULL,instance,NULL);
    if(!desktop||!SetShellWindow(desktop)){if(desktop)DestroyWindow(desktop);return 5;}
    taskbar=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE,L"Shell_TrayWnd",L"Taskbar",WS_POPUP|WS_VISIBLE|WS_CLIPCHILDREN,
        0,height-44,width,44,NULL,NULL,instance,NULL);
    if(!taskbar){DestroyWindow(desktop);return 6;}
    RECT work={0,0,width,height-44};if(!SystemParametersInfoW(SPI_SETWORKAREA,0,&work,SPIF_SENDCHANGE))return 7;
    SetWindowPos(desktop,HWND_BOTTOM,0,0,width,height-44,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    SetWindowPos(taskbar,HWND_TOPMOST,0,height-44,width,44,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    desktop_scan();task_refresh();UpdateWindow(desktop);UpdateWindow(taskbar);
    if (!RegisterHotKey(desktop, 1, 2u | 0x4000u, VK_ESCAPE) ||
        !RegisterHotKey(desktop, 2, 8u | 0x4000u, 'R')) return 8;
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, L"Global\\Svindows.Session.1.ShellReady");
    if (!ready) return 9;
    BOOL signaled = SetEvent(ready); CloseHandle(ready);
    if (!signaled) return 9;
    ShellLog("[explorer] desktop and taskbar ready\r\n");
    MSG message;BOOL result;
    while((result=GetMessageW(&message,NULL,0,0))>0){
        if (message.message == 0x0312) {
            if (message.wParam == 1) start_show(!start_panel || !IsWindowVisible(start_panel));
            else if (message.wParam == 2) run_dialog();
            continue;
        }
        if (message.message == WM_SYSCOMMAND && (message.wParam & 0xfff0u) == 0xf130u) {
            start_show(!start_panel || !IsWindowVisible(start_panel)); continue;
        }
        if(ShellProcessDialogMessage(&message)||ShellBrowserMessage(&message)||shell_keyboard(&message))continue;
        TranslateMessage(&message);DispatchMessageW(&message);
        if(refresh_tasks_pending)task_refresh();
    }
    closing_shell=TRUE;if(start_panel)DestroyWindow(start_panel);if(taskbar)DestroyWindow(taskbar);
    UnregisterHotKey(desktop, 1); UnregisterHotKey(desktop, 2);
    free_desktop_items(desktop_items,desktop_count);desktop_items=NULL;desktop_count=0;
    for(SIZE_T i=0;i<task_count;++i)ShellFree(tasks[i]);ShellFree(tasks);ShellFree(applications);
    SystemParametersInfoW(SPI_SETWORKAREA,0,&initial_work_area,SPIF_SENDCHANGE);
    return result<0?8:(int)message.wParam;
}
void WinMainCRTStartup(void)
{
    WCHAR *line=GetCommandLineW();const WCHAR *p=line;BOOL quoted=FALSE;
    while(p&&*p){if(*p=='"')quoted=!quoted;else if(!quoted&&(*p==' '||*p=='\t'))break;++p;}
    while(p&&(*p==' '||*p=='\t'))++p;
    ExitProcess((DWORD)wWinMain(GetModuleHandleW(NULL),NULL,(WCHAR *)p,SW_SHOWDEFAULT));
}
