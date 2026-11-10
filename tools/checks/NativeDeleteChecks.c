#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef void *HANDLE;
typedef uint32_t ULONG;
typedef int32_t NTSTATUS;
typedef uint64_t ULONGLONG;
typedef uint8_t BYTE;
typedef struct {uint16_t Length,MaximumLength;uint16_t *Buffer;} UNICODE_STRING_NT;
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xc0000002u)
static NTSTATUS open_result,set_result;
static int opened,marked,closed;
static const void *expected_attributes;
NTSTATUS NtOpenFile(HANDLE *file,ULONG access,const void *attributes,void *io,ULONG share,ULONG options)
{
    assert(access==0x110000&&attributes==expected_attributes&&io&&share==7&&options==0x20);
    opened++;if(open_result>=0)*file=(HANDLE)1;return open_result;
}
NTSTATUS NtSetInformationFile(HANDLE file,void *io,const void *data,ULONG size,ULONG type)
{assert(file==(HANDLE)1&&io&&size==1&&type==13&&*(const BYTE*)data==1);marked++;return set_result;}
NTSTATUS NtClose(HANDLE file){assert(file==(HANDLE)1);closed++;return 0;}
#include "../../user/w/ntdll/Rtl/FileCompat.inc"
int main(void)
{
    int attributes=0;expected_attributes=&attributes;
    open_result=(NTSTATUS)0xc0000022u;
    assert(NtDeleteFile(&attributes)==open_result&&opened==1&&!marked&&!closed);
    open_result=0;set_result=(NTSTATUS)0xc0000121u;
    assert(NtDeleteFile(&attributes)==set_result&&marked==1&&closed==1);
    set_result=0;
    assert(!NtDeleteFile(&attributes)&&marked==2&&closed==2);
    puts("PASS native delete preserves open/disposition errors and closes every successful open");
}
