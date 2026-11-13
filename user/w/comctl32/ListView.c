#include "Internal.h"

enum { LV_HEADER_HEIGHT=26, LV_ROW_HEIGHT=24 };
typedef struct { UINT size,mask;int minimum,maximum;UINT page;int position,track; } LV_SCROLL_INFO;
__declspec(dllimport) int SetScrollInfo(HWND,int,const LV_SCROLL_INFO *,BOOL);
__declspec(dllimport) BOOL GetScrollInfo(HWND,int,LV_SCROLL_INFO *);
__declspec(dllimport) BOOL IsWindow(HWND);
__declspec(dllimport) LONG_PTR SetWindowLongPtrW(HWND,int,LONG_PTR);
__declspec(dllimport) LRESULT CallWindowProcW(WNDPROC,HWND,UINT,WPARAM,LPARAM);
typedef struct LV_COLUMN { WCHAR *text; int width,format,subitem; } LV_COLUMN;
typedef struct LV_ROW { WCHAR **cells; int cell_count; UINT state; LPARAM param; ULONG_PTR identity; } LV_ROW;
typedef struct LISTVIEW_STATE {
    struct LISTVIEW_STATE *next;
    HWND hwnd,parent;
    UINT id;
    DWORD style,extended_style;
    LV_COLUMN *columns; int column_count,column_capacity;
    LV_ROW *rows; int row_count,row_capacity,first_visible;
    HWND vertical,horizontal;
    HWND editor;
    WNDPROC editor_proc;
    ULONG_PTR editor_identity;
    BOOL editor_ending;
    int horizontal_offset,wheel_remainder,view_width,view_height;
    int selection_anchor;
    ULONG_PTR next_identity;
    unsigned references;
    BOOL destroyed;
} LISTVIEW_STATE;
static LISTVIEW_STATE *states;
typedef struct {NMHDR hdr;LVITEMW item;} LV_LABEL_NOTICE;
static LRESULT listview_editor_proc(HWND,UINT,WPARAM,LPARAM);
static void ensure_visible(LISTVIEW_STATE *,int);
static unsigned state_lock;
static void lock_states(void){while(__atomic_exchange_n(&state_lock,1,__ATOMIC_ACQUIRE)){}}
static void unlock_states(void){__atomic_store_n(&state_lock,0,__ATOMIC_RELEASE);}
static LISTVIEW_STATE *find_state(HWND hwnd)
{
    lock_states();LISTVIEW_STATE *found=NULL;
    for(LISTVIEW_STATE *s=states;s;s=s->next)if(s->hwnd==hwnd&&!s->destroyed){found=s;++s->references;break;}
    unlock_states();return found;
}
static WCHAR *copy_text(const WCHAR *src)
{
    ULONG_PTR n=0;while(src&&src[n]){if(n>=0x7fffffffu/sizeof(WCHAR)-1)return NULL;++n;}
    WCHAR *dst=SvComctlAlloc((n+1)*sizeof(WCHAR));if(!dst)return NULL;
    for(ULONG_PTR i=0;i<n;++i)dst[i]=src[i];dst[n]=0;return dst;
}
static void free_row(LV_ROW *row)
{for(int i=0;i<row->cell_count;++i)SvComctlFree(row->cells[i]);SvComctlFree(row->cells);}
static void release_state(LISTVIEW_STATE *s)
{
    lock_states();BOOL dispose=!--s->references;unlock_states();
    if(!dispose)return;
    for(int i=0;i<s->row_count;++i)free_row(s->rows+i);
    for(int i=0;i<s->column_count;++i)SvComctlFree(s->columns[i].text);
    SvComctlFree(s->rows);SvComctlFree(s->columns);SvComctlFree(s);
}
static int clamp_coordinate(long long value)
{return value>0x7fffffff?0x7fffffff:value<(-0x7fffffff-1)?(-0x7fffffff-1):(int)value;}
static int content_width(LISTVIEW_STATE *s)
{long long width=0;for(int i=0;i<s->column_count;++i)width+=s->columns[i].width;return clamp_coordinate(width);}
static int rows_per_page(LISTVIEW_STATE *s)
{int rows=(s->view_height-LV_HEADER_HEIGHT)/LV_ROW_HEIGHT;return rows<1?1:rows;}
static void update_scrollbars(LISTVIEW_STATE *s)
{
    if(s->destroyed)return;
    RECT rect;if(!GetClientRect(s->hwnd,&rect))return;
    int width=rect.right-rect.left,height=rect.bottom-rect.top;
    BOOL vertical=FALSE,horizontal=FALSE;
    int content=content_width(s);
    for(int i=0;i<3;++i){
        int available_width=width-(vertical?17:0),available_height=height-(horizontal?17:0)-LV_HEADER_HEIGHT;
        horizontal=content>available_width;
        vertical=(long long)s->row_count*LV_ROW_HEIGHT>available_height;
    }
    s->view_width=width-(vertical?17:0);s->view_height=height-(horizontal?17:0);
    if(s->view_width<0)s->view_width=0;if(s->view_height<0)s->view_height=0;
    int last=s->row_count-rows_per_page(s);if(last<0)last=0;
    if(s->first_visible>last)s->first_visible=last;if(s->first_visible<0)s->first_visible=0;
    int right=content-s->view_width;if(right<0)right=0;
    if(s->horizontal_offset>right)s->horizontal_offset=right;if(s->horizontal_offset<0)s->horizontal_offset=0;
    if(s->vertical){
        MoveWindow(s->vertical,s->view_width,0,17,s->view_height,TRUE);
        LV_SCROLL_INFO info={sizeof(info),7,0,s->row_count?s->row_count-1:0,(UINT)rows_per_page(s),s->first_visible,0};
        SetScrollInfo(s->vertical,2,&info,TRUE);ShowWindow(s->vertical,vertical?SW_SHOW:SW_HIDE);
    }
    if(s->horizontal){
        MoveWindow(s->horizontal,0,s->view_height,s->view_width,17,TRUE);
        LV_SCROLL_INFO info={sizeof(info),7,0,content?content-1:0,(UINT)s->view_width,s->horizontal_offset,0};
        SetScrollInfo(s->horizontal,2,&info,TRUE);ShowWindow(s->horizontal,horizontal?SW_SHOW:SW_HIDE);
    }
}
static void scroll_rows(LISTVIEW_STATE *s,long long delta)
{s->first_visible=clamp_coordinate((long long)s->first_visible+delta);update_scrollbars(s);InvalidateRect(s->hwnd,NULL,FALSE);}
static BOOL ensure_columns(LISTVIEW_STATE *s,int wanted)
{
    if(wanted<=s->column_capacity)return TRUE;
    int cap=s->column_capacity?s->column_capacity:4;while(cap<wanted){if(cap>0x3fffffff/2)return FALSE;cap*=2;}
    LV_COLUMN *fresh=SvComctlAlloc((ULONG_PTR)cap*sizeof(*fresh));if(!fresh)return FALSE;
    for(int i=0;i<s->column_count;++i)fresh[i]=s->columns[i];
    SvComctlFree(s->columns);s->columns=fresh;s->column_capacity=cap;return TRUE;
}
static BOOL ensure_rows(LISTVIEW_STATE *s,int wanted)
{
    if(wanted<=s->row_capacity)return TRUE;
    int cap=s->row_capacity?s->row_capacity:16;while(cap<wanted){if(cap>0x3fffffff/2)return FALSE;cap*=2;}
    LV_ROW *fresh=SvComctlAlloc((ULONG_PTR)cap*sizeof(*fresh));if(!fresh)return FALSE;
    for(int i=0;i<s->row_count;++i)fresh[i]=s->rows[i];
    SvComctlFree(s->rows);s->rows=fresh;s->row_capacity=cap;return TRUE;
}
static BOOL set_cell(LV_ROW *row,int subitem,const WCHAR *text)
{
    if(subitem<0||subitem>0x3fffffff||text==(const WCHAR *)(LONG_PTR)-1)return FALSE;
    if(subitem>=row->cell_count){
        int cap=row->cell_count?row->cell_count:1;while(cap<=subitem){if(cap>0x3fffffff/2)return FALSE;cap*=2;}
        WCHAR **fresh=SvComctlAlloc((ULONG_PTR)cap*sizeof(*fresh));if(!fresh)return FALSE;
        for(int i=0;i<row->cell_count;++i)fresh[i]=row->cells[i];
        for(int i=row->cell_count;i<cap;++i)fresh[i]=NULL;
        SvComctlFree(row->cells);row->cells=fresh;row->cell_count=cap;
    }
    WCHAR *copy=copy_text(text);if(!copy)return FALSE;
    SvComctlFree(row->cells[subitem]);row->cells[subitem]=copy;return TRUE;
}
static int text_copy(WCHAR *out,int capacity,const WCHAR *text)
{
    if(!out||capacity<=0)return 0;
    int n=0;while(text&&text[n]&&n<capacity-1){out[n]=text[n];++n;}out[n]=0;return n;
}
static int column_x(LISTVIEW_STATE *s,int index)
{long long x=-(long long)s->horizontal_offset;for(int i=0;i<index&&i<s->column_count;++i)x+=s->columns[i].width;return clamp_coordinate(x);}
static RECT cell_rect(LISTVIEW_STATE *s,int row,int col)
{
    int x=column_x(s,col);
    return (RECT){x,clamp_coordinate(LV_HEADER_HEIGHT+((long long)row-s->first_visible)*LV_ROW_HEIGHT),
                  clamp_coordinate((long long)x+s->columns[col].width),clamp_coordinate(LV_HEADER_HEIGHT+((long long)row-s->first_visible+1)*LV_ROW_HEIGHT)};
}
static UINT column_text_flags(const LV_COLUMN *column)
{
    UINT flags = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;
    switch (column->format & LVCFMT_JUSTIFYMASK) {
    case LVCFMT_RIGHT: return flags | DT_RIGHT;
    case LVCFMT_CENTER: return flags | DT_CENTER;
    default: return flags | DT_LEFT;
    }
}

