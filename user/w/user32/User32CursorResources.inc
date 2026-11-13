#include "User32CursorDib.inc"
typedef struct { UINT32 Size, Width, Height, HotX, HotY, Flags; } SV_CURSOR_DESCRIPTION;
__declspec(dllimport) UINT64 SvNtUserUploadCursor(HCURSOR,const SV_CURSOR_DESCRIPTION*,
                                const UINT32*,const UINT8*,HINSTANCE,UINT_PTR,BOOL);
__declspec(dllimport) UINT64 SvNtUserQueryCursor(UINT,UINT_PTR,void*,void*,void*,UINT_PTR);
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(HANDLE,DWORD,ULONG_PTR);
__declspec(dllimport) BOOL HeapFree(HANDLE,DWORD,void*);
__declspec(dllimport) HANDLE FindResourceW(HINSTANCE,const WCHAR*,const WCHAR*);
__declspec(dllimport) HANDLE LoadResource(HINSTANCE,HANDLE);
__declspec(dllimport) void *LockResource(HANDLE);
__declspec(dllimport) DWORD SizeofResource(HINSTANCE,HANDLE);
__declspec(dllimport) HANDLE CreateFileW(const WCHAR*,DWORD,DWORD,void*,DWORD,DWORD,HANDLE);
__declspec(dllimport) BOOL ReadFile(HANDLE,void*,DWORD,DWORD*,void*);
__declspec(dllimport) DWORD GetFileSize(HANDLE,DWORD*);
__declspec(dllimport) BOOL CloseHandle(HANDLE);
__declspec(dllimport) int MultiByteToWideChar(UINT,DWORD,const char*,int,WCHAR*,int);
__declspec(dllimport) void SetLastError(DWORD);
__declspec(dllimport) UINT64 NtUserDestroyCursor(HCURSOR);

static WCHAR *sv_cursor_widen(const char *name)
{
    int count=MultiByteToWideChar(0,0,name,-1,NULL,0);
    if(count<=0) return NULL;
    WCHAR *wide=HeapAlloc(GetProcessHeap(),0,(ULONG_PTR)count*sizeof(WCHAR));
    if(wide&&!MultiByteToWideChar(0,0,name,-1,wide,count)) {
        HeapFree(GetProcessHeap(),0,wide); return NULL;
    }
    return wide;
}

static HCURSOR sv_cursor_from_dib(HINSTANCE instance, UINT_PTR key,
                    const UINT8 *data, DWORD bytes, unsigned hot_x,
                    unsigned hot_y, int width, int height, BOOL shared)
{
    if(width<=0||height<=0||width>256||height>256) return NULL;
    ULONG_PTR count=(ULONG_PTR)width*height;
    UINT32 *pixels=HeapAlloc(GetProcessHeap(),0,count*5);
    if(!pixels) return NULL;
    UINT8 *mask=(UINT8*)(pixels+count);
    unsigned flags,source_width,source_height;
    HCURSOR cursor=NULL;
    if(sv_cursor_dib(data,bytes,width,height,pixels,mask,&flags,&source_width,&source_height)&&
       hot_x<source_width&&hot_y<source_height) {
        SV_CURSOR_DESCRIPTION shape={sizeof(shape),width,height,
            hot_x*(unsigned)width/source_width,hot_y*(unsigned)height/source_height,flags};
        cursor=(HCURSOR)(UINT_PTR)SvNtUserUploadCursor(NULL,&shape,pixels,mask,instance,key,shared);
    }
    HeapFree(GetProcessHeap(),0,pixels);
    return cursor;
}

