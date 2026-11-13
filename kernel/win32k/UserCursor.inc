/* Session pointer artwork and per-thread cursor state. Images live in kernel
 * memory: changing or freeing an application's bitmap cannot race display. */
#define W32K_CURSOR_MAX_SIZE 256u
#define W32K_CURSOR_ALPHA 1u
#define W32K_CURSOR_ICON 2u
#define W32K_CURSOR_MONOCHROME 4u
typedef struct _W32K_CURSOR_DESCRIPTION {
    UINT32 Size, Width, Height, HotX, HotY, Flags;
} W32K_CURSOR_DESCRIPTION;
typedef struct _W32K_CURSOR_IMAGE {
    W32K_CURSOR_DESCRIPTION Description;
    UINT32 *Pixels;
    UINT8 *And;
    BOOLEAN Owned;
} W32K_CURSOR_IMAGE;
static W32K_CURSOR_IMAGE *g_cursor_images[USER_OBJECT_SLOTS];
static UINT64 g_cursor_selected;
static PKPROCESS g_cursor_process;
static UINT32 g_cursor_thread;
static PKTHREAD g_cursor_owner_thread;
static PVOID g_cursor_desktop, g_cursor_clip_desktop;
static BOOLEAN g_cursor_notify_registered;
static BOOLEAN g_cursor_default = TRUE;
static UINT32 g_cursor_default_id = 32512;
static BOOLEAN g_cursor_clipped;
static INT32 g_cursor_clip[4];
static PKPROCESS g_cursor_clip_owner;
static void W32kCursorRefresh(void);
static BOOLEAN W32kCursorPosition(INT32 *x, INT32 *y);
static void W32kCursorConstrain(void);
static UINT64 KiUserLoadCursorIcon(UINT64 module, UINT64 key, BOOLEAN icon,
                                  BOOLEAN shared);

static void W32kCursorSessionDeleted(PVOID identity, BOOLEAN station)
{
    if(station) return;
    if(g_cursor_desktop==identity) {
        g_cursor_selected=0; g_cursor_process=NULL; g_cursor_thread=0;
        g_cursor_owner_thread=NULL; g_cursor_default=TRUE; g_cursor_default_id=32512;
        g_cursor_desktop=NULL;
    }
    if(g_cursor_clip_desktop==identity) {
        g_cursor_clipped=FALSE; g_cursor_clip_owner=NULL; g_cursor_clip_desktop=NULL;
    }
}
static void W32kCursorSynchronizeDesktop(void)
{
    if(!g_cursor_notify_registered) {
        W32kRegisterSessionDeleteNotify(W32kCursorSessionDeleted);
        g_cursor_notify_registered=TRUE;
    }
    PVOID desktop=W32kGetInputDesktopIdentity();
    if(g_cursor_desktop!=desktop) {
        g_cursor_selected=0; g_cursor_process=NULL; g_cursor_thread=0;
        g_cursor_owner_thread=NULL; g_cursor_default=TRUE; g_cursor_default_id=32512;
        g_cursor_desktop=desktop;
    }
    if(g_cursor_clipped&&g_cursor_clip_desktop!=desktop) {
        g_cursor_clipped=FALSE; g_cursor_clip_owner=NULL; g_cursor_clip_desktop=NULL;
    }
}

static USER_OBJECT_OWNER *W32kCursorObject(UINT64 handle, PKPROCESS process)
{
    UINT16 slot = (UINT16)handle;
    if (slot < 200 || slot >= USER_OBJECT_SLOTS) return NULL;
    USER_OBJECT_OWNER *object = &g_user_objects[slot];
    return object->Type == USER_TYPE_CURSOR && object->Process == process &&
        handle == (((UINT64)object->Generation << 16) | slot) ? object : NULL;
}

/* Original stock artwork is part of the operating system, rather than a
 * special case in an application. Every stock id has its own raster/hotspot. */
static const UINT32 g_stock_cursor_ids[] = {
    32512,32513,32514,32515,32516,32642,32643,32644,32645,32646,
    32648,32649,32650,32651
};
#define W32K_STOCK_CURSORS (sizeof(g_stock_cursor_ids)/sizeof(g_stock_cursor_ids[0]))
static W32K_CURSOR_IMAGE g_stock_cursors[W32K_STOCK_CURSORS];
static UINT32 g_stock_cursor_pixels[W32K_STOCK_CURSORS][32 * 32];
static UINT8 g_stock_cursor_masks[W32K_STOCK_CURSORS][32 * 32];

