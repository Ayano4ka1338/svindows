#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define __declspec(x)
typedef void *HANDLE;
typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
typedef uint8_t BYTE;
typedef uint16_t WCHAR;
typedef int BOOL;
typedef int32_t NTSTATUS;
typedef struct { ULONGLONG Size; DWORD Attributes; } SVINDOWS_FILE_INFO;
#define NT_SUCCESS(s) ((s)>=0)
#define NT_INVALID_HANDLE ((void*)(uintptr_t)-1)
static BYTE source[81920],target[81920];
static DWORD error,source_pos,target_pos;
static int opened,closed,deleted,mode,calls,short_write;
static BOOL cancel_flag;
DWORD GetLastError(void){return error;}
void SetLastError(DWORD e){error=e;}
HANDLE GetProcessHeap(void){return (HANDLE)1;}
void *HeapAlloc(HANDLE heap,DWORD flags,size_t size){(void)heap;(void)flags;return malloc(size);}
BOOL HeapFree(HANDLE heap,DWORD flags,void *p){(void)heap;(void)flags;free(p);return 1;}
HANDLE CreateFileW(const WCHAR *name,DWORD access,DWORD share,void *security,DWORD create,DWORD flags,HANDLE template)
{
    (void)access;(void)share;(void)security;(void)create;(void)flags;(void)template;
    opened++;return (uintptr_t)name==1?(HANDLE)11:(HANDLE)12;
}
BOOL CloseHandle(HANDLE h){assert(h==(HANDLE)11||h==(HANDLE)12);closed++;error=6;return 1;}
NTSTATUS NtQueryFileInfo(HANDLE h,SVINDOWS_FILE_INFO *info){assert(h==(HANDLE)11);*info=(SVINDOWS_FILE_INFO){sizeof(source),0};return 0;}
DWORD ntstatus_to_win32(NTSTATUS status){(void)status;return 5;}
BOOL ReadFile(HANDLE h,void *p,DWORD count,DWORD *read,void *overlapped)
{(void)overlapped;assert(h==(HANDLE)11&&source_pos+count<=sizeof(source));memcpy(p,source+source_pos,count);source_pos+=count;*read=count;return 1;}
BOOL WriteFile(HANDLE h,const void *p,DWORD count,DWORD *written,void *overlapped)
{(void)overlapped;assert(h==(HANDLE)12&&target_pos+count<=sizeof(target));*written=count-short_write;memcpy(target+target_pos,p,*written);target_pos+=*written;return 1;}
BOOL DeleteFileW(const WCHAR *name){assert((uintptr_t)name==2);deleted++;target_pos=0;error=0;return 1;}
#include "../../user/w/kernel32/CopyFileEx.inc"
static DWORD progress(long long total,long long transferred,long long stream_size,long long stream_done,DWORD stream,DWORD reason,HANDLE src,HANDLE dst,void *context)
{
    assert(total==sizeof(source)&&stream_size==total&&stream_done==transferred);
    assert(stream==1&&reason==(calls?0:1)&&src==(HANDLE)11&&dst==(HANDLE)12&&context==(void*)7);
    calls++;
    if(mode==4)cancel_flag=1;
    if(mode==3)return 3;
    return transferred?(DWORD)mode:0;
}
static void reset(int action)
{source_pos=target_pos=0;opened=closed=deleted=calls=short_write=0;mode=action;cancel_flag=0;error=0;}
int main(void)
{
    for(unsigned i=0;i<sizeof(source);i++)source[i]=(BYTE)(i*37);
    reset(0);assert(PrivCopyFileExW((void*)1,(void*)2,progress,(void*)7,&cancel_flag,0));
    assert(calls==3&&opened==2&&closed==2&&!deleted&&target_pos==sizeof(source)&&!memcmp(source,target,sizeof(source)));
    reset(1);assert(!CopyFileExW((void*)1,(void*)2,progress,(void*)7,0,0));assert(error==1235&&deleted==1&&target_pos==0&&closed==2);
    reset(2);assert(!CopyFileExW((void*)1,(void*)2,progress,(void*)7,0,0));assert(error==1235&&!deleted&&target_pos==65536&&closed==2);
    reset(3);assert(CopyFileExW((void*)1,(void*)2,progress,(void*)7,0,0));assert(calls==1&&target_pos==sizeof(source));
    reset(4);assert(!CopyFileExW((void*)1,(void*)2,progress,(void*)7,&cancel_flag,0));assert(error==1235&&source_pos==0&&deleted==1);
    reset(0);short_write=1;assert(!CopyFileExW((void*)1,(void*)2,0,0,0,0));assert(error==29&&deleted==1&&closed==2);
    reset(0);assert(!CopyFileExW((void*)1,(void*)2,0,0,0,0x80000000u));assert(error==50&&!opened);
    puts("PASS copy data, progress, cancel/delete, stop/keep, quiet, short writes and cleanup errors");
}
