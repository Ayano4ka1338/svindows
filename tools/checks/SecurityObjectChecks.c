/* Shared Se/Ob code checks stored ACLs, typed access and inherited handles. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <svindows/Ke.h>
#include <svindows/Ob.h>
#include <svindows/Ex.h>
static KPROCESS process;
static KTHREAD thread;
static unsigned live;
static int fail_after=-1;
static UINT64 HostIrq(void){return 0;}
static void HostRestore(UINT64 flags){(void)flags;}
static UINT64 HostLock(PKSPIN_LOCK lock){(void)lock;return 0;}
static void HostUnlock(PKSPIN_LOCK lock,UINT64 flags){(void)lock;(void)flags;}
#define KiIrqSave HostIrq
#define KiIrqRestore HostRestore
#define KiAcquireRawSpinLock HostLock
#define KiReleaseRawSpinLock HostUnlock
PKTHREAD KeGetCurrentThread(void){return &thread;}
int KeLog(const char *format,...){(void)format;return 0;}
NORETURN void KeBugCheck(ULONG code,const char *message){fprintf(stderr,"%u %s\n",code,message);abort();}
PVOID ExAllocatePoolWithTag(POOL_TYPE type,SIZE_T bytes,ULONG tag)
{(void)type;(void)tag;if(!fail_after)return NULL;if(fail_after>0)fail_after--;void *p=malloc(bytes);if(p)live++;return p;}
void ExFreePool(PVOID p){if(p){live--;free(p);}}
static struct {UINT64 base;SIZE_T bytes;} ranges[32];
static unsigned range_count;
static void range(void *p,SIZE_T bytes){ranges[range_count].base=(UINT64)p;ranges[range_count++].bytes=bytes;}
BOOLEAN MmProbeForRead(UINT64 p,UINT64 bytes)
{
    for(unsigned i=0;i<range_count;i++)if(p>=ranges[i].base&&p-ranges[i].base<=ranges[i].bytes&&bytes<=ranges[i].bytes-(p-ranges[i].base))return TRUE;
    return FALSE;
}
BOOLEAN MmProbeForWrite(UINT64 p,UINT64 bytes){return MmProbeForRead(p,bytes);}
#include "../../kernel/se/Security.c"
#include "../../kernel/ob/ObCore.c"
#include "../../kernel/ob/ObNamespace.c"
#include "../../kernel/ob/ObHandle.c"

int main(void)
{
    thread.Process=&process;process.SecurityUserRid=1000;ObInitialize();
    process.HandleTable=ObCreateHandleTable();assert(process.HandleTable);
    POBJECT_TYPE type=ObCreateObjectType("FixtureEvent",NULL);assert(type);
    SeConfigureType(type,0x20001u,0x20002u,0x120000u,0x1F0003u);
    unsigned before=live;POBJECT object;assert(NT_SUCCESS(ObCreateObject(type,16,&object)));
    SE_DESCRIPTOR *initial=SeReferenceObjectDescriptor(object);ACCESS_MASK granted=0;
    assert(NT_SUCCESS(SeAccessCheck(initial,1000,GENERIC_ALL,&type->GenericMapping,&granted))&&granted==0x1F0003u);
    assert(SeAccessCheck(initial,2000,GENERIC_READ,&type->GenericMapping,&granted)==STATUS_ACCESS_DENIED);
    puts("PASS token-owner default descriptor grants actual mapped access and denies unrelated subject");

    UINT8 sd[68]={1,0,4,128};ULONG dacl_offset=20;memcpy(sd+16,&dacl_offset,4);
    UINT8 *acl=sd+20;acl[0]=2;UINT16 acl_size=48,ace_count=2;memcpy(acl+2,&acl_size,2);memcpy(acl+4,&ace_count,2);
    UINT8 everyone[12]={1,1,0,0,0,0,0,1,0,0,0,0};
    UINT16 ace_size=20;ULONG write=2,read=1;
    acl[8]=1;memcpy(acl+10,&ace_size,2);memcpy(acl+12,&write,4);memcpy(acl+16,everyone,12);
    acl[28]=0;memcpy(acl+30,&ace_size,2);memcpy(acl+32,&read,4);memcpy(acl+36,everyone,12);
    range(sd,sizeof(sd));SE_DESCRIPTOR *captured=NULL;
    assert(NT_SUCCESS(SeCaptureDescriptor((UINT64)sd,1000,&captured)));
    assert(NT_SUCCESS(SeAccessCheck(captured,2000,1,&type->GenericMapping,&granted))&&granted==1);
    assert(SeAccessCheck(captured,2000,3,&type->GenericMapping,&granted)==STATUS_ACCESS_DENIED);
    ULONG needed=0;UINT8 output[256];range(output,sizeof(output));range(&needed,sizeof(needed));
    assert(SeQueryDescriptor(captured,7,NULL,0,&needed)==STATUS_BUFFER_TOO_SMALL&&needed>20);
    assert(NT_SUCCESS(SeQueryDescriptor(captured,7,output,sizeof(output),&needed)));
    assert(output[0]==1&&(SepReadU16(output+2)&0x8000));
    ULONG saved=SepReadU32(sd+16);ULONG bad_offset=0x10000000;memcpy(sd+16,&bad_offset,4);
    SE_DESCRIPTOR *bad=NULL;assert(SeCaptureDescriptor((UINT64)sd,1000,&bad)==STATUS_ACCESS_VIOLATION&&!bad);
    memcpy(sd+16,&saved,4);
    puts("PASS captured self-relative allow/deny ACLs, sizing query and invalid offset validation");

    HANDLE handle;assert(NT_SUCCESS(ObCreateHandle(object,GENERIC_ALL,&handle)));
    assert(NT_SUCCESS(ObSetHandleAttributesInTable(process.HandleTable,handle,3,3)));
    assert(ObCloseHandle(handle)==STATUS_HANDLE_NOT_CLOSABLE);
    POB_HANDLE_TABLE child=ObCreateHandleTable();assert(child);
    assert(NT_SUCCESS(ObInheritHandles(process.HandleTable,child)));
    POBJECT inherited;ACCESS_MASK inherited_access;
    assert(NT_SUCCESS(ObReferenceObjectByHandleInTable(child,handle,2,type,&inherited,&inherited_access))&&inherited==object);
    assert(inherited_access==type->GenericMapping.All);ObDereferenceObject(inherited);
    ULONG attributes=0;assert(NT_SUCCESS(ObQueryHandleAttributesInTable(child,handle,&attributes))&&attributes==3);
    puts("PASS real protection/inheritance flags preserve handle value, object reference and granted mask");

    SeAssignObjectDescriptor(object,captured);
    SE_DESCRIPTOR *current=SeReferenceObjectDescriptor(object);
    assert(current!=initial&&NT_SUCCESS(SeAccessCheck(initial,1000,GENERIC_ALL,&type->GenericMapping,&granted)));
    assert(SeAccessCheck(current,2000,2,&type->GenericMapping,&granted)==STATUS_ACCESS_DENIED);
    SeDereferenceDescriptor(current);SeDereferenceDescriptor(initial);SeDereferenceDescriptor(captured);
    ObDestroyHandleTable(child);
    assert(NT_SUCCESS(ObSetHandleAttributesInTable(process.HandleTable,handle,2,0)));
    assert(NT_SUCCESS(ObCloseHandle(handle)));ObDereferenceObject(object);
    assert(live==before);
    puts("PASS descriptor replacement retains old snapshots and final object/handle cleanup balances resources");
    return 0;
}