static void W32kStockPixel(W32K_CURSOR_IMAGE *image, INT32 x, INT32 y,
                           UINT32 pixel)
{
    if (x >= 0 && y >= 0 && x < 32 && y < 32)
    {
        image->Pixels[y * 32 + x] = pixel;
        image->And[y * 32 + x] = !(pixel >> 24);
    }
}
static void W32kStockLine(W32K_CURSOR_IMAGE *image, INT32 x, INT32 y,
                         INT32 end_x, INT32 end_y, UINT32 pixel)
{
    INT32 dx = end_x > x ? end_x - x : x - end_x;
    INT32 dy = end_y > y ? y - end_y : end_y - y;
    INT32 sx = x < end_x ? 1 : -1, sy = y < end_y ? 1 : -1;
    INT32 error = dx + dy;
    for (;;) {
        W32kStockPixel(image, x, y, pixel);
        if (x == end_x && y == end_y) break;
        INT32 twice = error * 2;
        if (twice >= dy) { error += dy; x += sx; }
        if (twice <= dx) { error += dx; y += sy; }
    }
}
static void W32kStockArrow(W32K_CURSOR_IMAGE *image)
{
    static const char *const rows[] = {
        "#", "##", "#.#", "#..#", "#...#", "#....#", "#.....#",
        "#......#", "#.......#", "#........#", "#.........#", "#..........#",
        "#.....######", "#..#..#", "#.# #..#", "##  #..#", "#    #..#",
        "     #..#", "      ##"
    };
    for (UINT32 y = 0; y < sizeof(rows)/sizeof(rows[0]); ++y)
        for (UINT32 x = 0; rows[y][x]; ++x)
            if (rows[y][x] != ' ')
                W32kStockPixel(image, x, y, rows[y][x] == '#' ?
                                0xff000000u : 0xffffffffu);
}
static void W32kStockRing(W32K_CURSOR_IMAGE *image, INT32 cx, INT32 cy,
                         INT32 radius, UINT32 color)
{
    for (INT32 y = -radius-1; y <= radius+1; ++y)
        for (INT32 x = -radius-1; x <= radius+1; ++x) {
            INT32 distance = x*x + y*y;
            if (distance <= (radius+1)*(radius+1) &&
                distance >= (radius-2)*(radius-2))
                W32kStockPixel(image, cx+x, cy+y, color);
        }
}
static void W32kStockDoubleArrow(W32K_CURSOR_IMAGE *image, INT32 dx, INT32 dy)
{
    INT32 ax = 16-dx*10, ay = 16-dy*10, bx = 16+dx*10, by = 16+dy*10;
    W32kStockLine(image, ax, ay, bx, by, 0xff000000u);
    W32kStockLine(image, ax-dy, ay+dx, bx-dy, by+dx, 0xffffffffu);
    W32kStockLine(image, ax+dy, ay-dx, bx+dy, by-dx, 0xff000000u);
    for (INT32 side = -1; side <= 1; side += 2) {
        W32kStockLine(image, ax, ay, ax+dx*5-dy*side*5,
                       ay+dy*5+dx*side*5, 0xff000000u);
        W32kStockLine(image, bx, by, bx-dx*5-dy*side*5,
                       by-dy*5+dx*side*5, 0xff000000u);
    }
}
static W32K_CURSOR_IMAGE *W32kStockCursor(UINT64 id)
{
    UINT32 slot;
    for (slot = 0; slot < W32K_STOCK_CURSORS; ++slot)
        if (g_stock_cursor_ids[slot] == id) break;
    if (slot == W32K_STOCK_CURSORS) return NULL;
    UINT64 flags = KiIrqSave();
    W32K_CURSOR_IMAGE *image = &g_stock_cursors[slot];
    if (image->Pixels) { KiIrqRestore(flags); return image; }
    image->Description = (W32K_CURSOR_DESCRIPTION){sizeof(W32K_CURSOR_DESCRIPTION),
                                                  32,32,16,16,W32K_CURSOR_ALPHA};
    image->Pixels = g_stock_cursor_pixels[slot];
    image->And = g_stock_cursor_masks[slot];
    memset(image->And, 1, 32*32);
    if (id == 32512 || id == 32650 || id == 32651) {
        image->Description.HotX = image->Description.HotY = 0;
        W32kStockArrow(image);
        if (id == 32650) W32kStockRing(image,23,13,7,0xff2474b7u);
        if (id == 32651) {
            W32kStockLine(image,18,3,24,3,0xff000000u);
            W32kStockLine(image,24,3,25,7,0xff000000u);
            W32kStockLine(image,25,7,21,11,0xff000000u);
            W32kStockLine(image,21,11,21,14,0xff000000u);
            W32kStockPixel(image,21,17,0xff000000u);
        }
    } else if (id == 32513) {
        W32kStockLine(image,16,5,16,26,0xff000000u);
        W32kStockLine(image,15,5,15,26,0xffffffffu);
        W32kStockLine(image,12,4,19,4,0xff000000u);
        W32kStockLine(image,12,27,19,27,0xff000000u);
    } else if (id == 32514) {
        W32kStockRing(image,16,16,10,0xff2474b7u);
        W32kStockRing(image,16,16,7,0xffffffffu);
        W32kStockLine(image,16,16,16,9,0xff000000u);
        W32kStockLine(image,16,16,21,18,0xff000000u);
    } else if (id == 32515) {
        W32kStockLine(image,16,3,16,29,0xff000000u);
        W32kStockLine(image,3,16,29,16,0xff000000u);
        W32kStockLine(image,17,3,17,29,0xffffffffu);
        W32kStockLine(image,3,17,29,17,0xffffffffu);
    } else if (id == 32516) {
        image->Description.HotY = 3;
        for (INT32 y=3;y<=13;++y)
            for (INT32 x=16-(y-3);x<=16+(y-3);++x)
                W32kStockPixel(image,x,y,(x==16-(y-3)||x==16+(y-3)||y==13)?0xff000000u:0xffffffffu);
        for (INT32 y=14;y<29;++y)
            for (INT32 x=13;x<=19;++x)
                W32kStockPixel(image,x,y,(x==13||x==19||y==28)?0xff000000u:0xffffffffu);
    } else if (id >= 32642 && id <= 32646) {
        if (id==32642) W32kStockDoubleArrow(image,1,1);
        if (id==32643) W32kStockDoubleArrow(image,1,-1);
        if (id==32644||id==32646) W32kStockDoubleArrow(image,1,0);
        if (id==32645||id==32646) W32kStockDoubleArrow(image,0,1);
    } else if (id == 32648) {
        W32kStockRing(image,16,16,11,0xffa32020u);
        W32kStockLine(image,8,8,24,24,0xffa32020u);
        W32kStockLine(image,9,8,25,24,0xffa32020u);
    } else if (id == 32649) {
        image->Description.HotX=13; image->Description.HotY=2;
        static const char *const hand[] = {
            "      ###", "      #.#", "      #.#", "      #.#", "      #.#",
            "      #.#", "      #.#####", "      #.#.#.###", "      #.#.#.#.#",
            "  ### #.......#", "  #..##.......#", "   #..........#", "    #.........#",
            "    #.........#", "     #........#", "     #........#", "      #......#",
            "      #......#", "      ########"
        };
        for(UINT32 y=0;y<sizeof(hand)/sizeof(hand[0]);++y)
            for(UINT32 x=0;hand[y][x];++x)
                if(hand[y][x]!=' ') W32kStockPixel(image,x+6,y+2,hand[y][x]=='#'?0xff000000u:0xffffffffu);
    }
    KiIrqRestore(flags); return image;
}

