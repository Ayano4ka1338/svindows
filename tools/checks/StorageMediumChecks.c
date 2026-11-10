#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define __declspec(x)
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int BOOL;
typedef void *HANDLE;
static int released,freed,deleted,graphics,graphics_available;
static DWORD error;
static void *container;
void SetLastError(DWORD value){error=value;}
HANDLE GetModuleHandleW(const void *name){(void)name;return (HANDLE)1;}
static BOOL delete_graphic(HANDLE h){assert(h);graphics++;return 1;}
void (*GetProcAddress(HANDLE module,const char *name))(void)
{(void)module;(void)name;return graphics_available?(void (*)(void))delete_graphic:0;}
BOOL DeleteFileW(const void *path){assert(path);deleted++;return 1;}
void CoTaskMemFree(void *p){assert(p);freed++;}
HANDLE GlobalFree(HANDLE p){assert(p);freed++;return 0;}
void *GlobalLock(HANDLE p){return p;}
BOOL GlobalUnlock(HANDLE p){assert(p);return 0;}
#include "../../user/w/kernel32/StorageMedium.inc"
static DWORD release_object(STORAGE_UNKNOWN *object)
{
    assert(object);released++;
    if(container){free(container);container=0;}
    return 0;
}
int main(void)
{
    STORAGE_UNKNOWN_METHODS methods={0,0,release_object};
    STORAGE_UNKNOWN owner={&methods},stream={&methods};
    STORAGE_MEDIUM value={1,(void*)1,0};
    ReleaseStgMedium(&value);assert(freed==1&&!value.Type&&!value.Data);
    ReleaseStgMedium(&value);assert(freed==1);
    value=(STORAGE_MEDIUM){1,(void*)1,&owner};
    ReleaseStgMedium(&value);assert(freed==1&&released==1);
    value=(STORAGE_MEDIUM){4,&stream,&owner};
    ReleaseStgMedium(&value);assert(released==3);
    value=(STORAGE_MEDIUM){2,(void*)1,&owner};
    ReleaseStgMedium(&value);assert(freed==2&&deleted==0&&released==4);
    value=(STORAGE_MEDIUM){2,(void*)1,0};
    ReleaseStgMedium(&value);assert(freed==3&&deleted==1);
    value=(STORAGE_MEDIUM){16,(void*)1,0};
    ReleaseStgMedium(&value);assert(error==50&&value.Type==16&&graphics==0);
    graphics_available=1;
    ReleaseStgMedium(&value);assert(graphics==1&&!value.Type);
    STORAGE_METAFILE picture={0,0,0,(void*)1};
    value=(STORAGE_MEDIUM){32,&picture,0};
    ReleaseStgMedium(&value);assert(graphics==2&&freed==4);
    STORAGE_MEDIUM *owned=malloc(sizeof(*owned));assert(owned);
    *owned=(STORAGE_MEDIUM){8,&stream,&owner};container=owned;
    ReleaseStgMedium(owned);assert(!container&&released==6);
    puts("PASS medium ownership, streams, file deletion, graphics failure and reentrant destruction");
}
