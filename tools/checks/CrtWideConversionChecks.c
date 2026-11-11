#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define __declspec(x)
typedef uint16_t WCHAR;
typedef size_t SIZE_T;
static int g_errno;
#include "../../user/w/msvcrt/WideConversion.inc"

int main(void)
{
    unsigned first = 0, second = 0;
    assert(swscanf_s(L"12,34", L"%u,%u", &first, &second) == 2);
    assert(first == 12 && second == 34);
    unsigned long long large = 0;
    assert(swscanf_s(L"18446744073709551615", L"%I64u", &large) == 1 && large == UINT64_MAX);
    assert(swscanf_s(L"0xff 077 42", L"%i %*i %u", &first, &second) == 2);
    assert(first == 255 && second == 42);
    int signed_value = 0;
    assert(swscanf_s(L" -27%", L"%d%%", &signed_value) == 1 && signed_value == -27);
    WCHAR text[5] = {0};
    assert(swscanf_s(L"test", L"%s", text, 5u) == 1 && text[3] == 't' && !text[4]);
    assert(swscanf_s(L"longer", L"%s", text, 5u) == 0 && !text[0] && g_errno == 34);
    assert(swscanf_s(L"abc", L"%2c", text, 5u) == 1 && text[0] == 'a' && text[1] == 'b');
    first = 123;
    assert(swscanf_s(L"", L"%u", &first) == -1 && first == 123);
    assert(swscanf_s(L"x", L"%u", &first) == 0 && first == 123);
    WCHAR number[65];
    assert(!_ui64tow_s(UINT64_MAX, number, 65, 2));
    for (unsigned i = 0; i < 64; ++i) assert(number[i] == '1');
    assert(!number[64]);
    assert(_ui64tow_s(123, number, 3, 10) == 34 && !number[0]);
    assert(_ui64tow_s(123, number, 65, 1) == 22 && !number[0]);
    puts("PASS secure wide integer scanning, text capacities and 64-bit formatting");
}