static W32K_CURSOR_IMAGE *W32kSelectedCursor(void)
{
    W32kCursorSynchronizeDesktop();
    if (g_cursor_owner_thread && g_cursor_owner_thread->CursorDisplayCount < 0) return NULL;
    if (g_cursor_default) return W32kStockCursor(g_cursor_default_id);
    USER_OBJECT_OWNER *object = W32kCursorObject(g_cursor_selected,g_cursor_process);
    return object && !object->Icon ? g_cursor_images[(UINT16)g_cursor_selected] : NULL;
}

static UINT64 NtUserSetCursor(UINT64 *a)
{
    PKTHREAD caller=KeGetCurrentThread();
    if(!caller||!caller->Process) return 0;
    UINT64 flags=KiIrqSave();
    USER_OBJECT_OWNER *object=a[0]?W32kCursorObject(a[0],caller->Process):NULL;
    if(a[0]&&(!object||object->Icon||!g_cursor_images[(UINT16)a[0]])) {
        KiIrqRestore(flags); return 0;
    }
    UINT64 old=g_cursor_selected;
    if(W32kGetCurrentDesktopIdentity()==W32kGetInputDesktopIdentity()) {
        g_cursor_desktop=W32kGetInputDesktopIdentity();
        g_cursor_selected=a[0]; g_cursor_process=caller->Process;
        g_cursor_thread=caller->ThreadId; g_cursor_owner_thread=caller; g_cursor_default=FALSE;
        W32kCursorRefresh();
    }
    KiIrqRestore(flags); return old;
}

