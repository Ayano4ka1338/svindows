#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define __declspec(x)
typedef unsigned int DWORD;
typedef uint16_t WCHAR;
typedef void *HANDLE;
static DWORD last_error;
void SetLastError(DWORD error) { last_error = error; }
void *LocalAlloc(DWORD flags, unsigned long long size) { (void)flags; return malloc(size); }
void *LocalFree(void *memory) { free(memory); return 0; }
DWORD GetModuleFileNameW(HANDLE module, WCHAR *out, DWORD capacity)
{
    (void)module; const WCHAR name[] = L"C:\\TASKMGR.EXE";
    unsigned n = sizeof(name) / sizeof(*name);
    assert(n <= capacity); for (unsigned i = 0; i < n; ++i) out[i] = name[i];
    return n - 1;
}
#include "../../user/w/shell32/CommandLine.inc"
static int equal(const WCHAR *a, const WCHAR *b)
{ while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
int main(void)
{
    int count;
    WCHAR **args = CommandLineToArgvW(L"prog \"two words\" tail", &count);
    assert(count == 3 && equal(args[0], L"prog") && equal(args[1], L"two words"));
    assert(equal(args[2], L"tail") && !args[3]); LocalFree(args);
    args = CommandLineToArgvW(L"prog a\\\"b \"a\"\"b\"", &count);
    assert(count == 3 && equal(args[1], L"a\"b") && equal(args[2], L"a\"b")); LocalFree(args);
    args = CommandLineToArgvW(L"  prog", &count);
    assert(count == 2 && !*args[0] && equal(args[1], L"prog")); LocalFree(args);
    args = CommandLineToArgvW(L"", &count);
    assert(count == 1 && equal(args[0], L"C:\\TASKMGR.EXE")); LocalFree(args);
    assert(!CommandLineToArgvW(L"prog", 0) && last_error == 87);
    puts("PASS shell command-line quoting, escaped quotes, leading spaces and LocalFree ownership");
}
