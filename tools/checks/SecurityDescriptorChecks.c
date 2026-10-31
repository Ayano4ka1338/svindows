#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t BYTE;
typedef uint16_t WCHAR;
typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
typedef int BOOL_K;
#define TRUE_K 1
#define FALSE_K 0
#define __declspec(x)
static DWORD last_error;
void SetLastError(DWORD error) { last_error = error; }
void *LocalAlloc(DWORD flags, ULONGLONG bytes)
{ (void)flags; return malloc((size_t)bytes); }
void *LocalFree(void *memory) { free(memory); return NULL; }
#include "../../user/w/advapi32/AdvapiSid.inc"
#include "../../user/w/advapi32/AdvapiSecurityDescriptor.inc"

int main(void)
{
    void *raw = NULL;
    DWORD length = 0;
    assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:SYG:BAD:P(A;;GA;;;SY)(A;CI;GR;;;BU)", 1, &raw, &length));
    const BYTE *base = raw;
    const SDDL_RELATIVE_SD *sd = raw;
    assert(length >= 20 && sd->Revision == 1);
    assert(sd->Control & 0x8000 && sd->Control & 0x0004);
    assert(sd->Control & 0x1000);
    assert(sd->Owner && sd->Group && sd->Dacl && !sd->Sacl);
    assert(GetLengthSid(base + sd->Owner) == 12);
    assert(GetLengthSid(base + sd->Group) == 16);
    const BYTE *acl = base + sd->Dacl;
    assert(acl[0] == 2 && *(const uint16_t *)(acl + 4) == 2);
    assert(acl[8] == 0 && *(const DWORD *)(acl + 12) == 0x10000000u);
    const BYTE *second = acl + 8 + *(const uint16_t *)(acl + 10);
    assert(second[0] == 0 && second[1] == 2);
    assert(*(const DWORD *)(second + 4) == 0x80000000u);
    free(raw);
    raw = NULL;
    assert(!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(OA;;GA;;;SY)", 1, &raw, &length));
    assert(raw == NULL && last_error == 1336);
    assert(!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;ZZ;;;SY)", 1, &raw, &length));
    assert(!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GA;;;SY)", 2, &raw, &length));
    assert(last_error == 1305);
    puts("PASS SDDL owner/group, protected DACL, ACE masks and rejected syntax");
    return 0;
}
