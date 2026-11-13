/* Executive descriptors are captured into one canonical self-relative block.
 * Handles carry mapped access; later opens evaluate the stored descriptor. */
#include <svindows/Se.h>
#include <svindows/Ob.h>
#include <svindows/Ke.h>
#include <svindows/Mm.h>
#include <svindows/Ex.h>
#include <svindows/Rtl.h>

#define SE_DACL_PRESENT 0x0004u
#define SE_SACL_PRESENT 0x0010u
#define SE_SELF_RELATIVE 0x8000u
#define SE_OWNER_INFORMATION 1u
#define SE_GROUP_INFORMATION 2u
#define SE_DACL_INFORMATION 4u
#define SE_SACL_INFORMATION 8u
#define SE_READ_CONTROL 0x00020000u
#define SE_WRITE_DAC 0x00040000u
#define SE_WRITE_OWNER 0x00080000u
#define SE_ACCESS_SYSTEM_SECURITY 0x01000000u
#define SE_MAXIMUM_ALLOWED 0x02000000u

struct _SE_DESCRIPTOR { volatile LONG References; ULONG Bytes; UINT8 Data[]; };
typedef struct _SE_PARTS { UINT16 Control; const UINT8 *Owner,*Group,*Sacl,*Dacl;
    ULONG OwnerBytes,GroupBytes,SaclBytes,DaclBytes; } SE_PARTS;
static KSPIN_LOCK g_security_lock;