static HCURSOR sv_cursor_file(const WCHAR *path,int width,int height)
{
    HANDLE file=CreateFileW(path,0x80000000u,1,NULL,3,0x80,NULL);
    if(file==(HANDLE)(UINT_PTR)-1) return NULL;
    DWORD high=0,bytes=GetFileSize(file,&high),read=0;
    /* Resource lengths are bounded before allocation, not silently truncated. */
    if(high||bytes<6||bytes>16u*1024u*1024u) { CloseHandle(file); SetLastError(13); return NULL; }
    UINT8 *contents=HeapAlloc(GetProcessHeap(),0,bytes);
    if(!contents) { CloseHandle(file); return NULL; }
    BOOL valid=ReadFile(file,contents,bytes,&read,NULL)&&read==bytes;
    CloseHandle(file);
    HCURSOR cursor=NULL;
    if(valid&&!cursor_u16(contents)&&cursor_u16(contents+2)==2) {
        unsigned count=cursor_u16(contents+4),best_score=~0u;
        const UINT8 *best=NULL;
        if(count<=(bytes-6)/16) for(unsigned i=0;i<count;++i) {
            const UINT8 *entry=contents+6+i*16;
            unsigned w=entry[0]?entry[0]:256,h=entry[1]?entry[1]:256;
            unsigned size=cursor_u32(entry+8),offset=cursor_u32(entry+12);
            if(offset>bytes||size>bytes-offset||size<40) continue;
            unsigned score=(w>(unsigned)width?w-width:width-w)+(h>(unsigned)height?h-height:height-h);
            if(score<best_score) { best_score=score; best=entry; }
        }
        if(best) cursor=sv_cursor_from_dib(NULL,0,contents+cursor_u32(best+12),
            cursor_u32(best+8),cursor_u16(best+4),cursor_u16(best+6),width,height,FALSE);
    }
    HeapFree(GetProcessHeap(),0,contents);
    if(!cursor) SetLastError(13);
    return cursor;
}

static HCURSOR sv_load_cursor(HINSTANCE instance,const WCHAR *name,int width,
                            int height,UINT flags)
{
    if(!name) return NULL;
    if(!width) width=32; if(!height) height=32;
    if(width<0||height<0||width>256||height>256) { SetLastError(87); return NULL; }
    if(flags&0x10u) return sv_cursor_file(name,width,height);
    if(!instance) {
        SV_LARGE_STRING resource={0,0,name};
        if(!IS_INTRESOURCE(name)) { SetLastError(1814); return NULL; }
        HCURSOR source=(HCURSOR)(UINT_PTR)NtUserFindExistingCursorIcon(NULL,&resource,NULL);
        if(!source) { SetLastError(1814); return NULL; }
        if(width==32&&height==32) return source;
        SV_CURSOR_DESCRIPTION shape;
        if(!SvNtUserQueryCursor(2,(UINT_PTR)source,&shape,NULL,NULL,0)) return NULL;
        ULONG_PTR source_count=(ULONG_PTR)shape.Width*shape.Height,target_count=(ULONG_PTR)width*height;
        UINT32 *memory=HeapAlloc(GetProcessHeap(),0,(source_count+target_count)*5+3);
        if(!memory) return NULL;
        UINT8 *source_mask=(UINT8*)(memory+source_count);
        UINT32 *target=(UINT32*)(memory+source_count+(source_count+3)/4);
        UINT8 *target_mask=(UINT8*)(target+target_count);
        if(!SvNtUserQueryCursor(2,(UINT_PTR)source,&shape,memory,source_mask,source_count)) {
            HeapFree(GetProcessHeap(),0,memory); return NULL;
        }
        for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
            unsigned from=(unsigned)y*shape.Height/height*shape.Width+(unsigned)x*shape.Width/width;
            target[y*width+x]=memory[from]; target_mask[y*width+x]=source_mask[from];
        }
        shape.HotX=shape.HotX*(unsigned)width/shape.Width;
        shape.HotY=shape.HotY*(unsigned)height/shape.Height;
        shape.Width=width; shape.Height=height;
        HCURSOR cursor=(HCURSOR)(UINT_PTR)SvNtUserUploadCursor(NULL,&shape,target,target_mask,
            NULL,(UINT_PTR)name|((UINT_PTR)(unsigned)width<<32)|((UINT_PTR)(unsigned)height<<48),(flags&0x8000u)!=0);
        HeapFree(GetProcessHeap(),0,memory); return cursor;
    }
    HANDLE group=FindResourceW(instance,name,MAKEINTRESOURCEW(12));
    DWORD bytes=group?SizeofResource(instance,group):0;
    const UINT8 *contents=group?LockResource(LoadResource(instance,group)):NULL;
    if(!contents||bytes<6||cursor_u16(contents)||cursor_u16(contents+2)!=2) { SetLastError(1814); return NULL; }
    unsigned count=cursor_u16(contents+4),best_score=~0u;
    const UINT8 *best=NULL; DWORD best_size=0;
    if(count>(bytes-6)/14) { SetLastError(13); return NULL; }
    for(unsigned i=0;i<count;++i) {
        const UINT8 *entry=contents+6+i*14;
        HANDLE resource=FindResourceW(instance,MAKEINTRESOURCEW(cursor_u16(entry+12)),MAKEINTRESOURCEW(1));
        const UINT8 *data=resource?LockResource(LoadResource(instance,resource)):NULL;
        DWORD size=resource?SizeofResource(instance,resource):0;
        if(!data||size<44) continue;
        unsigned w=cursor_u32(data+8),h=cursor_u32(data+12)/2;
        if(!w||!h||w>256||h>256) continue;
        unsigned score=(w>(unsigned)width?w-width:width-w)+(h>(unsigned)height?h-height:height-h);
        if(score<best_score) { best_score=score; best=data; best_size=size; }
    }
    if(!best) { SetLastError(13); return NULL; }
    UINT_PTR key=(UINT_PTR)group^((UINT_PTR)(unsigned)width<<32)^((UINT_PTR)(unsigned)height<<48);
    return sv_cursor_from_dib(instance,key,best+4,best_size-4,cursor_u16(best),cursor_u16(best+2),
                              width,height,(flags&0x8000u)!=0);
}

