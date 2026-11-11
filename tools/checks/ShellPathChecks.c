#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define __declspec(x)
typedef uint16_t WCHAR;
#include "../../user/w/shlwapi/PathText.inc"
static int equal(const WCHAR *a, const WCHAR *b)
{ while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
int main(void)
{
    WCHAR path[260] = L"C:\\dir.ext\\file";
    assert(PathAddExtensionW(path, L".exe") && equal(path, L"C:\\dir.ext\\file.exe"));
    assert(!PathAddExtensionW(path, L".txt"));
    PathRemoveExtensionW(path);
    assert(equal(path, L"C:\\dir.ext\\file"));
    assert(PathAppendW(path, L"\\more") && equal(path, L"C:\\dir.ext\\file\\more"));
    for (unsigned i = 0; i < 258; ++i) path[i] = 'x'; path[258] = 0;
    assert(!PathAppendW(path, L"x") && path[258] == 0);
    const WCHAR text[] = L"abcdef";
    assert(StrStrW(text, L"cde") == text + 2 && !StrStrW(text, L"zzz"));
    WCHAR number[48];
    assert(StrFormatByteSizeW(1536, number, 48) && equal(number, L"1.50 KB"));
    assert(StrFormatByteSizeW(INT64_MAX, number, 48) && equal(number, L"7.99 EB"));
    assert(StrFormatByteSizeW(100, number, 48) && equal(number, L"100 bytes"));
    assert(!StrFormatByteSizeW(100, number, 2) && !number[0]);
    puts("PASS shell path boundaries, extension handling and byte-size formatting");
}