static UINT64 SvNtUserShowCursor(UINT64 *a)
{
    PKTHREAD caller=KeGetCurrentThread();
    if(!caller||!caller->Process) return 0;
    UINT64 flags=KiIrqSave();
    if((a[0]&&caller->CursorDisplayCount<0x7fffffff)||(!a[0]&&caller->CursorDisplayCount>(-2147483647-1)))
        caller->CursorDisplayCount+=a[0]?1:-1;
    if(g_cursor_owner_thread==caller) W32kCursorRefresh();
    INT32 count=caller->CursorDisplayCount;
    KiIrqRestore(flags); return (UINT64)(INT64)count;
}

static UINT64 SvNtUserUploadCursor(UINT64 *a)
{
    PKTHREAD caller=KeGetCurrentThread();
    if(!caller||!caller->Process||!MmProbeForRead(a[1],sizeof(W32K_CURSOR_DESCRIPTION))) return 0;
    W32K_CURSOR_DESCRIPTION description;
    memcpy(&description,(const void*)a[1],sizeof(description));
    if(description.Size!=sizeof(description)||!description.Width||!description.Height||
       description.Width>W32K_CURSOR_MAX_SIZE||description.Height>W32K_CURSOR_MAX_SIZE||
       description.HotX>=description.Width||description.HotY>=description.Height||
       description.Flags&~(W32K_CURSOR_ALPHA|W32K_CURSOR_ICON|W32K_CURSOR_MONOCHROME)) return 0;
    SIZE_T count=(SIZE_T)description.Width*description.Height;
    if(!MmProbeForRead(a[2],count*sizeof(UINT32))||
       (!(description.Flags&W32K_CURSOR_ALPHA)&&!MmProbeForRead(a[3],count))) return 0;
    W32K_CURSOR_IMAGE *image=ExAllocatePoolWithTag(NonPagedPool,
                                sizeof(*image)+count*5,'ruCU');
    if(!image) return 0;
    image->Description=description; image->Owned=TRUE;
    image->Pixels=(UINT32*)(image+1); image->And=(UINT8*)(image->Pixels+count);
    memcpy(image->Pixels,(const void*)a[2],count*4);
    if(description.Flags&W32K_CURSOR_ALPHA) memset(image->And,0,count);
    else {
        memcpy(image->And,(const void*)a[3],count);
        for(SIZE_T i=0;i<count;++i) if(image->And[i]>1) { ExFreePool(image); return 0; }
    }
    UINT64 flags=KiIrqSave(),handle=a[0];
    if(!handle) handle=KiUserLoadCursorIcon(a[4],a[5],
                               (description.Flags&W32K_CURSOR_ICON)!=0,a[6]!=0);
    USER_OBJECT_OWNER *object=W32kCursorObject(handle,caller->Process);
    if(!object||object->Icon!=((description.Flags&W32K_CURSOR_ICON)!=0)) {
        KiIrqRestore(flags); ExFreePool(image); return 0;
    }
    W32K_CURSOR_IMAGE *old=g_cursor_images[(UINT16)handle];
    if(old&&object->SharedIcon) { KiIrqRestore(flags); ExFreePool(image); return handle; }
    g_cursor_images[(UINT16)handle]=image;
    if(g_cursor_selected==handle) W32kCursorRefresh();
    if(old&&old->Owned) ExFreePool(old);
    KiIrqRestore(flags); return handle;
}

