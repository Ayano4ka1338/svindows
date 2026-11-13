#include "FileManager.h"

enum {
    ID_ADDRESS=100,ID_GO,ID_BACK,ID_FORWARD,ID_UP,ID_REFRESH,ID_OPEN,
    ID_FOLDER,ID_FILE,ID_RENAME,ID_COPY,ID_CUT,ID_PASTE,ID_DELETE,
    ID_LIST,ID_STATUS,ID_CANCEL,
    WM_OPERATION_PROGRESS=WM_APP+1,WM_OPERATION_DONE,WM_BROWSER_REFRESH,
    OP_COPY=1,OP_MOVE,OP_DELETE
};
typedef struct { WCHAR name[SHELL_PATH_CAPACITY];DWORD attributes;ULONGLONG size; } FILE_ENTRY;
typedef struct { WCHAR source[SHELL_PATH_CAPACITY],destination[SHELL_PATH_CAPACITY];DWORD attributes; } FILE_WALK;
typedef struct FILE_OPERATION {
    HWND owner;HANDLE worker;UINT kind;BOOL overwrite;
    volatile BOOL cancel;volatile LONG finished;
    WCHAR **sources;SIZE_T source_count;
    WCHAR directory[SHELL_PATH_CAPACITY],failed[SHELL_PATH_CAPACITY];
    DWORD error,clipboard_sequence,last_report;
    ULONGLONG completed_bytes,total_bytes;
    SIZE_T completed_files;
} FILE_OPERATION;
typedef struct FILE_BROWSER {
    struct FILE_BROWSER *next;
    HWND window,address,list,status,cancel;
    HWND buttons[13];
    WCHAR directory[SHELL_PATH_CAPACITY];
    FILE_ENTRY *entries;SIZE_T count,capacity;
    WCHAR **history;SIZE_T history_count,history_capacity,history_position;
    FILE_OPERATION *operation;
} FILE_BROWSER;
typedef struct { FILE_BROWSER *state; BOOL transferred; } BROWSER_CREATION;
static FILE_BROWSER *browsers;
static UINT preferred_drop_effect;