static int column_text_padding(const LV_COLUMN *column)
{
    return column->width < 48 ? 3 : 6;
}

static void draw_listview(HWND hwnd,LISTVIEW_STATE *s)
{
    PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(!dc)return;
    RECT client;if(!GetClientRect(hwnd,&client)){EndPaint(hwnd,&ps);return;}client.right=s->view_width;client.bottom=s->view_height;SvComctlFill(dc,&client,RGB(255,255,255));
    int x=-s->horizontal_offset;
    for(int c=0;c<s->column_count;++c){
        int right=clamp_coordinate((long long)x+s->columns[c].width);
        RECT head={x,0,right,LV_HEADER_HEIGHT};
        SvComctlFill(dc,&head,RGB(243,243,243));
        int pad = column_text_padding(&s->columns[c]);
        RECT label={clamp_coordinate((long long)x+pad),2,clamp_coordinate((long long)right-pad),LV_HEADER_HEIGHT-2};
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,0,0));
        DrawTextW(dc,s->columns[c].text?s->columns[c].text:L"",-1,&label,column_text_flags(&s->columns[c]));
        HPEN edge=CreatePen(0,1,RGB(210,210,210));HGDIOBJ old=edge?SelectObject(dc,edge):NULL;
        MoveToEx(dc,right-1,0,NULL);LineTo(dc,right-1,client.bottom);
        if(edge){SelectObject(dc,old);DeleteObject(edge);}x=right;
    }
    HPEN line=CreatePen(0,1,RGB(224,224,224));HGDIOBJ old_line=line?SelectObject(dc,line):NULL;
    MoveToEx(dc,0,LV_HEADER_HEIGHT,NULL);LineTo(dc,client.right,LV_HEADER_HEIGHT);
    for(int row=s->first_visible;row<s->row_count;++row){
        int y=LV_HEADER_HEIGHT+(row-s->first_visible)*LV_ROW_HEIGHT;if(y>=client.bottom)break;
        RECT rr={0,y,client.right,y+LV_ROW_HEIGHT};
        if(s->rows[row].state&LVIS_SELECTED)SvComctlFill(dc,&rr,RGB(204,232,255));
        else if(s->rows[row].state&LVIS_FOCUSED)SvComctlFill(dc,&rr,RGB(240,240,240));
        MoveToEx(dc,0,y+LV_ROW_HEIGHT-1,NULL);LineTo(dc,client.right,y+LV_ROW_HEIGHT-1);
        for(int col=0;col<s->column_count;++col){
            int pad = column_text_padding(&s->columns[col]);
            RECT r=cell_rect(s,row,col);r.left=clamp_coordinate((long long)r.left+pad);r.right=clamp_coordinate((long long)r.right-pad);r.top=clamp_coordinate((long long)r.top+1);r.bottom=clamp_coordinate((long long)r.bottom-1);
            WCHAR *text=col<s->rows[row].cell_count?s->rows[row].cells[col]:NULL;
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,0,0));
            DrawTextW(dc,text?text:L"",-1,&r,column_text_flags(&s->columns[col]));
        }
    }
    if(line){SelectObject(dc,old_line);DeleteObject(line);}EndPaint(hwnd,&ps);
}
static int hit_row(LISTVIEW_STATE *s,int y)
{if(y<LV_HEADER_HEIGHT)return -1;int row=s->first_visible+(y-LV_HEADER_HEIGHT)/LV_ROW_HEIGHT;return row>=0&&row<s->row_count?row:-1;}
static int hit_column(LISTVIEW_STATE *s,int x)
{for(int c=0;c<s->column_count;++c){int left=column_x(s,c);if(x>=left&&(long long)x<(long long)left+s->columns[c].width)return c;}return -1;}
static void make_change(NMLISTVIEW *note,HWND from,UINT id,int item,UINT before,UINT after,LPARAM param)
{
    note->hdr.hwndFrom=from;note->hdr.idFrom=id;note->hdr.code=LVN_ITEMCHANGED;
    note->iItem=item;note->iSubItem=0;note->uOldState=before;note->uNewState=after;
    note->uChanged=LVIF_STATE;note->ptAction.x=-1;note->ptAction.y=-1;note->lParam=param;
}
static int find_identity(LISTVIEW_STATE *s,ULONG_PTR identity)
{for(int i=0;i<s->row_count;++i)if(s->rows[i].identity==identity)return i;return -1;}
static int selection_focus(LISTVIEW_STATE *s)
{for(int i=0;i<s->row_count;++i)if(s->rows[i].state&LVIS_FOCUSED)return i;return -1;}
static BOOL change_row_state(LISTVIEW_STATE *s,ULONG_PTR identity,UINT state,UINT mask)
{
    int index=find_identity(s,identity);if(index<0||s->destroyed)return FALSE;
    UINT before=s->rows[index].state,after=(before&~mask)|(state&mask);
    if(before==after)return TRUE;
    HWND parent=s->parent,from=s->hwnd;UINT id=s->id;NMLISTVIEW note;
    make_change(&note,from,id,index,before,after,s->rows[index].param);
    note.hdr.code=LVN_ITEMCHANGING;
    if(SendMessageW(parent,WM_NOTIFY,(WPARAM)id,(LPARAM)&note)||s->destroyed)return FALSE;
    index=find_identity(s,identity);if(index<0)return FALSE;
    before=s->rows[index].state;after=(before&~mask)|(state&mask);
    if(before==after)return TRUE;
    for(int other=0;other<s->row_count;++other)if(other!=index) {
        if((after&LVIS_FOCUSED)&&(s->rows[other].state&LVIS_FOCUSED))return FALSE;
        if((s->style&LVS_SINGLESEL)&&(after&LVIS_SELECTED)&&(s->rows[other].state&LVIS_SELECTED))return FALSE;
    }
    s->rows[index].state=after;make_change(&note,from,id,index,before,after,s->rows[index].param);
    InvalidateRect(from,NULL,FALSE);
    SendMessageW(parent,WM_NOTIFY,(WPARAM)id,(LPARAM)&note);
    return !s->destroyed;
}
static ULONG_PTR *selection_snapshot(LISTVIEW_STATE *s,int *count)
{
    *count=s->row_count;
    if(!*count)return NULL;
    ULONG_PTR *identities=SvComctlAlloc((ULONG_PTR)*count*sizeof(*identities));
    if(!identities)return NULL;
    for(int i=0;i<*count;++i)identities[i]=s->rows[i].identity;
    return identities;
}
static BOOL set_item_states(LISTVIEW_STATE *s,int first,int last,UINT state,UINT mask)
{
    int count;ULONG_PTR *identities=selection_snapshot(s,&count);
    if(count&&!identities)return FALSE;
    BOOL result=TRUE;
    BOOL single=(s->style&LVS_SINGLESEL)!=0;
    for(int i=0;i<count&&!s->destroyed;++i)if(i!=first) {
        UINT clear=0;
        if((mask&LVIS_FOCUSED)&&(state&LVIS_FOCUSED))clear|=LVIS_FOCUSED;
        if(single&&(mask&LVIS_SELECTED)&&(state&LVIS_SELECTED))clear|=LVIS_SELECTED;
        if(clear&&!change_row_state(s,identities[i],0,clear))result=FALSE;
    }
    for(int i=0;i<count&&!s->destroyed;++i) {
        UINT changed=0,value=state;
        if(i>=first&&i<=last)changed=mask;
        if((mask&LVIS_FOCUSED)&&(state&LVIS_FOCUSED)&&i!=first){changed|=LVIS_FOCUSED;value&=~LVIS_FOCUSED;}
        if(single&&(mask&LVIS_SELECTED)&&(state&LVIS_SELECTED)&&i!=first){changed|=LVIS_SELECTED;value&=~LVIS_SELECTED;}
        if(changed&&!change_row_state(s,identities[i],value,changed))result=FALSE;
    }
    SvComctlFree(identities);return result&&!s->destroyed;
}
static void select_row(LISTVIEW_STATE *s,int index,UINT modifiers,BOOL focus_only)
{
    if(index<0||index>=s->row_count)return;
    int focused=selection_focus(s),anchor=s->selection_anchor;
    BOOL shift=(modifiers&4)!=0,control=(modifiers&8)!=0,single=(s->style&LVS_SINGLESEL)!=0;
    if(single){shift=control=focus_only=FALSE;}
    if(anchor<0||anchor>=s->row_count)anchor=focused>=0?focused:index;
    int low=anchor<index?anchor:index,high=anchor>index?anchor:index;
    int count;ULONG_PTR *identities=selection_snapshot(s,&count);if(count&&!identities)return;
    for(int iteration=0;iteration<count&&!s->destroyed;++iteration) {
        int i=iteration==count-1?index:iteration<index?iteration:iteration+1;
        int current=find_identity(s,identities[i]);if(current<0)continue;
        UINT before=s->rows[current].state,state=before;
        state=(state&~LVIS_FOCUSED)|(i==index?LVIS_FOCUSED:0);
        if(!focus_only) {
            if(shift) {
                if(i>=low&&i<=high)state|=LVIS_SELECTED;
                else if(!control)state&=~LVIS_SELECTED;
            } else if(control) {if(i==index)state^=LVIS_SELECTED;}
            else state=(state&~LVIS_SELECTED)|(i==index?LVIS_SELECTED:0);
        }
        change_row_state(s,identities[i],state,LVIS_SELECTED|LVIS_FOCUSED);
    }
    if(!s->destroyed&&!shift&&!focus_only)s->selection_anchor=find_identity(s,identities[index]);
    SvComctlFree(identities);
}
static int insert_row(LISTVIEW_STATE *s,int index,const LVITEMW *item)
{
    if(!item||index<0||index>s->row_count||s->row_count==0x3fffffff||s->next_identity==(ULONG_PTR)-1||!ensure_rows(s,s->row_count+1))return -1;
    for(int i=s->row_count;i>index;--i)s->rows[i]=s->rows[i-1];
    LV_ROW *row=&s->rows[index];row->cells=NULL;row->cell_count=0;
    row->state=(item->mask&LVIF_STATE)?item->state&item->stateMask:0;
    row->param=(item->mask&LVIF_PARAM)?item->lParam:0;
    row->identity=++s->next_identity;
    if((item->mask&LVIF_TEXT)&&!set_cell(row,0,item->pszText)){
        free_row(row);
        for(int i=index;i<s->row_count;++i)s->rows[i]=s->rows[i+1];return -1;}
    ++s->row_count;if(s->selection_anchor>=index)s->selection_anchor++;return index;
}
static BOOL set_item(LISTVIEW_STATE *s,const LVITEMW *item)
{
    if(!item||item->iItem<0||item->iItem>=s->row_count||item->iSubItem<0)return FALSE;
    LV_ROW *row=&s->rows[item->iItem];
    if((item->mask&LVIF_TEXT)&&!set_cell(row,item->iSubItem,item->pszText))return FALSE;
    if(item->mask&LVIF_PARAM)row->param=item->lParam;
    if(item->mask&LVIF_STATE)return set_item_states(s,item->iItem,item->iItem,item->state,item->stateMask);
    InvalidateRect(s->hwnd,NULL,FALSE);return TRUE;
}
static BOOL get_item(LISTVIEW_STATE *s,LVITEMW *item)
{
    if(!item||item->iItem<0||item->iItem>=s->row_count||item->iSubItem<0)return FALSE;
    LV_ROW *row=&s->rows[item->iItem];
    if(item->mask&LVIF_TEXT)text_copy(item->pszText,item->cchTextMax,item->iSubItem<row->cell_count?row->cells[item->iSubItem]:NULL);
    if(item->mask&LVIF_PARAM)item->lParam=row->param;
    if(item->mask&LVIF_STATE)item->state=row->state&item->stateMask;
    return TRUE;
}
static BOOL end_label_edit(LISTVIEW_STATE *s,BOOL commit,BOOL focus_list)
{
    if(!s->editor || s->editor_ending)return FALSE;
    s->editor_ending=TRUE;HWND editor=s->editor;WNDPROC previous=s->editor_proc;
    ULONG_PTR identity=s->editor_identity;int index=find_identity(s,identity);
    WCHAR *text=NULL;
    if(commit&&index>=0) {
        int length=GetWindowTextLengthW(editor);
        if(length>=0&&length<0x3fffffff) {
            text=SvComctlAlloc(((ULONG_PTR)length+1)*sizeof(WCHAR));
            if(text)GetWindowTextW(editor,text,length+1);
        }
    }
    LV_LABEL_NOTICE note;for(unsigned i=0;i<sizeof(note);++i)((BYTE *)&note)[i]=0;
    note.hdr=(NMHDR){s->hwnd,s->id,LVN_ENDLABELEDITW};note.item.iItem=index;
    note.item.mask=LVIF_TEXT;note.item.pszText=text;
    BOOL accepted=SendMessageW(s->parent,WM_NOTIFY,s->id,(LPARAM)&note)!=0;
    index=find_identity(s,identity);
    if(accepted&&text&&index>=0&&!s->destroyed)accepted=set_cell(s->rows+index,0,text);
    else accepted=FALSE;
    SvComctlFree(text);s->editor=NULL;s->editor_identity=0;
    if(IsWindow(editor)) {
        SetWindowLongPtrW(editor,-4,(LONG_PTR)previous);
        DestroyWindow(editor);
    }
    s->editor_ending=FALSE;
    if(!s->destroyed){InvalidateRect(s->hwnd,NULL,FALSE);if(focus_list)SetFocus(s->hwnd);}
    return accepted;
}
static HWND begin_label_edit(LISTVIEW_STATE *s,int index)
{
    if(!(s->style&LVS_EDITLABELS)||index<0||index>=s->row_count||!s->column_count)return NULL;
    if(s->editor)end_label_edit(s,TRUE,FALSE);
    if(s->destroyed||index>=s->row_count)return NULL;
    ULONG_PTR identity=s->rows[index].identity;
    WCHAR *text=copy_text(s->rows[index].cell_count?s->rows[index].cells[0]:L"");if(!text)return NULL;
    LV_LABEL_NOTICE note;for(unsigned i=0;i<sizeof(note);++i)((BYTE *)&note)[i]=0;
    note.hdr=(NMHDR){s->hwnd,s->id,LVN_BEGINLABELEDITW};note.item.iItem=index;
    note.item.mask=LVIF_TEXT;note.item.pszText=text;
    if(SendMessageW(s->parent,WM_NOTIFY,s->id,(LPARAM)&note)||s->destroyed){SvComctlFree(text);return NULL;}
    index=find_identity(s,identity);if(index<0){SvComctlFree(text);return NULL;}
    ensure_visible(s,index);RECT rect=cell_rect(s,index,0);
    HWND editor=CreateWindowExW(0,L"EDIT",text,WS_CHILD|WS_VISIBLE|WS_BORDER|WS_TABSTOP|0x80,
        rect.left+2,rect.top+1,rect.right-rect.left-4,rect.bottom-rect.top-2,
        s->hwnd,(HMENU)(UINT_PTR)3,NULL,NULL);
    SvComctlFree(text);if(!editor)return NULL;
    s->editor=editor;s->editor_identity=identity;
    s->editor_proc=(WNDPROC)SetWindowLongPtrW(editor,-4,(LONG_PTR)listview_editor_proc);
    if(!s->editor_proc){s->editor=NULL;DestroyWindow(editor);return NULL;}
    SetFocus(editor);SendMessageW(editor,0x00b1 /* EM_SETSEL */,0,-1);return editor;
}
static LRESULT listview_editor_proc(HWND editor,UINT message,WPARAM wp,LPARAM lp)
{
    LISTVIEW_STATE *s=find_state(GetParent(editor));
    if(!s)return DefWindowProcW(editor,message,wp,lp);
    WNDPROC previous=s->editor_proc;LRESULT result;
    if(message==WM_KEYDOWN&&(wp==VK_RETURN||wp==VK_ESCAPE)) {
        end_label_edit(s,wp==VK_RETURN,TRUE);result=0;
    } else if(message==WM_KILLFOCUS&&!s->editor_ending) {
        end_label_edit(s,TRUE,FALSE);result=0;
    } else result=CallWindowProcW(previous,editor,message,wp,lp);
    release_state(s);return result;
}
static void ensure_visible(LISTVIEW_STATE *s,int row)
{
    if(s->destroyed)return;int visible=rows_per_page(s);
    if(visible<1)visible=1;if(row<s->first_visible)s->first_visible=row;
    if(row>=s->first_visible+visible)s->first_visible=row-visible+1;
    if(s->first_visible<0)s->first_visible=0;update_scrollbars(s);
}
static int find_selected(LISTVIEW_STATE *s,int start)
{if(start< -1||start>=s->row_count-1)return -1;for(int i=start+1;i<s->row_count;++i)if(s->rows[i].state&LVIS_SELECTED)return i;return -1;}
static BOOL insert_column(LISTVIEW_STATE *s,int index,const LVCOLUMNW *column)
{
    if(!column||index<0||index>s->column_count||s->column_count==0x3fffffff||!ensure_columns(s,s->column_count+1))return FALSE;
    WCHAR *text=(column->mask&LVCF_TEXT)?copy_text(column->pszText):copy_text(L"");if(!text)return FALSE;
    for(int i=s->column_count;i>index;--i)s->columns[i]=s->columns[i-1];
    s->columns[index].text=text;s->columns[index].width=(column->mask&LVCF_WIDTH)?column->cx:100;
    if(s->columns[index].width<24)s->columns[index].width=24;
    s->columns[index].format=(column->mask&LVCF_FMT)?column->fmt:LVCFMT_LEFT;
    s->columns[index].subitem=(column->mask&LVCF_SUBITEM)?column->iSubItem:index;
    ++s->column_count;update_scrollbars(s);InvalidateRect(s->hwnd,NULL,FALSE);return TRUE;
}
static BOOL get_column(LISTVIEW_STATE *s,int index,LVCOLUMNW *column)
{
    if(!column||index<0||index>=s->column_count)return FALSE;
    if(column->mask&LVCF_TEXT)text_copy(column->pszText,column->cchTextMax,s->columns[index].text);
    if(column->mask&LVCF_WIDTH)column->cx=s->columns[index].width;
    if(column->mask&LVCF_FMT)column->fmt=s->columns[index].format;
    if(column->mask&LVCF_SUBITEM)column->iSubItem=s->columns[index].subitem;
    return TRUE;
}
static BOOL set_column(LISTVIEW_STATE *s,int index,const LVCOLUMNW *column)
{
    if(!column||index<0||index>=s->column_count)return FALSE;
    if(column->mask&LVCF_TEXT){WCHAR *text=copy_text(column->pszText);if(!text)return FALSE;
        WCHAR *old=s->columns[index].text;s->columns[index].text=text;SvComctlFree(old);}
    if(column->mask&LVCF_WIDTH)s->columns[index].width=column->cx<24?24:column->cx;
    if(column->mask&LVCF_FMT)s->columns[index].format=column->fmt;
    if(column->mask&LVCF_SUBITEM)s->columns[index].subitem=column->iSubItem;
    update_scrollbars(s);InvalidateRect(s->hwnd,NULL,FALSE);return TRUE;
}
static int column_click(LISTVIEW_STATE *s,int x)
{return hit_column(s,x);}
static void notify_simple(LISTVIEW_STATE *s,int item,int subitem,UINT code,LPARAM param)
{
    NMLISTVIEW note;note.hdr.hwndFrom=s->hwnd;note.hdr.idFrom=s->id;note.hdr.code=code;
    note.iItem=item;note.iSubItem=subitem;note.uNewState=note.uOldState=note.uChanged=0;
    note.ptAction.x=-1;note.ptAction.y=-1;note.lParam=param;
    SendMessageW(s->parent,WM_NOTIFY,(WPARAM)s->id,(LPARAM)&note);
}
static LRESULT listview_dispatch(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,LISTVIEW_STATE *s)
{
    if(msg==WM_NCCREATE){
        CREATESTRUCTW *cs=(CREATESTRUCTW *)lp;
        if(!cs||(((DWORD)cs->style&3u)!=LVS_REPORT))return FALSE;
        s=SvComctlAlloc(sizeof(*s));if(!s)return FALSE;
        s->hwnd=hwnd;s->parent=cs->hwndParent;s->id=(UINT)(UINT_PTR)cs->hMenu;s->style=(DWORD)cs->style;
        s->extended_style=0;s->columns=NULL;s->column_count=s->column_capacity=0;
        s->rows=NULL;s->row_count=s->row_capacity=0;s->first_visible=0;
        s->vertical=s->horizontal=s->editor=NULL;s->editor_proc=NULL;s->editor_identity=0;s->editor_ending=FALSE;s->horizontal_offset=s->wheel_remainder=0;
        s->view_width=s->view_height=0;s->references=1;s->destroyed=FALSE;s->selection_anchor=-1;s->next_identity=0;
        lock_states();s->next=states;states=s;unlock_states();return TRUE;
    }
    if(!s)return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg){
    case WM_CREATE:
        s->vertical=CreateWindowExW(0,L"SCROLLBAR",L"",WS_CHILD|1,0,0,17,1,hwnd,(HMENU)(UINT_PTR)1,NULL,NULL);
        s->horizontal=CreateWindowExW(0,L"SCROLLBAR",L"",WS_CHILD,0,0,1,17,hwnd,(HMENU)(UINT_PTR)2,NULL,NULL);
        if(!s->vertical||!s->horizontal)return -1;update_scrollbars(s);return 0;
    case WM_PAINT:draw_listview(hwnd,s);return 0;
    case WM_ERASEBKGND:return 1;
    case LVM_GETITEMCOUNT:return s->row_count;
    case LVM_EDITLABELW:return (LRESULT)begin_label_edit(s,(int)wp);
    case LVM_GETEDITCONTROL:return (LRESULT)s->editor;
    case LVM_CANCELEDITLABEL:end_label_edit(s,FALSE,FALSE);return TRUE;
    case LVM_INSERTCOLUMNW:{int index=(int)wp;return insert_column(s,index,(const LVCOLUMNW *)lp)?index:-1;}
    case LVM_GETCOLUMNW:return get_column(s,(int)wp,(LVCOLUMNW *)lp);
    case LVM_SETCOLUMNW:return set_column(s,(int)wp,(const LVCOLUMNW *)lp);
    case LVM_INSERTITEMW: {
        const LVITEMW *item = (const LVITEMW *)lp;
        if (!item || item->iSubItem || item->iItem < 0) return -1;
        int index = item->iItem < s->row_count ? item->iItem : s->row_count;
        int added = insert_row(s, index, item);
        if (added >= 0){update_scrollbars(s);InvalidateRect(hwnd, NULL, FALSE);}
        return added;
    }
    case LVM_DELETEITEM:{int index=(int)wp;if(index<0||index>=s->row_count)return FALSE;free_row(&s->rows[index]);
        for(int i=index;i<s->row_count-1;++i)s->rows[i]=s->rows[i+1];--s->row_count;
        if(s->first_visible>=s->row_count)s->first_visible=s->row_count?s->row_count-1:0;update_scrollbars(s);InvalidateRect(hwnd,NULL,FALSE);return TRUE;}
    case LVM_DELETEALLITEMS:for(int i=0;i<s->row_count;++i)free_row(&s->rows[i]);s->row_count=0;s->first_visible=0;update_scrollbars(s);InvalidateRect(hwnd,NULL,FALSE);return TRUE;
    case LVM_SETITEMW:{const LVITEMW *item=(const LVITEMW *)lp;return set_item(s,item);}
    case LVM_SETITEMTEXTW: {
        const LVITEMW *item = (const LVITEMW *)lp;
        if (!item || wp >= (WPARAM)s->row_count) return FALSE;
        if (!set_cell(&s->rows[wp], item->iSubItem, item->pszText)) return FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        return TRUE;
    }
    case LVM_GETITEMW:return get_item(s,(LVITEMW *)lp);
    case LVM_GETITEMTEXTW: {
        const LVITEMW *item = (const LVITEMW *)lp;
        if (!item || wp >= (WPARAM)s->row_count || item->iSubItem < 0) return 0;
        const LV_ROW *row = &s->rows[wp];
        const WCHAR *text = item->iSubItem < row->cell_count ? row->cells[item->iSubItem] : NULL;
        return text_copy(item->pszText, item->cchTextMax, text);
    }
    case LVM_GETITEMSTATE:{int i=(int)wp;return i>=0&&i<s->row_count?(s->rows[i].state&(UINT)lp):0;}
    case LVM_SETITEMSTATE:{
        const LVITEMW *item=(const LVITEMW *)lp;if(!item)return FALSE;
        int first=(int)wp,last=first;
        if(first==-1){first=0;last=s->row_count-1;if(!s->row_count)return TRUE;}
        if(first<0||last>=s->row_count)return FALSE;
        return set_item_states(s,first,last,item->state,item->stateMask);
    }
    case LVM_GETNEXTITEM:{int start=(int)wp;UINT flags=(UINT)lp;
        if(start< -1||start>=s->row_count||(flags&0x0c00u))return -1;
        int direction=(flags&0x0100u)?-1:1;
        int index=start<0?(direction>0?0:s->row_count-1):start+direction;
        UINT mask=0;if(flags&1)mask|=LVIS_FOCUSED;if(flags&2)mask|=LVIS_SELECTED;
        if(flags&4)mask|=4;if(flags&8)mask|=8;
        for(;index>=0&&index<s->row_count;index+=direction)if((s->rows[index].state&mask)==mask)return index;
        return -1;}
    case LVM_GETSELECTEDCOUNT:{int n=0;for(int i=0;i<s->row_count;++i)if(s->rows[i].state&LVIS_SELECTED)++n;return n;}
    case LVM_SETEXTENDEDLISTVIEWSTYLE:{DWORD mask=(DWORD)wp,value=(DWORD)lp,old=s->extended_style;
        if(!mask)mask=0xffffffffu;s->extended_style=(old&~mask)|(value&mask);InvalidateRect(hwnd,NULL,FALSE);return old;}
    case LVM_GETEXTENDEDLISTVIEWSTYLE:return s->extended_style;
    case LVM_GETTOPINDEX:return s->first_visible;
    case LVM_GETCOUNTPERPAGE:return rows_per_page(s);
    case LVM_SCROLL:{long long pixels=(int)lp;
        long long rows=pixels>=0?(pixels+LV_ROW_HEIGHT/2)/LV_ROW_HEIGHT:-((-pixels+LV_ROW_HEIGHT/2)/LV_ROW_HEIGHT);
        s->horizontal_offset=clamp_coordinate((long long)s->horizontal_offset+(int)wp);scroll_rows(s,rows);return TRUE;}
    case LVM_ENSUREVISIBLE:{int i=(int)wp;if(i<0||i>=s->row_count)return FALSE;ensure_visible(s,i);InvalidateRect(hwnd,NULL,FALSE);return TRUE;}
    case LVM_HITTEST:{LVHITTESTINFO *hit=(LVHITTESTINFO *)lp;if(!hit)return -1;hit->iItem=hit_row(s,hit->pt.y);hit->iSubItem=hit_column(s,hit->pt.x);hit->flags=hit->iItem>=0?LVHT_ONITEMLABEL:LVHT_NOWHERE;return hit->iItem;}
    case LVM_GETSUBITEMRECT:{RECT *r=(RECT *)lp;int row=(int)wp;if(!r||r->left!=LVIR_BOUNDS||row<0||row>=s->row_count)return FALSE;
        int subitem=(int)r->top;if(subitem<0||subitem>=s->column_count)return FALSE;*r=cell_rect(s,row,subitem);return TRUE;}
    case WM_LBUTTONDOWN:{int x=(short)LOWORD(lp),y=(short)HIWORD(lp);SetFocus(hwnd);
        if(y<LV_HEADER_HEIGHT){int col=column_click(s,x);if(col>=0)notify_simple(s,-1,col,LVN_COLUMNCLICK,0);return 0;}
        int row=hit_row(s,y);if(row>=0){select_row(s,row,(UINT)wp,FALSE);return 0;}return 0;}
    case WM_SIZE:update_scrollbars(s);InvalidateRect(hwnd,NULL,FALSE);return 0;
    case 0x020a /* WM_MOUSEWHEEL */:{s->wheel_remainder+=(short)HIWORD(wp);int steps=s->wheel_remainder/120;s->wheel_remainder%=120;if(steps)scroll_rows(s,-(long long)steps*3);return 0;}
    case WM_VSCROLL:case WM_HSCROLL:{BOOL vertical=msg==WM_VSCROLL;int position=vertical?s->first_visible:s->horizontal_offset;
        int page=vertical?rows_per_page(s):s->view_width;int code=LOWORD(wp);
        if(code==0)position=clamp_coordinate((long long)position-(vertical?1:16));
        else if(code==1)position=clamp_coordinate((long long)position+(vertical?1:16));
        else if(code==2)position=clamp_coordinate((long long)position-page);
        else if(code==3)position=clamp_coordinate((long long)position+page);
        else if(code==4||code==5){LV_SCROLL_INFO info={sizeof(info),16,0,0,0,0,0};if(GetScrollInfo(vertical?s->vertical:s->horizontal,2,&info))position=info.track;else position=HIWORD(wp);}
        else if(code==6)position=0;else if(code==7)position=0x7fffffff;
        if(vertical)s->first_visible=position;else s->horizontal_offset=position;update_scrollbars(s);InvalidateRect(hwnd,NULL,FALSE);return 0;}
    case WM_LBUTTONDBLCLK:{int row=hit_row(s,(short)HIWORD(lp));if(row>=0)notify_simple(s,row,0,LVN_ITEMACTIVATE,s->rows[row].param);return 0;}
    case WM_KEYDOWN:{int selected=selection_focus(s),next;
        if(selected<0)selected=find_selected(s,-1);
        UINT modifiers=((GetKeyState(VK_SHIFT)&0x8000)?4u:0)|((GetKeyState(VK_CONTROL)&0x8000)?8u:0);
        if((modifiers&8)&&wp=='A'&&!(s->style&LVS_SINGLESEL)){set_item_states(s,0,s->row_count-1,LVIS_SELECTED,LVIS_SELECTED);return 0;}
        if(wp==VK_SPACE&&selected>=0){select_row(s,selected,modifiers&8?8:0,FALSE);return 0;}
        if(wp==VK_UP)next=selected>0?selected-1:0;else if(wp==VK_DOWN)next=selected<0?0:selected+1;
        else if(wp==VK_HOME)next=0;else if(wp==VK_END)next=s->row_count-1;
        else if(wp==0x21 /* VK_PRIOR */)next=selected<0?0:selected-rows_per_page(s);
        else if(wp==0x22 /* VK_NEXT */)next=selected<0?0:selected+rows_per_page(s);
        else if(wp==VK_RETURN){if(selected>=0)notify_simple(s,selected,0,LVN_ITEMACTIVATE,s->rows[selected].param);return 0;}else break;
        if(next<0)next=0;if(next>=s->row_count)next=s->row_count-1;
        if(next>=0&&next<s->row_count){select_row(s,next,modifiers,(modifiers&8)&&!(modifiers&4));
            int focused=selection_focus(s);if(focused>=0)ensure_visible(s,focused);InvalidateRect(hwnd,NULL,FALSE);}return 0;}
    case WM_NCDESTROY:{if(s->editor&&!s->editor_ending)end_label_edit(s,FALSE,FALSE);lock_states();LISTVIEW_STATE **link=&states;while(*link&&*link!=s)link=&(*link)->next;if(*link)*link=s->next;s->destroyed=TRUE;unlock_states();release_state(s);return 0;}
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
LRESULT SvListViewProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    if(msg==WM_NCCREATE)return listview_dispatch(hwnd,msg,wp,lp,NULL);
    LISTVIEW_STATE *state=find_state(hwnd);
    if(!state)return DefWindowProcW(hwnd,msg,wp,lp);
    LRESULT result=listview_dispatch(hwnd,msg,wp,lp,state);release_state(state);return result;
}
BOOL SvRegisterListView(void)
{
    WNDCLASSEXW cls;cls.cbSize=sizeof(cls);cls.style=8 /* CS_DBLCLKS */;cls.lpfnWndProc=SvListViewProc;
    cls.cbClsExtra=0;cls.cbWndExtra=0;cls.hInstance=NULL;cls.hIcon=NULL;cls.hCursor=NULL;
    cls.hbrBackground=NULL;cls.lpszMenuName=NULL;cls.lpszClassName=WC_LISTVIEWW;cls.hIconSm=NULL;
    return RegisterClassExW(&cls)!=0;
}