USER32_API HCURSOR LoadCursorA(HINSTANCE instance,const char *name)
{
    if(IS_INTRESOURCE(name)) return LoadCursorW(instance,(const WCHAR*)name);
    WCHAR *wide=sv_cursor_widen(name); if(!wide) return NULL;
    HCURSOR cursor=LoadCursorW(instance,wide); HeapFree(GetProcessHeap(),0,wide); return cursor;
}
USER32_API HCURSOR LoadCursorFromFileW(const WCHAR *name)
{ return name?sv_cursor_file(name,32,32):NULL; }
USER32_API HCURSOR LoadCursorFromFileA(const char *name)
{
    WCHAR *wide=name?sv_cursor_widen(name):NULL; if(!wide) return NULL;
    HCURSOR cursor=LoadCursorFromFileW(wide); HeapFree(GetProcessHeap(),0,wide); return cursor;
}
USER32_API BOOL DestroyCursor(HCURSOR cursor)
{
    if(cursor&&NtUserDestroyCursor(cursor)) return TRUE;
    SetLastError(1402); return FALSE;
}
USER32_API HCURSOR CreateCursor(HINSTANCE instance,int hot_x,int hot_y,
                               int width,int height,const void *and_bits,
                               const void *xor_bits)
{
    (void)instance;
    if(width<=0||height<=0||width>256||height>256||hot_x<0||hot_y<0||
       hot_x>=width||hot_y>=height||!and_bits||!xor_bits) { SetLastError(87); return NULL; }
    ULONG_PTR count=(ULONG_PTR)width*height;
    UINT32 *pixels=HeapAlloc(GetProcessHeap(),0,count*5);
    if(!pixels) return NULL;
    UINT8 *mask=(UINT8*)(pixels+count);
    unsigned stride=((unsigned)width+15)/16*2;
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        ULONG_PTR index=(ULONG_PTR)y*stride+x/8; unsigned bit=0x80u>>(x%8);
        mask[y*width+x]=(((const UINT8*)and_bits)[index]&bit)!=0;
        pixels[y*width+x]=(((const UINT8*)xor_bits)[index]&bit)?0xffffffu:0;
    }
    SV_CURSOR_DESCRIPTION shape={sizeof(shape),width,height,hot_x,hot_y,4};
    HCURSOR cursor=(HCURSOR)(UINT_PTR)SvNtUserUploadCursor(NULL,&shape,pixels,mask,NULL,0,FALSE);
    HeapFree(GetProcessHeap(),0,pixels); return cursor;
}

