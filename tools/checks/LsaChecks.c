#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void *HANDLE;
typedef uint8_t BYTE;
typedef uint16_t WCHAR;
typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
typedef long LONG, NTSTATUS;
typedef int BOOL_K;
#define TRUE_K 1
#define FALSE_K 0
#define __declspec(x)
typedef struct { WCHAR Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
void SetLastError(DWORD error) { (void)error; }
void *LocalAlloc(DWORD flags, ULONGLONG bytes)
{ (void)flags; return malloc((size_t)bytes); }
void *LocalFree(void *memory) { free(memory); return NULL; }
static int wide_len(const WCHAR *str)
{ int n = 0; while (str[n]) n++; return n; }
#include "../../user/w/advapi32/AdvapiSid.inc"
#include "../../user/w/advapi32/AdvapiLsa.inc"

int main(void)
{
    HANDLE policy = 0;
    assert(LsaOpenPolicy(NULL, NULL, 0x800, &policy) == 0 && policy);
    BYTE builtin[32] = {1, 2, 0, 0, 0, 0, 0, 5};
    DWORD parts[] = {32, 545};
    sid_copy(builtin + 8, parts, sizeof(parts));
    BYTE unknown[32] = {1, 1, 0, 0, 0, 0, 0, 5};
    DWORD rid = 777;
    sid_copy(unknown + 8, &rid, 4);
    void *sids[] = {builtin, unknown};
    void *domains = 0, *names = 0;
    assert((DWORD)LsaLookupSids(policy, 2, sids, &domains, &names) == 0x107);
    LSA_DOMAIN_LIST_LOCAL *list = domains;
    LSA_TRANSLATED_NAME_LOCAL *entries = names;
    assert(list->Entries == 1 && entries[0].DomainIndex == 0);
    assert(entries[0].Use == 4 && entries[1].Use == 8);
    assert(entries[1].DomainIndex == -1);
    assert(entries[0].Name.Length == 10);
    assert(entries[0].Name.Buffer[0] == 'U');
    assert(entries[1].Name.Buffer[0] == 'S');
    assert(LsaFreeMemory(domains) == 0 && LsaFreeMemory(names) == 0);
    assert(LsaClose(policy) == 0);
    assert((DWORD)LsaLookupSids(policy, 2, sids, &domains, &names) ==
           0xC0000008u);
    puts("PASS LSA policy lifetime, translated BUILTIN SID and unknown fallback");
    return 0;
}
