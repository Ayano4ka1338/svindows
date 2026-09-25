#include "include/ntseh.h"

typedef void *HANDLE;
typedef int BOOL;
#define STD_OUTPUT_HANDLE ((DWORD)-11)

__declspec(dllimport) HANDLE GetStdHandle(DWORD);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) void *AddVectoredExceptionHandler(
    ULONG, NT_VECTORED_EXCEPTION_HANDLER);
__declspec(dllimport) ULONG RemoveVectoredExceptionHandler(void *);

int SehTriggerDivide(void);
int SehTriggerAccess(void);
void SehDivideResume(void);
void SehAccessResume(void);

static int divide_seen;
static int access_seen;

static void say(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) ++length;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, 0);
}

static LONG exception_handler(NT_EXCEPTION_POINTERS64 *p)
{
    if (p->ExceptionRecord->ExceptionCode == STATUS_INTEGER_DIVIDE_BY_ZERO) {
        divide_seen = 1;
        p->ContextRecord->Rip = (U64)(void *)&SehDivideResume;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (p->ExceptionRecord->ExceptionCode == STATUS_ACCESS_VIOLATION &&
        p->ExceptionRecord->NumberParameters == 2 &&
        p->ExceptionRecord->ExceptionInformation[0] == 1 &&
        p->ExceptionRecord->ExceptionInformation[1] == 0) {
        access_seen = 1;
        p->ContextRecord->Rip = (U64)(void *)&SehAccessResume;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(void)
{
    void *registration = AddVectoredExceptionHandler(1, exception_handler);
    if (!registration || !SehTriggerDivide() || !SehTriggerAccess() ||
        !divide_seen || !access_seen ||
        !RemoveVectoredExceptionHandler(registration)) {
        say("SEH_TEST: FAIL\r\n");
        return 1;
    }
    say("SEH_TEST: PASS\r\n");
    return 0;
}