static UINT32 *sv_cursor_bitmap_pixels(HBITMAP bitmap, BITMAP *shape)
{
    if(GetObjectW(bitmap,sizeof(*shape),shape)!=sizeof(*shape)||shape->bmWidth<=0||
       shape->bmHeight<=0||shape->bmWidth>256||shape->bmHeight>512) return NULL;
    ULONG_PTR count=(ULONG_PTR)shape->bmWidth*shape->bmHeight;
    UINT32 *pixels=HeapAlloc(GetProcessHeap(),0,count*4);
    if(!pixels) return NULL;
    struct { DWORD Size; LONG Width,Height; WORD Planes,Bits; DWORD Compression,Bytes;
        LONG X,Y; DWORD Used,Important;
    } info={40,shape->bmWidth,-shape->bmHeight,1,32,0,0,0,0,0,0};
    if(GetDIBits(NULL,bitmap,0,shape->bmHeight,pixels,&info,0)!=shape->bmHeight) {
        HeapFree(GetProcessHeap(),0,pixels); return NULL;
    }
    return pixels;
}

USER32_API HICON CreateIconIndirect(const ICONINFO *info)
{
    if(!info||!info->hbmMask) { SetLastError(87); return NULL; }
    BITMAP mask_shape,color_shape;
    UINT32 *mask_pixels=sv_cursor_bitmap_pixels(info->hbmMask,&mask_shape);
    if(!mask_pixels) return NULL;
    UINT32 *color_pixels=info->hbmColor?sv_cursor_bitmap_pixels(info->hbmColor,&color_shape):NULL;
    unsigned width=info->hbmColor?color_pixels?(unsigned)color_shape.bmWidth:0:(unsigned)mask_shape.bmWidth;
    unsigned height=info->hbmColor?color_pixels?(unsigned)color_shape.bmHeight:0:(unsigned)mask_shape.bmHeight/2;
    HICON icon=NULL;
    if(width&&height&&height<=256&&(unsigned)mask_shape.bmWidth==width&&
       (unsigned)mask_shape.bmHeight==(info->hbmColor?height:height*2)&&
       (info->fIcon||(info->xHotspot<width&&info->yHotspot<height))) {
        ULONG_PTR count=(ULONG_PTR)width*height;
        UINT32 *pixels=HeapAlloc(GetProcessHeap(),0,count*5);
        if(pixels) {
            UINT8 *mask=(UINT8*)(pixels+count); BOOL alpha=FALSE;
            for(ULONG_PTR i=0;i<count;++i) {
                mask[i]=(mask_pixels[i]&0xffffffu)!=0;
                pixels[i]=color_pixels?color_pixels[i]:mask_pixels[count+i]&0xffffffu;
                if(color_pixels&&pixels[i]>>24) alpha=TRUE;
            }
            SV_CURSOR_DESCRIPTION shape={sizeof(shape),width,height,
                info->fIcon?width/2:info->xHotspot,info->fIcon?height/2:info->yHotspot,
                (info->fIcon?2u:0u)|(alpha?1u:0u)|(!color_pixels?4u:0u)};
            icon=(HICON)(UINT_PTR)SvNtUserUploadCursor(NULL,&shape,pixels,mask,NULL,0,FALSE);
            HeapFree(GetProcessHeap(),0,pixels);
        }
    }
    if(color_pixels) HeapFree(GetProcessHeap(),0,color_pixels);
    HeapFree(GetProcessHeap(),0,mask_pixels);
    if(!icon) SetLastError(87);
    return icon;
}

