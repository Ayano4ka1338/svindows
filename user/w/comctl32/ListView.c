#include "Internal.h"

enum { LV_HEADER_HEIGHT=26, LV_ROW_HEIGHT=24 };
typedef struct LV_COLUMN { WCHAR *text; int width,format,subitem; } LV_COLUMN;
typedef struct LV_ROW { WCHAR **cells; int cell_count; UINT state; LPARAM param; } LV_ROW;
typedef struct LISTVIEW_STATE {
    struct LISTVIEW_STATE *next;
    HWND hwnd,parent;
    UINT id;
    DWORD style,extended_style;
    LV_COLUMN *columns; int column_count,column_capacity;
    LV_ROW *rows; int row_count,row_capacity,first_visible;
} LISTVIEW_STATE;
static LISTVIEW_STATE *states;
static unsigned state_lock;
static void lock_states(void){while(__atomic_exchange_n(&state_lock,1,__ATOMIC_ACQUIRE)){}}
static void unlock_states(void){__atomic_store_n(&state_lock,0,__ATOMIC_RELEASE);}
static LISTVIEW_STATE *find_state(HWND hwnd)
{
    lock_states();LISTVIEW_STATE *found=NULL;
    for(LISTVIEW_STATE *s=states;s;s=s->next)if(s->hwnd==hwnd){found=s;break;}
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
{int x=0;for(int i=0;i<index&&i<s->column_count;++i)x+=s->columns[i].width;return x;}
static RECT cell_rect(LISTVIEW_STATE *s,int row,int col)
{
    int x=column_x(s,col);
    return (RECT){x,LV_HEADER_HEIGHT+(row-s->first_visible)*LV_ROW_HEIGHT,
                  x+s->columns[col].width,LV_HEADER_HEIGHT+(row-s->first_visible+1)*LV_ROW_HEIGHT};
}
static void draw_listview(HWND hwnd,LISTVIEW_STATE *s)
{
    PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(!dc)return;
    RECT client;GetClientRect(hwnd,&client);SvComctlFill(dc,&client,RGB(255,255,255));
    int x=0;
    for(int c=0;c<s->column_count;++c){
        RECT head={x,0,x+s->columns[c].width,LV_HEADER_HEIGHT};
        SvComctlFill(dc,&head,RGB(243,243,243));
        RECT label={x+8,2,x+s->columns[c].width-8,LV_HEADER_HEIGHT-2};
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,0,0));
        DrawTextW(dc,s->columns[c].text?s->columns[c].text:L"",-1,&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
        HPEN edge=CreatePen(0,1,RGB(210,210,210));HGDIOBJ old=edge?SelectObject(dc,edge):NULL;
        MoveToEx(dc,x+s->columns[c].width-1,0,NULL);LineTo(dc,x+s->columns[c].width-1,client.bottom);
        if(edge){SelectObject(dc,old);DeleteObject(edge);}x+=s->columns[c].width;
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
            RECT r=cell_rect(s,row,col);r.left+=8;r.right-=6;r.top+=1;r.bottom-=1;
            WCHAR *text=col<s->rows[row].cell_count?s->rows[row].cells[col]:NULL;
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,0,0));
            DrawTextW(dc,text?text:L"",-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
        }
    }
    if(line){SelectObject(dc,old_line);DeleteObject(line);}EndPaint(hwnd,&ps);
}
static int hit_row(LISTVIEW_STATE *s,int y)
{if(y<LV_HEADER_HEIGHT)return -1;int row=s->first_visible+(y-LV_HEADER_HEIGHT)/LV_ROW_HEIGHT;return row>=0&&row<s->row_count?row:-1;}
static int hit_column(LISTVIEW_STATE *s,int x)
{for(int c=0;c<s->column_count;++c){int left=column_x(s,c);if(x>=left&&x<left+s->columns[c].width)return c;}return -1;}
static void make_change(NMLISTVIEW *note,HWND from,UINT id,int item,UINT before,UINT after,LPARAM param)
{
    note->hdr.hwndFrom=from;note->hdr.idFrom=id;note->hdr.code=LVN_ITEMCHANGED;
    note->iItem=item;note->iSubItem=0;note->uOldState=before;note->uNewState=after;
    note->uChanged=LVIF_STATE;note->ptAction.x=-1;note->ptAction.y=-1;note->lParam=param;
}
static void select_row(LISTVIEW_STATE *s,int index)
{
    if(index<0||index>=s->row_count)return;
    int old=-1;for(int i=0;i<s->row_count;++i)if(s->rows[i].state&LVIS_SELECTED){old=i;break;}
    UINT old_state=old>=0?s->rows[old].state:0;
    if(old>=0)s->rows[old].state&=~(LVIS_SELECTED|LVIS_FOCUSED);
    UINT before=s->rows[index].state;s->rows[index].state|=LVIS_SELECTED|LVIS_FOCUSED;
    NMLISTVIEW old_note,new_note;BOOL send_old=old>=0&&old!=index&&old_state!=s->rows[old].state;
    BOOL send_new=before!=s->rows[index].state;HWND parent=s->parent,from=s->hwnd;UINT id=s->id;
    if(send_old)make_change(&old_note,from,id,old,old_state,s->rows[old].state,s->rows[old].param);
    if(send_new)make_change(&new_note,from,id,index,before,s->rows[index].state,s->rows[index].param);
    InvalidateRect(from,NULL,FALSE);
    if(send_old)SendMessageW(parent,WM_NOTIFY,(WPARAM)id,(LPARAM)&old_note);
    if(send_new)SendMessageW(parent,WM_NOTIFY,(WPARAM)id,(LPARAM)&new_note);
}
static int insert_row(LISTVIEW_STATE *s,int index,const LVITEMW *item)
{
    if(!item||index<0||index>s->row_count||s->row_count==0x3fffffff||!ensure_rows(s,s->row_count+1))return -1;
    for(int i=s->row_count;i>index;--i)s->rows[i]=s->rows[i-1];
    LV_ROW *row=&s->rows[index];row->cells=NULL;row->cell_count=0;
    row->state=(item->mask&LVIF_STATE)?item->state&item->stateMask:0;
    row->param=(item->mask&LVIF_PARAM)?item->lParam:0;
    if((item->mask&LVIF_TEXT)&&!set_cell(row,0,item->pszText)){
        free_row(row);
        for(int i=index;i<s->row_count;++i)s->rows[i]=s->rows[i+1];return -1;}
    ++s->row_count;return index;
}
static BOOL set_item(LISTVIEW_STATE *s,const LVITEMW *item)
{
    if(!item||item->iItem<0||item->iItem>=s->row_count||item->iSubItem<0)return FALSE;
    LV_ROW *row=&s->rows[item->iItem];
    if((item->mask&LVIF_TEXT)&&!set_cell(row,item->iSubItem,item->pszText))return FALSE;
    if(item->mask&LVIF_PARAM)row->param=item->lParam;
    if(item->mask&LVIF_STATE)row->state=(row->state&~item->stateMask)|(item->state&item->stateMask);
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
static void ensure_visible(LISTVIEW_STATE *s,int row)
{
    RECT client;GetClientRect(s->hwnd,&client);int visible=(client.bottom-LV_HEADER_HEIGHT)/LV_ROW_HEIGHT;
    if(visible<1)visible=1;if(row<s->first_visible)s->first_visible=row;
    if(row>=s->first_visible+visible)s->first_visible=row-visible+1;
    if(s->first_visible<0)s->first_visible=0;
}
static int find_selected(LISTVIEW_STATE *s,int start)
{for(int i=start+1;i<s->row_count;++i)if(s->rows[i].state&LVIS_SELECTED)return i;return -1;}
static BOOL insert_column(LISTVIEW_STATE *s,int index,const LVCOLUMNW *column)
{
    if(!column||index<0||index>s->column_count||s->column_count==0x3fffffff||!ensure_columns(s,s->column_count+1))return FALSE;
    WCHAR *text=(column->mask&LVCF_TEXT)?copy_text(column->pszText):copy_text(L"");if(!text)return FALSE;
    for(int i=s->column_count;i>index;--i)s->columns[i]=s->columns[i-1];
    s->columns[index].text=text;s->columns[index].width=(column->mask&LVCF_WIDTH)?column->cx:100;
    if(s->columns[index].width<24)s->columns[index].width=24;
    s->columns[index].format=(column->mask&LVCF_FMT)?column->fmt:LVCFMT_LEFT;
    s->columns[index].subitem=(column->mask&LVCF_SUBITEM)?column->iSubItem:index;
    ++s->column_count;InvalidateRect(s->hwnd,NULL,FALSE);return TRUE;
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
    InvalidateRect(s->hwnd,NULL,FALSE);return TRUE;
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
LRESULT SvListViewProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    LISTVIEW_STATE *s=find_state(hwnd);
    if(msg==WM_NCCREATE){
        CREATESTRUCTW *cs=(CREATESTRUCTW *)lp;
        if(!cs||(((DWORD)cs->style&3u)!=LVS_REPORT))return FALSE;
        s=SvComctlAlloc(sizeof(*s));if(!s)return FALSE;
        s->hwnd=hwnd;s->parent=cs->hwndParent;s->id=(UINT)(UINT_PTR)cs->hMenu;s->style=(DWORD)cs->style;
        s->extended_style=0;s->columns=NULL;s->column_count=s->column_capacity=0;
        s->rows=NULL;s->row_count=s->row_capacity=0;s->first_visible=0;
        lock_states();s->next=states;states=s;unlock_states();return TRUE;
    }
    if(!s)return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg){
    case WM_PAINT:draw_listview(hwnd,s);return 0;
    case WM_ERASEBKGND:return 1;
    case LVM_GETITEMCOUNT:return s->row_count;
    case LVM_INSERTCOLUMNW:{int index=(int)wp;return insert_column(s,index,(const LVCOLUMNW *)lp)?index:-1;}
    case LVM_GETCOLUMNW:return get_column(s,(int)wp,(LVCOLUMNW *)lp);
    case LVM_SETCOLUMNW:return set_column(s,(int)wp,(const LVCOLUMNW *)lp);
    case LVM_INSERTITEMW: {
        const LVITEMW *item = (const LVITEMW *)lp;
        if (!item || item->iSubItem || item->iItem < 0) return -1;
        int index = item->iItem < s->row_count ? item->iItem : s->row_count;
        int added = insert_row(s, index, item);
        if (added >= 0) InvalidateRect(hwnd, NULL, FALSE);
        return added;
    }
    case LVM_DELETEITEM:{int index=(int)wp;if(index<0||index>=s->row_count)return FALSE;free_row(&s->rows[index]);
        for(int i=index;i<s->row_count-1;++i)s->rows[i]=s->rows[i+1];--s->row_count;
        if(s->first_visible>=s->row_count)s->first_visible=s->row_count?s->row_count-1:0;InvalidateRect(hwnd,NULL,FALSE);return TRUE;}
    case LVM_DELETEALLITEMS:for(int i=0;i<s->row_count;++i)free_row(&s->rows[i]);s->row_count=0;s->first_visible=0;InvalidateRect(hwnd,NULL,FALSE);return TRUE;
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
    case LVM_SETITEMSTATE:{const LVITEMW *item=(const LVITEMW *)lp;if(!item)return FALSE;int first=(int)wp,last=first;
        if(!s->row_count)return FALSE;
        if(first==-1){first=0;last=s->row_count-1;}if(first<0||last>=s->row_count)return FALSE;
        if((item->stateMask&LVIS_SELECTED)&&(item->state&LVIS_SELECTED)){select_row(s,first);return TRUE;}
        UINT before=s->rows[first].state;
        for(int i=first;i<=last;++i)s->rows[i].state=(s->rows[i].state&~item->stateMask)|(item->state&item->stateMask);
        UINT after=s->rows[first].state;LPARAM param=s->rows[first].param;HWND parent=s->parent;UINT id=s->id;
        InvalidateRect(hwnd,NULL,FALSE);
        if(first==last&&before!=after){NMLISTVIEW note;make_change(&note,hwnd,id,first,before,after,param);
            SendMessageW(parent,WM_NOTIFY,(WPARAM)id,(LPARAM)&note);}
        return TRUE;}
    case LVM_GETNEXTITEM:{int start=(int)wp;if(((int)lp&LVNI_SELECTED)==0)return -1;return find_selected(s,start);}
    case LVM_GETSELECTEDCOUNT:{int n=0;for(int i=0;i<s->row_count;++i)if(s->rows[i].state&LVIS_SELECTED)++n;return n;}
    case LVM_SETEXTENDEDLISTVIEWSTYLE:{DWORD mask=(DWORD)wp,value=(DWORD)lp,old=s->extended_style;
        if(!mask)mask=0xffffffffu;s->extended_style=(old&~mask)|(value&mask);InvalidateRect(hwnd,NULL,FALSE);return old;}
    case LVM_GETEXTENDEDLISTVIEWSTYLE:return s->extended_style;
    case LVM_ENSUREVISIBLE:{int i=(int)wp;if(i<0||i>=s->row_count)return FALSE;ensure_visible(s,i);InvalidateRect(hwnd,NULL,FALSE);return TRUE;}
    case LVM_HITTEST:{LVHITTESTINFO *hit=(LVHITTESTINFO *)lp;if(!hit)return -1;hit->iItem=hit_row(s,hit->pt.y);hit->iSubItem=hit_column(s,hit->pt.x);hit->flags=hit->iItem>=0?LVHT_ONITEMLABEL:LVHT_NOWHERE;return hit->iItem;}
    case LVM_GETSUBITEMRECT:{RECT *r=(RECT *)lp;int row=(int)wp;if(!r||r->left!=LVIR_BOUNDS||row<0||row>=s->row_count)return FALSE;
        int subitem=(int)r->top;if(subitem<0||subitem>=s->column_count)return FALSE;*r=cell_rect(s,row,subitem);return TRUE;}
    case WM_LBUTTONDOWN:{int x=(short)LOWORD(lp),y=(short)HIWORD(lp);SetFocus(hwnd);
        if(y<LV_HEADER_HEIGHT){int col=column_click(s,x);if(col>=0)notify_simple(s,-1,col,LVN_COLUMNCLICK,0);return 0;}
        int row=hit_row(s,y);if(row>=0){select_row(s,row);return 0;}return 0;}
    case WM_SIZE:InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_LBUTTONDBLCLK:{int row=hit_row(s,(short)HIWORD(lp));if(row>=0)notify_simple(s,row,0,LVN_ITEMACTIVATE,s->rows[row].param);return 0;}
    case WM_KEYDOWN:{int selected=find_selected(s,-1),next;
        if(wp==VK_UP)next=selected>0?selected-1:0;else if(wp==VK_DOWN)next=selected<0?0:selected+1;
        else if(wp==VK_HOME)next=0;else if(wp==VK_END)next=s->row_count-1;else break;
        if(next>=0&&next<s->row_count){select_row(s,next);ensure_visible(s,next);InvalidateRect(hwnd,NULL,FALSE);}return 0;}
    case WM_NCDESTROY:{lock_states();LISTVIEW_STATE **link=&states;while(*link&&*link!=s)link=&(*link)->next;if(*link)*link=s->next;unlock_states();
        for(int i=0;i<s->row_count;++i)free_row(&s->rows[i]);for(int i=0;i<s->column_count;++i)SvComctlFree(s->columns[i].text);
        SvComctlFree(s->rows);SvComctlFree(s->columns);SvComctlFree(s);return 0;}
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
BOOL SvRegisterListView(void)
{
    WNDCLASSEXW cls;cls.cbSize=sizeof(cls);cls.style=0;cls.lpfnWndProc=SvListViewProc;
    cls.cbClsExtra=0;cls.cbWndExtra=0;cls.hInstance=NULL;cls.hIcon=NULL;cls.hCursor=NULL;
    cls.hbrBackground=NULL;cls.lpszMenuName=NULL;cls.lpszClassName=WC_LISTVIEWW;cls.hIconSm=NULL;
    return RegisterClassExW(&cls)!=0;
}