static BOOL is_dot(const WCHAR *name)
{ return name[0]=='.'&&(!name[1]||(name[1]=='.'&&!name[2])); }
static int compare_entries(const FILE_ENTRY *a,const FILE_ENTRY *b)
{
    BOOL af=!!(a->attributes&FILE_ATTRIBUTE_DIRECTORY),bf=!!(b->attributes&FILE_ATTRIBUTE_DIRECTORY);
    return af!=bf?(af?-1:1):ShellCompare(a->name,b->name);
}
static void sift_entries(FILE_ENTRY *entries,SIZE_T count,SIZE_T root)
{
    while(root<count/2) {
        SIZE_T child=root*2+1;
        if(child+1<count&&compare_entries(entries+child,entries+child+1)<0) ++child;
        if(compare_entries(entries+root,entries+child)>=0) break;
        FILE_ENTRY swap=entries[root];entries[root]=entries[child];entries[child]=swap;root=child;
    }
}
static void sort_entries(FILE_ENTRY *entries,SIZE_T count)
{
    for(SIZE_T i=count/2;i>0;--i) sift_entries(entries,count,i-1);
    for(SIZE_T end=count;end>1;--end) {FILE_ENTRY swap=entries[0];entries[0]=entries[end-1];entries[end-1]=swap;sift_entries(entries,end-1,0);}
}
static void status_count(FILE_BROWSER *state)
{
    WCHAR status[96],number[32];ShellUnsigned(number,32,state->count);
    ShellCopy(status,96,number);ShellCopy(status+ShellLength(status),96-ShellLength(status),L" items");
    ShellSetStatus(state->status,status);
}
static BOOL add_history(FILE_BROWSER *state,const WCHAR *path)
{
    if(state->history_count&&state->history_position<state->history_count&&
        !ShellCompare(state->history[state->history_position],path)) return TRUE;
    if(!ShellReserve((void **)&state->history,&state->history_capacity,state->history_count+1,sizeof(WCHAR *))) return FALSE;
    WCHAR *copy=ShellDuplicate(path);if(!copy) return FALSE;
    SIZE_T next=state->history_count?state->history_position+1:0;
    while(state->history_count>next) ShellFree(state->history[--state->history_count]);
    state->history[state->history_count++]=copy;state->history_position=state->history_count-1;return TRUE;
}
static BOOL browser_load(FILE_BROWSER *state,const WCHAR *input,BOOL history)
{
    WCHAR path[SHELL_PATH_CAPACITY],pattern[SHELL_PATH_CAPACITY];
    if(!ShellNormalizePath(input,state->directory[0]?state->directory:L"C:\\",path,SHELL_PATH_CAPACITY)||
        !ShellJoin(pattern,SHELL_PATH_CAPACITY,path,L"*")) {ShellError(state->window,L"Open folder",input,206);return FALSE;}
    DWORD attributes=GetFileAttributesW(path);
    if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)) {ShellError(state->window,L"Open folder",path,attributes==INVALID_FILE_ATTRIBUTES?GetLastError():267);return FALSE;}
    FILE_ENTRY *entries=NULL;SIZE_T count=0,capacity=0;SHELL_FIND_DATA data;
    HANDLE find=FindFirstFileW(pattern,&data);
    if(find!=INVALID_HANDLE_VALUE) {
        do {
            if(is_dot(data.name)) continue;
            if(!ShellReserve((void **)&entries,&capacity,count+1,sizeof(*entries))) {
                FindClose(find);ShellFree(entries);ShellError(state->window,L"Read folder",path,8);return FALSE;
            }
            ShellCopy(entries[count].name,SHELL_PATH_CAPACITY,data.name);
            entries[count].attributes=data.attributes;
            entries[count].size=((ULONGLONG)data.size_high<<32)|data.size_low;++count;
        } while(FindNextFileW(find,&data));FindClose(find);
    } else {DWORD error=GetLastError();if(error!=2&&error!=18){ShellError(state->window,L"Read folder",path,error);return FALSE;}}
    sort_entries(entries,count);
    if(history&&!add_history(state,path)) {ShellFree(entries);ShellError(state->window,L"Open folder",path,8);return FALSE;}
    SendMessageW(state->list,LVM_DELETEALLITEMS,0,0);ShellFree(state->entries);
    state->entries=entries;state->count=count;state->capacity=capacity;ShellCopy(state->directory,SHELL_PATH_CAPACITY,path);
    SetWindowTextW(state->address,path);SetWindowTextW(state->window,path);
    for(SIZE_T i=0;i<count;++i) {
        LVITEMW item;ShellZero(&item,sizeof(item));item.mask=LVIF_TEXT|LVIF_PARAM;item.iItem=(int)i;
        item.pszText=entries[i].name;item.lParam=(LPARAM)i;
        if(SendMessageW(state->list,LVM_INSERTITEMW,0,(LPARAM)&item)<0) {ShellError(state->window,L"Display folder",path,8);break;}
        item.iSubItem=1;item.pszText=(entries[i].attributes&FILE_ATTRIBUTE_DIRECTORY)?L"Folder":
            !ShellCompare(ShellExtension(entries[i].name),L".exe")?L"Application":
            !ShellCompare(ShellExtension(entries[i].name),L".url")?L"Shortcut":L"File";
        SendMessageW(state->list,LVM_SETITEMTEXTW,i,(LPARAM)&item);
        WCHAR size[40];size[0]=0;if(!(entries[i].attributes&FILE_ATTRIBUTE_DIRECTORY)) ShellUnsigned(size,40,entries[i].size);
        item.iSubItem=2;item.pszText=size;SendMessageW(state->list,LVM_SETITEMTEXTW,i,(LPARAM)&item);
    }
    EnableWindow(state->buttons[1],state->history_position>0);
    EnableWindow(state->buttons[2],state->history_position+1<state->history_count);
    status_count(state);ShellLogPath("[explorer] folder ",path);return TRUE;
}
static int selected_entry(FILE_BROWSER *state)
{
    int i=(int)SendMessageW(state->list,LVM_GETNEXTITEM,(WPARAM)-1,LVNI_SELECTED);
    return i>=0&&(SIZE_T)i<state->count?i:-1;
}
static BOOL selected_path(FILE_BROWSER *state,WCHAR *path)
{
    int selected=selected_entry(state);
    if(selected<0) {ShellSetStatus(state->status,L"Select a file or folder first.");return FALSE;}
    return ShellJoin(path,SHELL_PATH_CAPACITY,state->directory,state->entries[selected].name);
}
static void free_selected_paths(WCHAR **paths,SIZE_T count)
{for(SIZE_T i=0;i<count;++i)ShellFree(paths[i]);ShellFree(paths);}
static WCHAR **selected_paths(FILE_BROWSER *state,SIZE_T *count)
{
    *count=0;SIZE_T capacity=0;WCHAR **paths=NULL;
    int row=-1;
    while((row=(int)SendMessageW(state->list,LVM_GETNEXTITEM,(WPARAM)row,LVNI_SELECTED))>=0) {
        if((SIZE_T)row>=state->count)break;
        WCHAR path[SHELL_PATH_CAPACITY];
        if(!ShellJoin(path,SHELL_PATH_CAPACITY,state->directory,state->entries[row].name)||
           !ShellReserve((void **)&paths,&capacity,*count+1,sizeof(*paths)))goto failed;
        WCHAR *copy=ShellDuplicate(path);if(!copy)goto failed;paths[(*count)++]=copy;
    }
    if(!*count)ShellSetStatus(state->status,L"Select a file or folder first.");return paths;
failed:free_selected_paths(paths,*count);*count=0;SetLastError(8);return NULL;
}
static void open_selected(FILE_BROWSER *state)
{
    WCHAR path[SHELL_PATH_CAPACITY];if(!selected_path(state,path)) return;
    DWORD attributes=GetFileAttributesW(path);
    if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)) browser_load(state,path,TRUE);
    else ShellOpenPath(state->window,path);
}
static void operation_free(FILE_OPERATION *op)
{
    if(!op) return;
    if(op->worker) CloseHandle(op->worker);
    for(SIZE_T i=0;i<op->source_count;++i) ShellFree(op->sources[i]);
    ShellFree(op->sources);ShellFree(op);
}
static void report_operation(FILE_OPERATION *op,BOOL force)
{
    DWORD now=GetTickCount();if(force||(DWORD)(now-op->last_report)>=100) {
        op->last_report=now;PostMessageW(op->owner,WM_OPERATION_PROGRESS,0,0);
    }
}
static DWORD copy_progress(long long total,long long done,long long stream_total,long long stream_done,
    DWORD stream,DWORD reason,HANDLE source,HANDLE target,void *context)
{
    (void)total;(void)stream_total;(void)stream_done;(void)stream;(void)reason;(void)source;(void)target;
    FILE_OPERATION *op=context;
    /* completed_bytes includes previous files, while this callback describes
     * the current stream. The transfer counter shown below is per operation. */
    (void)done;report_operation(op,FALSE);
    return __atomic_load_n(&op->cancel,__ATOMIC_RELAXED)?1:0;
}
static BOOL append_walk(FILE_WALK **walk,SIZE_T *count,SIZE_T *capacity,const WCHAR *source,const WCHAR *destination,DWORD attributes)
{
    if(!ShellReserve((void **)walk,capacity,*count+1,sizeof(**walk))) {SetLastError(8);return FALSE;}
    FILE_WALK *entry=*walk+(*count)++;
    if(!ShellCopy(entry->source,SHELL_PATH_CAPACITY,source)||!ShellCopy(entry->destination,SHELL_PATH_CAPACITY,destination?destination:L"")) {SetLastError(206);return FALSE;}
    entry->attributes=attributes;return TRUE;
}
static DWORD operation_worker(void *argument)
{
    FILE_OPERATION *op=argument;FILE_WALK *walk=NULL;SIZE_T count=0,capacity=0;
    for(SIZE_T i=0;i<op->source_count&&!op->error;++i) {
        WCHAR destination[SHELL_PATH_CAPACITY];destination[0]=0;
        DWORD attributes=GetFileAttributesW(op->sources[i]);
        if(attributes==INVALID_FILE_ATTRIBUTES) {op->error=GetLastError();ShellCopy(op->failed,SHELL_PATH_CAPACITY,op->sources[i]);break;}
        if(op->kind!=OP_DELETE&&!ShellJoin(destination,SHELL_PATH_CAPACITY,op->directory,ShellLeaf(op->sources[i]))) {op->error=206;break;}
        if(op->kind!=OP_DELETE&&!ShellCompare(destination,op->sources[i])) {if(op->kind==OP_MOVE)continue;op->error=32;ShellCopy(op->failed,SHELL_PATH_CAPACITY,destination);break;}
        if(op->kind!=OP_DELETE&&(attributes&FILE_ATTRIBUTE_DIRECTORY)&&ShellPathWithin(destination,op->sources[i])) {op->error=87;ShellCopy(op->failed,SHELL_PATH_CAPACITY,destination);break;}
        if(!append_walk(&walk,&count,&capacity,op->sources[i],destination,attributes)) op->error=GetLastError();
    }
    for(SIZE_T i=0;i<count&&!op->error;++i) {
        if(__atomic_load_n(&op->cancel,__ATOMIC_RELAXED)) {op->error=1235;break;}
        /* Appending can grow the vector, so never retain an entry pointer
         * across enumeration. Copies are bounded by the filesystem path ABI. */
        FILE_WALK entry=walk[i];ShellCopy(op->failed,SHELL_PATH_CAPACITY,entry.source);
        if(op->kind==OP_MOVE) {
            if(op->overwrite&&GetFileAttributesW(entry.destination)!=INVALID_FILE_ATTRIBUTES) {
                op->error=183; /* A move never deletes a destination implicitly. */
                break;
            }
            if(!MoveFileW(entry.source,entry.destination)) op->error=GetLastError();
            else ++op->completed_files;
            report_operation(op,FALSE);continue;
        }
        if(entry.attributes&FILE_ATTRIBUTE_DIRECTORY) {
            if(op->kind==OP_COPY&&!CreateDirectoryW(entry.destination,NULL)) {
                DWORD attributes=GetFileAttributesW(entry.destination);
                if(!op->overwrite||attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)) {op->error=GetLastError();if(!op->error)op->error=183;break;}
            }
            WCHAR pattern[SHELL_PATH_CAPACITY];
            if(!ShellJoin(pattern,SHELL_PATH_CAPACITY,entry.source,L"*")) {op->error=206;break;}
            SHELL_FIND_DATA data;HANDLE find=FindFirstFileW(pattern,&data);
            if(find!=INVALID_HANDLE_VALUE) {
                do {
                    if(is_dot(data.name)) continue;
                    WCHAR source[SHELL_PATH_CAPACITY],destination[SHELL_PATH_CAPACITY];destination[0]=0;
                    if(!ShellJoin(source,SHELL_PATH_CAPACITY,entry.source,data.name)||
                        (op->kind==OP_COPY&&!ShellJoin(destination,SHELL_PATH_CAPACITY,entry.destination,data.name))) {op->error=206;break;}
                    if(!append_walk(&walk,&count,&capacity,source,destination,data.attributes)) {op->error=GetLastError();break;}
                    if(!(data.attributes&FILE_ATTRIBUTE_DIRECTORY)) op->total_bytes+=((ULONGLONG)data.size_high<<32)|data.size_low;
                } while(FindNextFileW(find,&data));FindClose(find);
            } else {DWORD error=GetLastError();if(error!=2&&error!=18)op->error=error;}
        } else if(op->kind==OP_COPY) {
            if(!CopyFileExW(entry.source,entry.destination,copy_progress,op,(BOOL *)&op->cancel,op->overwrite?0:1)) op->error=GetLastError();
            else {++op->completed_files;SHELL_FIND_DATA data;HANDLE find=FindFirstFileW(entry.destination,&data);if(find!=INVALID_HANDLE_VALUE){op->completed_bytes+=((ULONGLONG)data.size_high<<32)|data.size_low;FindClose(find);}}
            report_operation(op,FALSE);
        }
    }
    if(op->kind==OP_DELETE&&!op->error) {
        for(SIZE_T i=count;i>0;--i) {
            if(__atomic_load_n(&op->cancel,__ATOMIC_RELAXED)) {op->error=1235;break;}
            FILE_WALK *entry=walk+i-1;ShellCopy(op->failed,SHELL_PATH_CAPACITY,entry->source);
            BOOL ok=(entry->attributes&FILE_ATTRIBUTE_DIRECTORY)?RemoveDirectoryW(entry->source):DeleteFileW(entry->source);
            if(!ok) {op->error=GetLastError();break;}
            ++op->completed_files;report_operation(op,FALSE);
        }
    }
    ShellFree(walk);__atomic_store_n(&op->finished,1,__ATOMIC_RELEASE);
    PostMessageW(op->owner,WM_OPERATION_DONE,0,0);return op->error;
}
static void operation_done(FILE_BROWSER *state)
{
    FILE_OPERATION *op=state->operation;
    if(!op||!__atomic_load_n(&op->finished,__ATOMIC_ACQUIRE)) return;
    /* The worker's final message precedes thread exit; wait before releasing
     * its argument or closing its handle. */
    WaitForSingleObject(op->worker,INFINITE);
    state->operation=NULL;ShowWindow(state->cancel,SW_HIDE);
    DWORD error=op->error;WCHAR failed[SHELL_PATH_CAPACITY];ShellCopy(failed,SHELL_PATH_CAPACITY,op->failed);
    if(!error&&op->kind==OP_MOVE&&op->clipboard_sequence&&
        GetClipboardSequenceNumber()==op->clipboard_sequence&&OpenClipboard(state->window)) {EmptyClipboard();CloseClipboard();}
    operation_free(op);browser_load(state,state->directory,FALSE);ShellRefreshDesktop();
    if(error==1235) ShellSetStatus(state->status,L"Operation cancelled. Completed files remain at the destination.");
    else if(error) ShellError(state->window,L"File operation",failed,error);
    else {ShellSetStatus(state->status,L"Operation completed.");ShellLog("[explorer] file operation completed\r\n");}
}
static BOOL begin_operation(FILE_BROWSER *state,FILE_OPERATION *op)
{
    if(state->operation) {ShellSetStatus(state->status,L"Wait for the current operation or cancel it first.");operation_free(op);return FALSE;}
    op->owner=state->window;
    if(op->kind==OP_COPY) {
        for(SIZE_T i=0;i<op->source_count;++i) {
            WCHAR destination[SHELL_PATH_CAPACITY];
            if(!ShellJoin(destination,SHELL_PATH_CAPACITY,op->directory,ShellLeaf(op->sources[i]))) {operation_free(op);ShellError(state->window,L"Copy",state->directory,206);return FALSE;}
            if(GetFileAttributesW(destination)!=INVALID_FILE_ATTRIBUTES&&ShellCompare(destination,op->sources[i])) {
                if(!ShellConfirm(state->window,L"Replace destination",L"The destination already exists. Merge folders and replace matching files?")) {operation_free(op);return FALSE;}
                op->overwrite=TRUE;break;
            }
        }
    }
    state->operation=op;
    op->worker=CreateThread(NULL,0,operation_worker,op,0,NULL);
    if(!op->worker) {DWORD error=GetLastError();state->operation=NULL;operation_free(op);ShellError(state->window,L"Start file operation",state->directory,error);return FALSE;}
    ShowWindow(state->cancel,SW_SHOW);ShellSetStatus(state->status,L"Working…");return TRUE;
}
static BOOL clipboard_write(FILE_BROWSER *state,BOOL cut)
{
    SIZE_T count=0;WCHAR **paths=selected_paths(state,&count);if(!paths||!count)return FALSE;
    SIZE_T words=1;
    for(SIZE_T i=0;i<count;++i){SIZE_T length=ShellLength(paths[i])+1;if(length>(SIZE_T)-1-words){free_selected_paths(paths,count);return FALSE;}words+=length;}
    if(words>((SIZE_T)-1-sizeof(SHELL_DROPFILES))/sizeof(WCHAR)){free_selected_paths(paths,count);return FALSE;}
    SIZE_T size=sizeof(SHELL_DROPFILES)+words*sizeof(WCHAR);
    HANDLE data=GlobalAlloc(GMEM_MOVEABLE|0x40,size),effect=GlobalAlloc(GMEM_MOVEABLE|0x40,sizeof(DWORD));
    if(!data||!effect){if(data)GlobalFree(data);if(effect)GlobalFree(effect);free_selected_paths(paths,count);ShellError(state->window,L"Copy to clipboard",state->directory,8);return FALSE;}
    SHELL_DROPFILES *drop=GlobalLock(data);DWORD *value=GlobalLock(effect);
    if(!drop||!value){if(drop)GlobalUnlock(data);if(value)GlobalUnlock(effect);GlobalFree(data);GlobalFree(effect);free_selected_paths(paths,count);return FALSE;}
    drop->files_offset=sizeof(*drop);drop->wide=TRUE;
    WCHAR *names=(WCHAR *)((BYTE *)drop+sizeof(*drop));SIZE_T offset=0;
    for(SIZE_T i=0;i<count;++i){SIZE_T length=ShellLength(paths[i]);for(SIZE_T j=0;j<=length;++j)names[offset+j]=paths[i][j];offset+=length+1;}
    names[offset]=0;*value=cut?2:1;GlobalUnlock(data);GlobalUnlock(effect);free_selected_paths(paths,count);
    if(!preferred_drop_effect)preferred_drop_effect=RegisterClipboardFormatW(L"Preferred DropEffect");
    BOOL ok=FALSE,transferred_data=FALSE,transferred_effect=FALSE;
    if(preferred_drop_effect&&OpenClipboard(state->window)){
        if(EmptyClipboard()){transferred_data=SetClipboardData(CF_HDROP,data)!=NULL;transferred_effect=SetClipboardData(preferred_drop_effect,effect)!=NULL;ok=transferred_data&&transferred_effect;if(!ok)EmptyClipboard();}
        CloseClipboard();
    }
    if(!transferred_data)GlobalFree(data);if(!transferred_effect)GlobalFree(effect);
    if(!ok)ShellError(state->window,L"Copy to clipboard",state->directory,GetLastError());
    else ShellSetStatus(state->status,cut?L"Ready to move. Open the destination folder and choose Paste.":L"Ready to copy. Open the destination folder and choose Paste.");
    return ok;
}
static FILE_OPERATION *clipboard_read(FILE_BROWSER *state)
{
    if(!OpenClipboard(state->window)) {ShellError(state->window,L"Paste",state->directory,GetLastError());return NULL;}
    HANDLE handle=GetClipboardData(CF_HDROP);SIZE_T size=handle?GlobalSize(handle):0;
    SHELL_DROPFILES *drop=handle?GlobalLock(handle):NULL;
    FILE_OPERATION *op=NULL;SIZE_T capacity=0;DWORD error=13;
    if(drop&&size>=sizeof(*drop)&&size!=(SIZE_T)-1&&drop->files_offset>=sizeof(*drop)&&drop->files_offset<size) {
        op=ShellAlloc(sizeof(*op));if(!op) {error=8;goto finish;}
        op->kind=OP_COPY;op->clipboard_sequence=GetClipboardSequenceNumber();ShellCopy(op->directory,SHELL_PATH_CAPACITY,state->directory);
        HANDLE effect=preferred_drop_effect?GetClipboardData(preferred_drop_effect):NULL;
        if(effect&&GlobalSize(effect)>=sizeof(DWORD)) {DWORD *value=GlobalLock(effect);if(value){if(*value&2)op->kind=OP_MOVE;GlobalUnlock(effect);}}
        SIZE_T offset=drop->files_offset;BOOL terminated=FALSE;
        if(drop->wide&&(offset&1)) goto bad;
        while(offset<size) {
            WCHAR source[SHELL_PATH_CAPACITY];SIZE_T length=0;
            if(drop->wide) {
                if(offset+sizeof(WCHAR)>size) goto bad;
                const WCHAR *entry=(const WCHAR *)((BYTE *)drop+offset);
                if(!*entry) {terminated=TRUE;break;}
                while(offset+(length+1)*sizeof(WCHAR)<=size&&entry[length]) {if(length+1>=SHELL_PATH_CAPACITY)goto bad;source[length]=entry[length];++length;}
                if(offset+(length+1)*sizeof(WCHAR)>size) goto bad;
                source[length]=0;offset+=(length+1)*sizeof(WCHAR);
            } else {
                const char *entry=(const char *)drop+offset;
                if(!*entry) {terminated=TRUE;break;}
                while(offset+length<size&&entry[length]) ++length;
                if(offset+length>=size||length>0x7fffffff) goto bad;
                int converted=MultiByteToWideChar(0,0,entry,(int)length,source,SHELL_PATH_CAPACITY-1);
                if(converted<=0) goto bad;source[converted]=0;offset+=length+1;
            }
            WCHAR normalized[SHELL_PATH_CAPACITY];
            if(!ShellNormalizePath(source,L"C:\\",normalized,SHELL_PATH_CAPACITY)) goto bad;
            if(!ShellReserve((void **)&op->sources,&capacity,op->source_count+1,sizeof(WCHAR *))) {error=8;goto bad;}
            WCHAR *copy=ShellDuplicate(normalized);if(!copy){error=8;goto bad;}op->sources[op->source_count++]=copy;
        }
        if(!terminated||!op->source_count) goto bad;
        error=0;goto finish;
bad:
        operation_free(op);op=NULL;
    }
finish:
    if(drop) GlobalUnlock(handle);CloseClipboard();
    if(error) ShellError(state->window,L"Paste",state->directory,error);
    return op;
}
static void browser_delete(FILE_BROWSER *state)
{
    SIZE_T count=0;WCHAR **paths=selected_paths(state,&count);if(!paths||!count)return;
    WCHAR text[600];ShellCopy(text,600,count==1?L"Permanently delete this item and everything inside it?\\r\\n":L"Permanently delete all selected items and everything inside them?\\r\\n");
    if(count==1)ShellCopy(text+ShellLength(text),600-ShellLength(text),paths[0]);
    if(!ShellConfirm(state->window,L"Delete",text)){free_selected_paths(paths,count);return;}
    FILE_OPERATION *op=ShellAlloc(sizeof(*op));if(!op){free_selected_paths(paths,count);ShellError(state->window,L"Delete",state->directory,8);return;}
    op->kind=OP_DELETE;op->sources=paths;op->source_count=count;begin_operation(state,op);
}
static void browser_create(FILE_BROWSER *state,BOOL folder)
{
    WCHAR name[SHELL_PATH_CAPACITY],path[SHELL_PATH_CAPACITY];ShellCopy(name,SHELL_PATH_CAPACITY,folder?L"New folder":L"New file.txt");
    if(!ShellPrompt(state->window,folder?L"New folder":L"New file",L"Name",name,SHELL_PATH_CAPACITY)) return;
    if(!ShellValidName(name)||!ShellJoin(path,SHELL_PATH_CAPACITY,state->directory,name)) {ShellError(state->window,L"Create item",name,123);return;}
    BOOL ok=FALSE;DWORD error;
    if(folder) {ok=CreateDirectoryW(path,NULL);error=GetLastError();}
    else {HANDLE file=CreateFileW(path,0x40000000,0,NULL,1,FILE_ATTRIBUTE_NORMAL,NULL);error=GetLastError();
        if(file!=INVALID_HANDLE_VALUE) {ok=FlushFileBuffers(file);error=GetLastError();CloseHandle(file);}}
    if(!ok) ShellError(state->window,L"Create item",path,error);
    else {browser_load(state,state->directory,FALSE);ShellRefreshDesktop();}
}
static void browser_rename(FILE_BROWSER *state)
{
    int selected=selected_entry(state);if(selected<0)return;
    if(!SendMessageW(state->list,LVM_EDITLABELW,selected,0))ShellError(state->window,L"Rename",state->entries[selected].name,GetLastError());
}
static void browser_command(FILE_BROWSER *state,UINT command)
{
    if(command==ID_GO) {WCHAR path[SHELL_PATH_CAPACITY];GetWindowTextW(state->address,path,SHELL_PATH_CAPACITY);browser_load(state,path,TRUE);}
    else if(command==ID_BACK&&state->history_position>0) {SIZE_T position=state->history_position-1;if(browser_load(state,state->history[position],FALSE))state->history_position=position;}
    else if(command==ID_FORWARD&&state->history_position+1<state->history_count) {SIZE_T position=state->history_position+1;if(browser_load(state,state->history[position],FALSE))state->history_position=position;}
    else if(command==ID_UP) browser_load(state,L"..",TRUE);
    else if(command==ID_REFRESH) browser_load(state,state->directory,FALSE);
    else if(command==ID_OPEN) open_selected(state);
    else if(command==ID_FOLDER) browser_create(state,TRUE);
    else if(command==ID_FILE) browser_create(state,FALSE);
    else if(command==ID_RENAME) browser_rename(state);
    else if(command==ID_COPY||command==ID_CUT) clipboard_write(state,command==ID_CUT);
    else if(command==ID_PASTE) {FILE_OPERATION *op=clipboard_read(state);if(op)begin_operation(state,op);}
    else if(command==ID_DELETE) browser_delete(state);
    else if(command==ID_CANCEL&&state->operation) {__atomic_store_n(&state->operation->cancel,TRUE,__ATOMIC_RELAXED);ShellSetStatus(state->status,L"Cancelling…");}
    EnableWindow(state->buttons[1],state->history_position>0);
    EnableWindow(state->buttons[2],state->history_position+1<state->history_count);
}
static void browser_layout(FILE_BROWSER *state)
{
    RECT client;GetClientRect(state->window,&client);int width=client.right,height=client.bottom;
    for(int i=0;i<5;++i) MoveWindow(state->buttons[i],6+i*62,6,58,28,TRUE);
    MoveWindow(state->address,320,6,width>412?width-412:80,28,TRUE);
    MoveWindow(state->buttons[5],width-86,6,80,28,TRUE);
    for(int i=6;i<13;++i) MoveWindow(state->buttons[i],6+(i-6)*76,40,72,28,TRUE);
    MoveWindow(state->cancel,width-86,40,80,28,TRUE);
    MoveWindow(state->list,6,76,width-12,height>112?height-108:1,TRUE);
    MoveWindow(state->status,8,height-26,width-16,22,TRUE);
    LVCOLUMNW column;ShellZero(&column,sizeof(column));column.mask=LVCF_WIDTH;
    column.cx=width>380?width-300:180;SendMessageW(state->list,LVM_SETCOLUMNW,0,(LPARAM)&column);
}
static void browser_context(FILE_BROWSER *state,LPARAM lp)
{
    HMENU menu=CreatePopupMenu();if(!menu)return;
    AppendMenuW(menu,MF_STRING,ID_OPEN,L"Open");AppendMenuW(menu,MF_SEPARATOR,0,NULL);
    AppendMenuW(menu,MF_STRING,ID_COPY,L"Copy");AppendMenuW(menu,MF_STRING,ID_CUT,L"Cut");AppendMenuW(menu,MF_STRING,ID_PASTE,L"Paste");
    AppendMenuW(menu,MF_STRING,ID_RENAME,L"Rename");AppendMenuW(menu,MF_STRING,ID_DELETE,L"Delete");AppendMenuW(menu,MF_SEPARATOR,0,NULL);
    AppendMenuW(menu,MF_STRING,ID_FOLDER,L"New folder");AppendMenuW(menu,MF_STRING,ID_FILE,L"New file");
    int x=(short)LOWORD(lp),y=(short)HIWORD(lp);if(lp==-1){RECT rect;GetWindowRect(state->list,&rect);x=rect.left+30;y=rect.top+50;}
    UINT command=(UINT)TrackPopupMenu(menu,TPM_RETURNCMD,x,y,0,state->window,NULL);DestroyMenu(menu);if(command)browser_command(state,command);
}
static LRESULT browser_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    FILE_BROWSER *state=(void *)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(message==WM_CREATE) {
        BROWSER_CREATION *creation=((CREATESTRUCTW *)lp)->lpCreateParams;
        state=creation->state;creation->transferred=TRUE;state->window=hwnd;
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)state);state->next=browsers;browsers=state;
        const WCHAR *captions[13]={L"Open",L"Back",L"Forward",L"Up",L"Refresh",L"Go",L"Folder",L"New file",L"Rename",L"Copy",L"Cut",L"Paste",L"Delete"};
        const UINT ids[13]={ID_OPEN,ID_BACK,ID_FORWARD,ID_UP,ID_REFRESH,ID_GO,ID_FOLDER,ID_FILE,ID_RENAME,ID_COPY,ID_CUT,ID_PASTE,ID_DELETE};
        for(int i=0;i<13;++i) {state->buttons[i]=ShellButton(hwnd,ids[i],captions[i]);if(!state->buttons[i])return -1;}
        state->address=ShellEdit(hwnd,ID_ADDRESS,state->directory);state->cancel=ShellButton(hwnd,ID_CANCEL,L"Cancel");ShowWindow(state->cancel,SW_HIDE);
        if(state->address)SendMessageW(state->address,EM_LIMITTEXT,SHELL_PATH_CAPACITY-1,0);
        state->status=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,1,1,hwnd,(HMENU)(UINT_PTR)ID_STATUS,ShellInstance,NULL);
        state->list=CreateWindowExW(0,WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|WS_BORDER|WS_TABSTOP|LVS_REPORT|LVS_EDITLABELS,
            0,0,1,1,hwnd,(HMENU)(UINT_PTR)ID_LIST,ShellInstance,NULL);
        if(!state->address||!state->status||!state->list||!state->cancel) return -1;
        const WCHAR *columns[3]={L"Name",L"Type",L"Size (bytes)"};int widths[3]={420,140,130};
        for(int i=0;i<3;++i){LVCOLUMNW c;ShellZero(&c,sizeof(c));c.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_FMT;c.pszText=(WCHAR *)columns[i];c.cx=widths[i];c.fmt=i==2?LVCFMT_RIGHT:LVCFMT_LEFT;
            if(SendMessageW(state->list,LVM_INSERTCOLUMNW,i,(LPARAM)&c)<0)return -1;}
        SendMessageW(state->list,LVM_SETEXTENDEDLISTVIEWSTYLE,0,LVS_EX_FULLROWSELECT);
        browser_layout(state);SetTimer(hwnd,1,200,NULL);return 0;
    }
    if(!state) return DefWindowProcW(hwnd,message,wp,lp);
    if(message==WM_SIZE) {browser_layout(state);return 0;}
    if(message==WM_COMMAND&&HIWORD(wp)==BN_CLICKED) {browser_command(state,LOWORD(wp));return 0;}
    if(message==WM_NOTIFY) {
        NMHDR *note=(void *)lp;
        if(note&&note->hwndFrom==state->list&&note->code==LVN_BEGINLABELEDITW)return state->operation!=NULL;
        if(note&&note->hwndFrom==state->list&&note->code==LVN_ENDLABELEDITW) {
            struct {NMHDR header;LVITEMW item;} *edit=(void *)lp;
            int row=edit->item.iItem;
            if(!edit->item.pszText||row<0||(SIZE_T)row>=state->count||state->operation)return FALSE;
            const WCHAR *name=edit->item.pszText;
            WCHAR old_path[SHELL_PATH_CAPACITY],new_path[SHELL_PATH_CAPACITY];
            if(!ShellValidName(name)||!ShellJoin(old_path,SHELL_PATH_CAPACITY,state->directory,state->entries[row].name)||
               !ShellJoin(new_path,SHELL_PATH_CAPACITY,state->directory,name))return FALSE;
            if(ShellCompare(old_path,new_path)&&!MoveFileW(old_path,new_path)){ShellError(hwnd,L"Rename",old_path,GetLastError());return FALSE;}
            ShellCopy(state->entries[row].name,SHELL_PATH_CAPACITY,name);
            PostMessageW(hwnd,WM_BROWSER_REFRESH,0,0);ShellRefreshDesktop();return TRUE;
        }
        if(note&&note->hwndFrom==state->list&&note->code==LVN_ITEMACTIVATE) {open_selected(state);return 0;}
    }
    if(message==WM_BROWSER_REFRESH){if(!state->operation)browser_load(state,state->directory,FALSE);return 0;}
    if(message==WM_CONTEXTMENU) {browser_context(state,lp);return 0;}
    if(message==WM_OPERATION_PROGRESS&&state->operation) {
        WCHAR text[160],number[32];ShellCopy(text,160,L"Completed ");ShellUnsigned(number,32,state->operation->completed_files);
        ShellCopy(text+ShellLength(text),160-ShellLength(text),number);ShellCopy(text+ShellLength(text),160-ShellLength(text),L" items. Cancel stops after the current write.");
        ShellSetStatus(state->status,text);return 0;
    }
    if(message==WM_OPERATION_DONE||message==WM_TIMER) {operation_done(state);return 0;}
    if(message==WM_CLOSE) {DestroyWindow(hwnd);return 0;}
    if(message==WM_DESTROY) {
        KillTimer(hwnd,1);
        if(state->operation) {__atomic_store_n(&state->operation->cancel,TRUE,__ATOMIC_RELAXED);WaitForSingleObject(state->operation->worker,INFINITE);operation_free(state->operation);state->operation=NULL;}
        FILE_BROWSER **link=&browsers;while(*link&&*link!=state)link=&(*link)->next;if(*link)*link=state->next;
        for(SIZE_T i=0;i<state->history_count;++i)ShellFree(state->history[i]);ShellFree(state->history);ShellFree(state->entries);
        state->window=NULL;SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);ShellFree(state);
        if(ShellStandalone&&!browsers)PostQuitMessage(0);return 0;
    }
    if(message==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(dc){RECT r;GetClientRect(hwnd,&r);ShellPaintFill(dc,&r,RGB(247,248,250));EndPaint(hwnd,&ps);}return 0;}
    return DefWindowProcW(hwnd,message,wp,lp);
}
BOOL ShellRegisterBrowserClass(void)
{
    WNDCLASSEXW cls;ShellZero(&cls,sizeof(cls));cls.cbSize=sizeof(cls);cls.lpfnWndProc=browser_proc;
    cls.hInstance=ShellInstance;cls.hCursor=LoadCursorW(NULL,IDC_ARROW);cls.lpszClassName=L"SvFileExplorer";
    preferred_drop_effect=RegisterClipboardFormatW(L"Preferred DropEffect");
    return RegisterClassExW(&cls)!=0;
}
HWND ShellOpenFolder(const WCHAR *path)
{
    FILE_BROWSER *state=ShellAlloc(sizeof(*state));if(!state)return NULL;
    if(!ShellNormalizePath(path,L"C:\\",state->directory,SHELL_PATH_CAPACITY)){ShellFree(state);return NULL;}
    BROWSER_CREATION creation={state,FALSE};
    HWND hwnd=CreateWindowExW(0,L"SvFileExplorer",state->directory,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,820,600,NULL,NULL,ShellInstance,&creation);
    /* Failed creation also sends WM_DESTROY, which owns state after WM_CREATE. */
    if(!hwnd) {if(!creation.transferred)ShellFree(state);return NULL;}
    if(!browser_load(state,path,TRUE)) {DestroyWindow(hwnd);return NULL;}
    ShowWindow(hwnd,SW_SHOW);UpdateWindow(hwnd);SetForegroundWindow(hwnd);SetFocus(state->list);return hwnd;
}
BOOL ShellSelectFolderItems(HWND window,const WCHAR *const *paths,SIZE_T count,BOOL edit)
{
    FILE_BROWSER *state=browsers;while(state&&state->window!=window)state=state->next;
    if(!state || (count&&!paths) || (edit&&count!=1)){SetLastError(87);return FALSE;}
    if(count>(SIZE_T)-1/sizeof(SIZE_T)){SetLastError(8);return FALSE;}
    SIZE_T *indices=count?ShellAlloc(count*sizeof(*indices)):NULL;
    if(count&&!indices){SetLastError(8);return FALSE;}
    for(SIZE_T selected=0;selected<count;++selected) {
        WCHAR target[SHELL_PATH_CAPACITY];
        if(!ShellNormalizePath(paths[selected],state->directory,target,SHELL_PATH_CAPACITY)){ShellFree(indices);SetLastError(206);return FALSE;}
        SIZE_T row;
        for(row=0;row<state->count;++row) {
            WCHAR full[SHELL_PATH_CAPACITY];if(!ShellJoin(full,SHELL_PATH_CAPACITY,state->directory,state->entries[row].name))continue;
            if(!ShellCompare(target,full))break;
        }
        if(row==state->count){ShellFree(indices);SetLastError(2);return FALSE;}
        indices[selected]=row;
    }
    LVITEMW selection={0};selection.stateMask=LVIS_SELECTED|LVIS_FOCUSED;
    if(!SendMessageW(state->list,LVM_SETITEMSTATE,(WPARAM)-1,(LPARAM)&selection)){ShellFree(indices);SetLastError(31);return FALSE;}
    for(SIZE_T i=0;i<count;++i) {
        selection.state=LVIS_SELECTED|(i?0:LVIS_FOCUSED);selection.stateMask=LVIS_SELECTED|(i?0:LVIS_FOCUSED);
        if(!SendMessageW(state->list,LVM_SETITEMSTATE,indices[i],(LPARAM)&selection)){ShellFree(indices);SetLastError(31);return FALSE;}
    }
    if(count)SendMessageW(state->list,LVM_ENSUREVISIBLE,indices[0],FALSE);
    BOOL started=!edit || SendMessageW(state->list,0x1076 /* LVM_EDITLABELW */,indices[0],0)!=0;
    ShellFree(indices);UpdateWindow(state->list);
    if(!started){SetLastError(31);return FALSE;}return TRUE;
}
BOOL ShellBrowserMessage(MSG *message)
{
    if(message->message!=WM_KEYDOWN&&message->message!=WM_SYSKEYDOWN&&message->message!=WM_MOUSEWHEEL) return FALSE;
    HWND root=GetAncestor(message->hwnd,GA_ROOT);FILE_BROWSER *state=browsers;
    while(state&&state->window!=root)state=state->next;if(!state)return FALSE;
    if(message->message==WM_MOUSEWHEEL){SendMessageW(state->list,WM_MOUSEWHEEL,message->wParam,message->lParam);return TRUE;}
    if(message->wParam==VK_F5){browser_command(state,ID_REFRESH);return TRUE;}
    if((GetKeyState(VK_MENU)&0x8000)&&message->wParam==VK_LEFT){browser_command(state,ID_BACK);return TRUE;}
    if((GetKeyState(VK_MENU)&0x8000)&&message->wParam==VK_RIGHT){browser_command(state,ID_FORWARD);return TRUE;}
    if((GetKeyState(VK_MENU)&0x8000)&&message->wParam==VK_UP){browser_command(state,ID_UP);return TRUE;}
    if(message->hwnd==state->address&&message->wParam==VK_RETURN){browser_command(state,ID_GO);return TRUE;}
    if((GetKeyState(VK_CONTROL)&0x8000)&&message->wParam=='L'){SetFocus(state->address);SendMessageW(state->address,EM_SETSEL,0,-1);return TRUE;}
    if(message->hwnd==state->list) {
        UINT command=0;
        if(message->wParam==VK_RETURN)command=ID_OPEN;
        else if(message->wParam==VK_F2)command=ID_RENAME;
        else if(message->wParam==VK_DELETE)command=ID_DELETE;
        else if(message->wParam==VK_BACK)command=ID_UP;
        else if(GetKeyState(VK_CONTROL)&0x8000){if(message->wParam=='C')command=ID_COPY;else if(message->wParam=='X')command=ID_CUT;else if(message->wParam=='V')command=ID_PASTE;}
        if(command){browser_command(state,command);return TRUE;}
    }
    if(message->wParam==VK_TAB) {
        HWND controls[15];int count=0;controls[count++]=state->address;controls[count++]=state->list;
        for(int i=0;i<13;++i)controls[count++]=state->buttons[i];
        int selected=0;for(int i=0;i<count;++i)if(controls[i]==message->hwnd)selected=i;
        selected=(selected+((GetKeyState(VK_SHIFT)&0x8000)?count-1:1))%count;SetFocus(controls[selected]);return TRUE;
    }
    return FALSE;
}
