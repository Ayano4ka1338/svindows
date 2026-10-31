/* Binary SID and string conversion using the actual advapi32 implementation. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "../../user/w/advapi32/AdvapiSid.inc"

static int equals(const WCHAR *left, const WCHAR *right)
{
    while (*left && *left == *right) { left++; right++; }
    return *left == *right;
}

int main(void)
{
    void *sid = NULL, *alias = NULL;
    WCHAR *text = NULL;
    assert(ConvertStringSidToSidW(L"S-1-5-32-544", &sid));
    assert(IsValidSid(sid) && GetLengthSid(sid) == 16);
    assert(*GetSidSubAuthorityCount(sid) == 2);
    assert(*GetSidSubAuthority(sid, 0) == 32);
    assert(*GetSidSubAuthority(sid, 1) == 544);
    assert(!GetSidSubAuthority(sid, 2));
    assert(ConvertSidToStringSidW(sid, &text));
    assert(equals(text, L"S-1-5-32-544"));
    free(text);
    assert(ConvertStringSidToSidW(L"BA", &alias));
    assert(EqualSid(sid, alias));
    free(alias);
    BYTE copy[32];
    assert(CopySid(sizeof(copy), copy, sid) && EqualSid(copy, sid));
    assert(!CopySid(8, copy, sid) && last_error == 122);
    DWORD length = 0;
    assert(!CreateWellKnownSid(26, NULL, NULL, &length));
    assert(length == 16 && last_error == 122);
    assert(CreateWellKnownSid(26, NULL, copy, &length));
    assert(EqualSid(copy, sid));
    assert(!CreateWellKnownSid(12345, NULL, copy, &length));
    assert(!ConvertStringSidToSidW(L"S-1-5-4294967296", &alias));
    assert(alias == NULL && last_error == SID_ERROR_INVALID_SID);
    free(sid);
    puts("PASS SID validation, subauthorities, SDDL round trip, aliases and well-known values");
    return 0;
}
