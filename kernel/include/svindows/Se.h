/* Shared executive security descriptors and access checks. */
#ifndef SVINDOWS_SE_H
#define SVINDOWS_SE_H
#include <nt/Ntdef.h>
#include <nt/Ntstatus.h>
struct _KPROCESS;
struct _OBJECT_TYPE;
typedef struct _SE_DESCRIPTOR SE_DESCRIPTOR;
typedef struct _SE_GENERIC_MAPPING { ACCESS_MASK Read,Write,Execute,All; } SE_GENERIC_MAPPING;
NTSTATUS SeCreateDefaultDescriptor(ULONG user_rid, SE_DESCRIPTOR **out);
NTSTATUS SeCreateUnrestrictedDescriptor(ULONG owner_rid, SE_DESCRIPTOR **out);
NTSTATUS SeCaptureDescriptor(UINT64 user_address, ULONG default_owner, SE_DESCRIPTOR **out);
NTSTATUS SeCaptureAttributes(UINT64 user_address, ULONG default_owner,
    SE_DESCRIPTOR **descriptor, BOOLEAN *inherit);
void SeReferenceDescriptor(SE_DESCRIPTOR *descriptor);
void SeDereferenceDescriptor(SE_DESCRIPTOR *descriptor);
ACCESS_MASK SeMapGenericAccess(ACCESS_MASK access, const SE_GENERIC_MAPPING *mapping);
NTSTATUS SeAccessCheck(SE_DESCRIPTOR *descriptor, ULONG user_rid, ACCESS_MASK desired,
    const SE_GENERIC_MAPPING *mapping, ACCESS_MASK *granted);
NTSTATUS SeQueryDescriptor(SE_DESCRIPTOR *descriptor, ULONG information,
    void *output, ULONG capacity, ULONG *needed);
NTSTATUS SeMergeDescriptor(SE_DESCRIPTOR *old_descriptor, SE_DESCRIPTOR *replacement,
    ULONG information, SE_DESCRIPTOR **out);
ULONG SeDescriptorOwnerRid(SE_DESCRIPTOR *descriptor);
void SeConfigureType(struct _OBJECT_TYPE *type, ACCESS_MASK read, ACCESS_MASK write,
    ACCESS_MASK execute, ACCESS_MASK all);
void SeAssignObjectDescriptor(PVOID object, SE_DESCRIPTOR *descriptor);
SE_DESCRIPTOR *SeReferenceObjectDescriptor(PVOID object);
const UINT8 *SeDescriptorOwnerSid(SE_DESCRIPTOR *descriptor, ULONG *bytes);
UINT64 NtQuerySecurityObject(UINT64 *args);
UINT64 NtSetSecurityObject(UINT64 *args);
#endif