static ULONG SepReadU32(const void *source) { ULONG value; memcpy(&value,source,4); return value; }
static UINT16 SepReadU16(const void *source) { UINT16 value; memcpy(&value,source,2); return value; }
static ULONG SepSidBytes(const UINT8 *sid)
{ return sid && sid[0]==1 && sid[1]<=15 ? 8u+sid[1]*4u : 0; }
static BOOLEAN SepSameSid(const UINT8 *a,const UINT8 *b)
{ ULONG bytes=SepSidBytes(a); return bytes && bytes==SepSidBytes(b) && !memcmp(a,b,bytes); }
static ULONG SepUserSid(UINT8 *output,ULONG rid)
{
    static const UINT8 prefix[8]={1,5,0,0,0,0,0,5};
    static const ULONG parts[4]={21,1,2,3};
    memcpy(output,prefix,8);memcpy(output+8,parts,16);memcpy(output+24,&rid,4);return 28;
}
static BOOLEAN SepSubjectSid(const UINT8 *sid,ULONG rid,const UINT8 *owner)
{
    ULONG bytes=SepSidBytes(sid); if(!bytes) return FALSE;
    for(ULONG i=2;i<7;i++) if(sid[i]) return FALSE;
    const UINT8 *parts=sid+8;
    if(sid[7]==1&&sid[1]==1&&!SepReadU32(parts)) return TRUE; /* Everyone */
    if(sid[7]==3&&sid[1]==1&&!SepReadU32(parts)) {
        UINT8 current[28];SepUserSid(current,rid);return SepSameSid(owner,current);
    }
    if(sid[7]!=5) return FALSE;
    if(sid[1]==1&&(SepReadU32(parts)==11||SepReadU32(parts)==4)) return TRUE;
    if(sid[1]==2&&SepReadU32(parts)==32&&SepReadU32(parts+4)==545) return TRUE;
    return sid[1]==5&&SepReadU32(parts)==21&&SepReadU32(parts+4)==1&&
        SepReadU32(parts+8)==2&&SepReadU32(parts+12)==3&&SepReadU32(parts+16)==rid;
}
static const UINT8 *SepAceSid(const UINT8 *ace,ULONG bytes)
{
    ULONG offset=8;
    if(ace[0]==5||ace[0]==6||ace[0]==7||ace[0]==8||ace[0]==11||ace[0]==12||ace[0]==15||ace[0]==16) {
        if(bytes<12) return NULL;
        ULONG flags=SepReadU32(ace+8);if(flags&~3u) return NULL;
        offset=12+((flags&1)?16:0)+((flags&2)?16:0);
    }
    if(offset>bytes||bytes-offset<8) return NULL;
    ULONG length=SepSidBytes(ace+offset);
    return length && length<=bytes-offset ? ace+offset : NULL;
}
static NTSTATUS SepValidateAcl(const UINT8 *acl,ULONG bytes)
{
    if(bytes<8||(acl[0]!=2&&acl[0]!=4)||SepReadU16(acl+2)!=bytes) return STATUS_INVALID_PARAMETER;
    ULONG offset=8,count=SepReadU16(acl+4);
    for(ULONG i=0;i<count;i++) {
        if(offset>bytes||bytes-offset<8) return STATUS_INVALID_PARAMETER;
        ULONG size=SepReadU16(acl+offset+2);
        if(size<16||(size&3)||size>bytes-offset||!SepAceSid(acl+offset,size)) return STATUS_INVALID_PARAMETER;
        offset+=size;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS SepCapturePart(UINT64 address,BOOLEAN acl,UINT8 **copy,ULONG *bytes)
{
    *copy=NULL;*bytes=0;if(!address) return STATUS_SUCCESS;
    if(!MmProbeForRead(address,8)) return STATUS_ACCESS_VIOLATION;
    ULONG length=acl?SepReadU16((void *)(address+2)):SepSidBytes((void *)address);
    if(!length||!MmProbeForRead(address,length)) return STATUS_INVALID_PARAMETER;
    UINT8 *storage=ExAllocatePoolWithTag(NonPagedPool,length,'traS');
    if(!storage) return STATUS_NO_MEMORY;
    memcpy(storage,(void *)address,length);
    if(acl&&!NT_SUCCESS(SepValidateAcl(storage,length))) {ExFreePool(storage);return STATUS_INVALID_PARAMETER;}
    *copy=storage;*bytes=length;return STATUS_SUCCESS;
}
static void SepDescriptorParts(SE_DESCRIPTOR *descriptor,SE_PARTS *parts)
{
    memset(parts,0,sizeof(*parts)); if(!descriptor) return;
    UINT8 *data=descriptor->Data;parts->Control=SepReadU16(data+2);
    ULONG offsets[4];memcpy(offsets,data+4,16);
    if(offsets[0]) {parts->Owner=data+offsets[0];parts->OwnerBytes=SepSidBytes(parts->Owner);}
    if(offsets[1]) {parts->Group=data+offsets[1];parts->GroupBytes=SepSidBytes(parts->Group);}
    if(offsets[2]) {parts->Sacl=data+offsets[2];parts->SaclBytes=SepReadU16(parts->Sacl+2);}
    if(offsets[3]) {parts->Dacl=data+offsets[3];parts->DaclBytes=SepReadU16(parts->Dacl+2);}
}
static NTSTATUS SepBuildDescriptor(const SE_PARTS *parts,SE_DESCRIPTOR **out)
{
    ULONG sizes[4]={parts->OwnerBytes,parts->GroupBytes,parts->SaclBytes,parts->DaclBytes};
    const UINT8 *source[4]={parts->Owner,parts->Group,parts->Sacl,parts->Dacl};
    ULONG bytes=20;
    for(ULONG i=0;i<4;i++) {if(sizes[i]>0xFFFFFFFFu-bytes-3) return STATUS_NO_MEMORY;bytes+=(sizes[i]+3)&~3u;}
    SE_DESCRIPTOR *descriptor=ExAllocatePoolWithTag(NonPagedPool,sizeof(*descriptor)+bytes,'ceDS');
    if(!descriptor) return STATUS_NO_MEMORY;
    descriptor->References=1;descriptor->Bytes=bytes;memset(descriptor->Data,0,bytes);
    descriptor->Data[0]=1;UINT16 control=parts->Control|SE_SELF_RELATIVE;memcpy(descriptor->Data+2,&control,2);
    ULONG offset=20;
    for(ULONG i=0;i<4;i++) if(sizes[i]) {
        memcpy(descriptor->Data+4+i*4,&offset,4);memcpy(descriptor->Data+offset,source[i],sizes[i]);offset+=(sizes[i]+3)&~3u;
    }
    *out=descriptor;return STATUS_SUCCESS;
}
NTSTATUS SeCreateDefaultDescriptor(ULONG rid,SE_DESCRIPTOR **out)
{
    UINT8 owner[28],group[16]={1,2,0,0,0,0,0,5},acl[44]={2,0,44,0,1,0,0,0};
    ULONG group_parts[2]={32,545};memcpy(group+8,group_parts,8);SepUserSid(owner,rid);
    acl[8]=0;UINT16 size=36;memcpy(acl+10,&size,2);ULONG all=GENERIC_ALL;memcpy(acl+12,&all,4);memcpy(acl+16,owner,28);
    SE_PARTS parts={.Control=SE_DACL_PRESENT,.Owner=owner,.OwnerBytes=28,.Group=group,.GroupBytes=16,.Dacl=acl,.DaclBytes=44};
    return SepBuildDescriptor(&parts,out);
}
void SeReferenceDescriptor(SE_DESCRIPTOR *descriptor)
{if(descriptor) InterlockedIncrement(&descriptor->References);}
NTSTATUS SeCreateUnrestrictedDescriptor(ULONG rid,SE_DESCRIPTOR **out)
{
    UINT8 owner[28],group[16]={1,2,0,0,0,0,0,5};ULONG group_parts[2]={32,545};
    SepUserSid(owner,rid);memcpy(group+8,group_parts,8);
    SE_PARTS parts={.Control=SE_DACL_PRESENT,.Owner=owner,.OwnerBytes=28,.Group=group,.GroupBytes=16};
    return SepBuildDescriptor(&parts,out);
}
void SeDereferenceDescriptor(SE_DESCRIPTOR *descriptor)
{if(descriptor&&!InterlockedDecrement(&descriptor->References)) ExFreePool(descriptor);}
static NTSTATUS SepCaptureDescriptor(UINT64 address,ULONG default_owner,
    BOOLEAN default_missing,SE_DESCRIPTOR **out)
{
    if(!out) return STATUS_INVALID_PARAMETER;
    *out=NULL;
    if(!address) return SeCreateDefaultDescriptor(default_owner,out);
    if(!MmProbeForRead(address,4)) return STATUS_ACCESS_VIOLATION;
    UINT8 revision=*(UINT8 *)address;UINT16 control=SepReadU16((void *)(address+2));
    if(revision!=1) return STATUS_INVALID_PARAMETER;
    UINT64 pointers[4]={0};
    if(control&SE_SELF_RELATIVE) {
        if(!MmProbeForRead(address,20)) return STATUS_ACCESS_VIOLATION;
        for(ULONG i=0;i<4;i++) {ULONG offset=SepReadU32((void *)(address+4+i*4));if(offset) {if(offset<20||address+offset<address) return STATUS_INVALID_PARAMETER;pointers[i]=address+offset;}}
    } else {
        if(!MmProbeForRead(address,40)) return STATUS_ACCESS_VIOLATION;
        memcpy(pointers,(void *)(address+8),32);
    }
    if(!(control&SE_SACL_PRESENT)) pointers[2]=0;
    if(!(control&SE_DACL_PRESENT)) pointers[3]=0;
    UINT8 *copy[4]={0};ULONG bytes[4]={0};NTSTATUS status=STATUS_SUCCESS;
    for(ULONG i=0;i<4;i++) {status=SepCapturePart(pointers[i],i>=2,&copy[i],&bytes[i]);if(!NT_SUCCESS(status)) break;}
    UINT8 owner[28],group[16]={1,2,0,0,0,0,0,5};ULONG group_parts[2]={32,545};memcpy(group+8,group_parts,8);SepUserSid(owner,default_owner);
    if(NT_SUCCESS(status)) {
        SE_PARTS parts={.Control=control,.Owner=copy[0]?copy[0]:default_missing?owner:NULL,.OwnerBytes=copy[0]?bytes[0]:default_missing?28:0,
            .Group=copy[1]?copy[1]:default_missing?group:NULL,.GroupBytes=copy[1]?bytes[1]:default_missing?16:0,
            .Sacl=copy[2],.SaclBytes=bytes[2],.Dacl=copy[3],.DaclBytes=bytes[3]};
        status=SepBuildDescriptor(&parts,out);
    }
    for(ULONG i=0;i<4;i++) if(copy[i]) ExFreePool(copy[i]);
    return status;
}
NTSTATUS SeCaptureDescriptor(UINT64 address,ULONG owner,SE_DESCRIPTOR **out)
{return SepCaptureDescriptor(address,owner,TRUE,out);}
NTSTATUS SeCaptureAttributes(UINT64 address,ULONG rid,SE_DESCRIPTOR **descriptor,BOOLEAN *inherit)
{
    if(!inherit) return STATUS_INVALID_PARAMETER;
    *inherit=FALSE;
    if(!address) return SeCreateDefaultDescriptor(rid,descriptor);
    if(!MmProbeForRead(address,24)) return STATUS_ACCESS_VIOLATION;
    if(SepReadU32((void *)address)!=24) return STATUS_INVALID_PARAMETER;
    UINT64 pointer;memcpy(&pointer,(void *)(address+8),8);*inherit=SepReadU32((void *)(address+16))!=0;
    NTSTATUS status=SeCaptureDescriptor(pointer,rid,descriptor);
    if(NT_SUCCESS(status)&&pointer) {
        SE_PARTS parts;SepDescriptorParts(*descriptor,&parts);UINT8 owner[28];SepUserSid(owner,rid);
        if(!SepSameSid(parts.Owner,owner)) {SeDereferenceDescriptor(*descriptor);*descriptor=NULL;return STATUS_INVALID_OWNER;}
    }
    return status;
}
ACCESS_MASK SeMapGenericAccess(ACCESS_MASK access,const SE_GENERIC_MAPPING *mapping)
{
    ACCESS_MASK mapped=access&0x0FFFFFFFu;
    if(access&0x80000000u) mapped|=mapping->Read;
    if(access&0x40000000u) mapped|=mapping->Write;
    if(access&0x20000000u) mapped|=mapping->Execute;
    if(access&0x10000000u) mapped|=mapping->All;
    return mapped;
}
NTSTATUS SeAccessCheck(SE_DESCRIPTOR *descriptor,ULONG rid,ACCESS_MASK desired,
    const SE_GENERIC_MAPPING *mapping,ACCESS_MASK *granted)
{
    if(!descriptor||!mapping||!granted) return STATUS_INVALID_PARAMETER;
    desired=SeMapGenericAccess(desired,mapping);*granted=0;
    if(desired&SE_ACCESS_SYSTEM_SECURITY) return STATUS_PRIVILEGE_NOT_HELD;
    if(desired&~(mapping->All|SE_MAXIMUM_ALLOWED)) return STATUS_ACCESS_DENIED;
    BOOLEAN maximum=(desired&SE_MAXIMUM_ALLOWED)!=0;desired&=~SE_MAXIMUM_ALLOWED;
    SE_PARTS parts;SepDescriptorParts(descriptor,&parts);
    UINT8 current[28];SepUserSid(current,rid);ACCESS_MASK implicit=SepSameSid(parts.Owner,current)?
        SE_READ_CONTROL|SE_WRITE_DAC:0;
    ACCESS_MASK allowed=implicit,denied=0,remaining=maximum?mapping->All:desired&~implicit;
    if(!(parts.Control&SE_DACL_PRESENT)||!parts.Dacl) {allowed|=mapping->All;remaining=0;}
    else {
        ULONG offset=8,count=SepReadU16(parts.Dacl+4);
        for(ULONG i=0;i<count&&(maximum||remaining);i++) {
            const UINT8 *ace=parts.Dacl+offset;ULONG size=SepReadU16(ace+2);offset+=size;
            if(ace[1]&8u) continue; /* inherit-only */
            const UINT8 *sid=SepAceSid(ace,size);if(!SepSubjectSid(sid,rid,parts.Owner)) continue;
            ACCESS_MASK rights=SeMapGenericAccess(SepReadU32(ace+4),mapping)&mapping->All;
            if((ace[0]==5||ace[0]==6)&&(SepReadU32(ace+8)&1u)) continue; /* object GUID does not name this executive type */
            if(ace[0]==0||ace[0]==5) {allowed|=rights&~denied;remaining&=~rights;}
            else if(ace[0]==1||ace[0]==6) {
                if(!maximum&&(remaining&rights)) return STATUS_ACCESS_DENIED;
                denied|=rights&~allowed;remaining&=~rights;
            } else if(ace[0]==9||ace[0]==10||ace[0]==11||ace[0]==12) {
                /* A callback ACE requires a condition/security callback. An
                 * unevaluated allow is never turned into an access grant. */
                if(ace[0]==10||ace[0]==12) {denied|=rights;remaining&=~rights;}
            }
        }
    }
    if(parts.Sacl) {
        ULONG offset=8,count=SepReadU16(parts.Sacl+4);
        for(ULONG i=0;i<count;i++) {
            const UINT8 *ace=parts.Sacl+offset;ULONG size=SepReadU16(ace+2);offset+=size;
            if(ace[0]!=17) continue;
            const UINT8 *sid=SepAceSid(ace,size);
            if(!sid||sid[7]!=16||sid[1]!=1||SepReadU32(sid+8)<=8192) continue;
            ULONG policy=SepReadU32(ace+4);ACCESS_MASK restricted=0;
            if(policy&1) restricted|=mapping->Write;
            if(policy&2) restricted|=mapping->Read;
            if(policy&4) restricted|=mapping->Execute;
            allowed&=~restricted;if(!maximum&&(desired&restricted)) return STATUS_ACCESS_DENIED;
        }
    }
    if(!maximum&&(remaining||((allowed&desired)!=desired))) return STATUS_ACCESS_DENIED;
    *granted=maximum?(allowed&~denied&mapping->All):desired;return STATUS_SUCCESS;
}
NTSTATUS SeQueryDescriptor(SE_DESCRIPTOR *descriptor,ULONG information,void *output,ULONG capacity,ULONG *needed)
{
    if(!descriptor||!needed||!information||(information&~15u)) return STATUS_INVALID_PARAMETER;
    SE_PARTS parts;SepDescriptorParts(descriptor,&parts);
    if(!(information&1)) {parts.Owner=NULL;parts.OwnerBytes=0;}
    if(!(information&2)) {parts.Group=NULL;parts.GroupBytes=0;}
    if(!(information&4)) {parts.Dacl=NULL;parts.DaclBytes=0;parts.Control&=~SE_DACL_PRESENT;}
    if(!(information&8)) {parts.Sacl=NULL;parts.SaclBytes=0;parts.Control&=~SE_SACL_PRESENT;}
    SE_DESCRIPTOR *copy;NTSTATUS status=SepBuildDescriptor(&parts,&copy);if(!NT_SUCCESS(status)) return status;
    *needed=copy->Bytes;if(!output||capacity<copy->Bytes) status=STATUS_BUFFER_TOO_SMALL;
    else memcpy(output,copy->Data,copy->Bytes);
    SeDereferenceDescriptor(copy);return status;
}
NTSTATUS SeMergeDescriptor(SE_DESCRIPTOR *old,SE_DESCRIPTOR *replacement,ULONG information,SE_DESCRIPTOR **out)
{
    if(!old||!replacement||!out||!information||(information&~15u)) return STATUS_INVALID_PARAMETER;
    SE_PARTS current,changed;SepDescriptorParts(old,&current);SepDescriptorParts(replacement,&changed);
    if(information&1) {current.Owner=changed.Owner;current.OwnerBytes=changed.OwnerBytes;}
    if(information&2) {current.Group=changed.Group;current.GroupBytes=changed.GroupBytes;}
    if(information&4) {current.Dacl=changed.Dacl;current.DaclBytes=changed.DaclBytes;current.Control=(current.Control&~SE_DACL_PRESENT)|(changed.Control&SE_DACL_PRESENT);}
    if(information&8) {current.Sacl=changed.Sacl;current.SaclBytes=changed.SaclBytes;current.Control=(current.Control&~SE_SACL_PRESENT)|(changed.Control&SE_SACL_PRESENT);}
    return SepBuildDescriptor(&current,out);
}
ULONG SeDescriptorOwnerRid(SE_DESCRIPTOR *descriptor)
{
    SE_PARTS parts;SepDescriptorParts(descriptor,&parts);
    const UINT8 *sid=parts.Owner;
    return sid&&sid[7]==5&&sid[1]==5&&SepReadU32(sid+8)==21&&SepReadU32(sid+12)==1&&
        SepReadU32(sid+16)==2&&SepReadU32(sid+20)==3?SepReadU32(sid+24):0xFFFFFFFFu;
}
const UINT8 *SeDescriptorOwnerSid(SE_DESCRIPTOR *descriptor,ULONG *bytes)
{
    SE_PARTS parts;SepDescriptorParts(descriptor,&parts);
    if(bytes) *bytes=parts.OwnerBytes;
    return parts.Owner;
}
void SeConfigureType(POBJECT_TYPE type,ACCESS_MASK read,ACCESS_MASK write,ACCESS_MASK execute,ACCESS_MASK all)
{if(type) {type->GenericMapping=(SE_GENERIC_MAPPING){read,write,execute,all};type->ValidAccessMask=all;}}
void SeAssignObjectDescriptor(PVOID object,SE_DESCRIPTOR *descriptor)
{
    POBJECT_HEADER header=ObHeaderFromObject(object);SeReferenceDescriptor(descriptor);
    UINT64 flags=KiAcquireRawSpinLock(&g_security_lock);
    SE_DESCRIPTOR *old=header->SecurityDescriptor;header->SecurityDescriptor=descriptor;
    KiReleaseRawSpinLock(&g_security_lock,flags);SeDereferenceDescriptor(old);
}
SE_DESCRIPTOR *SeReferenceObjectDescriptor(PVOID object)
{
    UINT64 flags=KiAcquireRawSpinLock(&g_security_lock);
    SE_DESCRIPTOR *descriptor=ObHeaderFromObject(object)->SecurityDescriptor;SeReferenceDescriptor(descriptor);
    KiReleaseRawSpinLock(&g_security_lock,flags);return descriptor;
}
UINT64 NtQuerySecurityObject(UINT64 *args)
{
    ULONG information=(ULONG)args[1];ULONG *needed=(ULONG *)args[4];
    if(!needed||!MmProbeForWrite((UINT64)needed,4)) return (UINT64)STATUS_ACCESS_VIOLATION;
    ACCESS_MASK access=(information&8)?SE_ACCESS_SYSTEM_SECURITY:SE_READ_CONTROL;
    POBJECT object;NTSTATUS status=ObReferenceObjectByHandle((HANDLE)args[0],access,NULL,&object);
    if(!NT_SUCCESS(status)) return (UINT64)status;
    SE_DESCRIPTOR *descriptor=SeReferenceObjectDescriptor(object);
    ULONG size=0;status=SeQueryDescriptor(descriptor,information,NULL,0,&size);
    *needed=size;
    if(status==STATUS_BUFFER_TOO_SMALL&&args[2]&&(ULONG)args[3]>=size) {
        if(!MmProbeForWrite(args[2],size)) status=STATUS_ACCESS_VIOLATION;
        else status=SeQueryDescriptor(descriptor,information,(void *)args[2],(ULONG)args[3],&size);
    }
    SeDereferenceDescriptor(descriptor);ObDereferenceObject(object);return (UINT64)status;
}
UINT64 NtSetSecurityObject(UINT64 *args)
{
    ULONG information=(ULONG)args[1];
    if(!information||(information&~15u)) return (UINT64)STATUS_INVALID_PARAMETER;
    ACCESS_MASK access=(information&4)?SE_WRITE_DAC:0;
    if(information&3) access|=SE_WRITE_OWNER;
    if(information&8) access|=SE_ACCESS_SYSTEM_SECURITY;
    POBJECT object;NTSTATUS status=ObReferenceObjectByHandle((HANDLE)args[0],access,NULL,&object);
    if(!NT_SUCCESS(status)) return (UINT64)status;
    PKTHREAD caller=KeGetCurrentThread();ULONG rid=caller&&caller->Process?caller->Process->SecurityUserRid:0;
    SE_DESCRIPTOR *captured=NULL,*merged=NULL;
    status=args[2]?SepCaptureDescriptor(args[2],rid,FALSE,&captured):STATUS_INVALID_PARAMETER;
    if(NT_SUCCESS(status)&&(information&3)) {
        SE_PARTS changed;SepDescriptorParts(captured,&changed);UINT8 current[28];SepUserSid(current,rid);
        if((information&1)&&!SepSameSid(changed.Owner,current)) status=STATUS_INVALID_OWNER;
        if((information&2)&&!SepSubjectSid(changed.Group,rid,current)) status=STATUS_INVALID_PARAMETER;
    }
    while(NT_SUCCESS(status)) {
        SE_DESCRIPTOR *previous=SeReferenceObjectDescriptor(object);
        status=SeMergeDescriptor(previous,captured,information,&merged);
        if(!NT_SUCCESS(status)){SeDereferenceDescriptor(previous);break;}
        UINT64 flags=KiAcquireRawSpinLock(&g_security_lock);
        BOOLEAN unchanged=ObHeaderFromObject(object)->SecurityDescriptor==previous;
        if(unchanged){SeReferenceDescriptor(merged);ObHeaderFromObject(object)->SecurityDescriptor=merged;}
        KiReleaseRawSpinLock(&g_security_lock,flags);
        if(unchanged) SeDereferenceDescriptor(previous); /* former object reference */
        SeDereferenceDescriptor(previous); /* local snapshot */
        if(unchanged) break;
        SeDereferenceDescriptor(merged);merged=NULL;
    }
    SeDereferenceDescriptor(merged);SeDereferenceDescriptor(captured);ObDereferenceObject(object);return (UINT64)status;
}