static UINT64 SvNtUserQueryCursor(UINT64 *a)
{
    PKTHREAD caller=KeGetCurrentThread();
    UINT64 flags=KiIrqSave();
    W32kCursorSynchronizeDesktop();
    if(a[0]==0) { UINT64 cursor=g_cursor_selected; KiIrqRestore(flags); return cursor; }
    if(a[0]==1) {
        if(!MmProbeForWrite(a[1],24)||*(const UINT32*)a[1]!=24) { KiIrqRestore(flags); return 0; }
        INT32 x=0,y=0; BOOLEAN position=W32kCursorPosition(&x,&y);
        UINT8 *out=(UINT8*)a[1];
        *(UINT32*)(out+4)=position&&W32kSelectedCursor()?1:0;
        *(UINT64*)(out+8)=g_cursor_selected;
        *(INT32*)(out+16)=x; *(INT32*)(out+20)=y;
        KiIrqRestore(flags); return 1;
    }
    USER_OBJECT_OWNER *object=caller?W32kCursorObject(a[1],caller->Process):NULL;
    W32K_CURSOR_IMAGE *image=object?g_cursor_images[(UINT16)a[1]]:NULL;
    if(a[0]!=2||!image||!MmProbeForWrite(a[2],sizeof(image->Description))) { KiIrqRestore(flags); return 0; }
    SIZE_T count=(SIZE_T)image->Description.Width*image->Description.Height;
    if((a[3]||a[4])&&(a[5]<count||!MmProbeForWrite(a[3],count*4)||!MmProbeForWrite(a[4],count))) {
        KiIrqRestore(flags); return 0;
    }
    memcpy((void*)a[2],&image->Description,sizeof(image->Description));
    if(a[3]) { memcpy((void*)a[3],image->Pixels,count*4); memcpy((void*)a[4],image->And,count); }
    KiIrqRestore(flags); return 1;
}

static UINT64 SvNtUserClipCursor(UINT64 *a)
{
    if(W32kGetCurrentDesktopIdentity()!=W32kGetInputDesktopIdentity()) return 0;
    UINT64 flags=KiIrqSave();
    W32kCursorSynchronizeDesktop();
    if(a[0]) {
        if(!MmProbeForWrite(a[1],sizeof(g_cursor_clip))) { KiIrqRestore(flags); return 0; }
        INT32 rect[4]={0,0,(INT32)GfxFramebuffer.Width,(INT32)GfxFramebuffer.Height};
        memcpy((void*)a[1],g_cursor_clipped?g_cursor_clip:rect,sizeof(rect));
    } else if(a[1]) {
        INT32 rect[4];
        if(!MmProbeForRead(a[1],sizeof(rect))) { KiIrqRestore(flags); return 0; }
        memcpy(rect,(const void*)a[1],sizeof(rect));
        if(rect[0]>=rect[2]||rect[1]>=rect[3]) { KiIrqRestore(flags); return 0; }
        if(rect[0]<0) rect[0]=0;
        if(rect[1]<0) rect[1]=0;
        if(rect[2]>(INT32)GfxFramebuffer.Width) rect[2]=(INT32)GfxFramebuffer.Width;
        if(rect[3]>(INT32)GfxFramebuffer.Height) rect[3]=(INT32)GfxFramebuffer.Height;
        if(rect[0]>=rect[2]||rect[1]>=rect[3]) { KiIrqRestore(flags); return 0; }
        memcpy(g_cursor_clip,rect,sizeof(rect)); g_cursor_clipped=TRUE;
        g_cursor_clip_desktop=W32kGetInputDesktopIdentity();
        PKTHREAD caller=KeGetCurrentThread(); g_cursor_clip_owner=caller?caller->Process:NULL;
    } else { g_cursor_clipped=FALSE; g_cursor_clip_owner=NULL; g_cursor_clip_desktop=NULL; }
    if(!a[0]) W32kCursorConstrain();
    KiIrqRestore(flags); return 1;
}

static void W32kCursorDestroy(UINT64 handle)
{
    W32K_CURSOR_IMAGE *image=g_cursor_images[(UINT16)handle];
    if(g_cursor_selected==handle) {
        g_cursor_selected=0; g_cursor_default=TRUE; g_cursor_default_id=32512; W32kCursorRefresh();
    }
    g_cursor_images[(UINT16)handle]=NULL;
    if(image&&image->Owned) ExFreePool(image);
}
static void W32kCursorCleanupThread(PKTHREAD thread)
{
    if(g_cursor_process==thread->Process&&g_cursor_thread==thread->ThreadId) {
        g_cursor_selected=0; g_cursor_default=TRUE;
        g_cursor_default_id=32512;
        g_cursor_process=NULL; g_cursor_thread=0; g_cursor_owner_thread=NULL; W32kCursorRefresh();
    }
    thread->CursorDisplayCount=0;
}
static void W32kCursorCleanupProcess(PKPROCESS process)
{
    if(g_cursor_clip_owner==process) { g_cursor_clipped=FALSE; g_cursor_clip_owner=NULL; }
    if(g_cursor_process==process) {
        g_cursor_selected=0; g_cursor_default=TRUE; g_cursor_process=NULL; g_cursor_thread=0;
        g_cursor_owner_thread=NULL;
        g_cursor_default_id=32512;
        W32kCursorRefresh();
    }
}