USER32_API BOOL GetIconInfo(HICON icon,ICONINFO *info)
{
    SV_CURSOR_DESCRIPTION shape;
    if(!info||!SvNtUserQueryCursor(2,(UINT_PTR)icon,&shape,NULL,NULL,0)) { SetLastError(1402); return FALSE; }
    ULONG_PTR count=(ULONG_PTR)shape.Width*shape.Height;
    unsigned stride=(shape.Width+15)/16*2,mask_height=(shape.Flags&4u)?shape.Height*2:shape.Height;
    UINT32 *pixels=HeapAlloc(GetProcessHeap(),0,count*5+(ULONG_PTR)stride*mask_height);
    if(!pixels) return FALSE;
    UINT8 *mask=(UINT8*)(pixels+count),*bitmap_bits=mask+count;
    if(!SvNtUserQueryCursor(2,(UINT_PTR)icon,&shape,pixels,mask,count)) { HeapFree(GetProcessHeap(),0,pixels); return FALSE; }
    for(ULONG_PTR i=0;i<(ULONG_PTR)stride*mask_height;++i) bitmap_bits[i]=0;
    for(unsigned y=0;y<shape.Height;++y) for(unsigned x=0;x<shape.Width;++x) {
        unsigned i=y*shape.Width+x; UINT8 bit=(UINT8)(0x80u>>(x%8));
        if(mask[i]||((shape.Flags&1u)&&!(pixels[i]>>24))) bitmap_bits[y*stride+x/8]|=bit;
        if((shape.Flags&4u)&&(pixels[i]&0xffffffu)) bitmap_bits[(y+shape.Height)*stride+x/8]|=bit;
    }
    HBITMAP mask_bitmap=CreateBitmap(shape.Width,mask_height,1,1,bitmap_bits),color_bitmap=NULL;
    if(mask_bitmap&&!(shape.Flags&4u)) {
        struct { DWORD Size; LONG Width,Height; WORD Planes,Bits; DWORD Compression,Bytes;
            LONG X,Y; DWORD Used,Important;
        } bitmap_info={40,shape.Width,-(LONG)shape.Height,1,32,0,0,0,0,0,0};
        void *bits=NULL;
        color_bitmap=CreateDIBSection(NULL,&bitmap_info,0,&bits,NULL,0);
        if(color_bitmap) for(ULONG_PTR i=0;i<count;++i) ((UINT32*)bits)[i]=pixels[i];
    }
    BOOL success=mask_bitmap&&((shape.Flags&4u)||color_bitmap);
    if(success) *info=(ICONINFO){(shape.Flags&2u)!=0,shape.HotX,shape.HotY,mask_bitmap,color_bitmap};
    else { if(mask_bitmap) DeleteObject(mask_bitmap); if(color_bitmap) DeleteObject(color_bitmap); }
    HeapFree(GetProcessHeap(),0,pixels); return success;
}

USER32_API HICON CopyIcon(HICON icon)
{
    SV_CURSOR_DESCRIPTION shape;
    if(!SvNtUserQueryCursor(2,(UINT_PTR)icon,&shape,NULL,NULL,0)) { SetLastError(1402); return NULL; }
    ULONG_PTR count=(ULONG_PTR)shape.Width*shape.Height;
    UINT32 *pixels=HeapAlloc(GetProcessHeap(),0,count*5); if(!pixels) return NULL;
    UINT8 *mask=(UINT8*)(pixels+count);
    HICON copy=NULL;
    if(SvNtUserQueryCursor(2,(UINT_PTR)icon,&shape,pixels,mask,count))
        copy=(HICON)(UINT_PTR)SvNtUserUploadCursor(NULL,&shape,pixels,mask,NULL,0,FALSE);
    HeapFree(GetProcessHeap(),0,pixels); return copy;
}
USER32_API HCURSOR CopyCursor(HCURSOR cursor) { return (HCURSOR)CopyIcon((HICON)cursor); }
