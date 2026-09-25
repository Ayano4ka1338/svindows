typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
#define STD_OUTPUT_HANDLE ((DWORD)-11)

__declspec(dllimport) HANDLE GetStdHandle(DWORD);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) void NtDisplayString(const char *);

static volatile int g_caught;
static volatile int g_filtered;
static volatile int g_filter_called;
static volatile int g_unwind_sequence;

__declspec(noinline) static void fault_in_leaf(void)
{
    *(volatile int *)0 = 0x534548;
}

__declspec(noinline) static void fault_through_frame(void)
{
    volatile unsigned long long stack_values[8];
    stack_values[0] = 0x554e57494e44ULL;
    fault_in_leaf();
    if (stack_values[0] == 0)
        g_caught = 0;
}

static int exception_filter(void)
{
    g_filter_called = 1;
    return 1;
}

__declspec(noinline) static void fault_with_finally(void)
{
    __try {
        fault_through_frame();
    } __finally {
        g_unwind_sequence = g_unwind_sequence == 0 ? 1 : -1;
    }
}

static void say(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) ++length;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, 0);
}

int main(void)
{
    __try {
        fault_through_frame();
    } __except(1) {
        g_caught = 1;
    }
    __try {
        fault_through_frame();
    } __except(exception_filter()) {
        g_filtered = 1;
    }
    __try {
        fault_with_finally();
    } __except(1) {
        NtDisplayString("[sehframe] outer handler\n");
        g_unwind_sequence = g_unwind_sequence == 1 ? 2 : -2;
    }
    NtDisplayString("[sehframe] after handler\n");
    if (!g_caught || !g_filtered || !g_filter_called ||
        g_unwind_sequence != 2) {
        say("SEH_FRAME_TEST: FAIL\r\n");
        return 1;
    }
    say("SEH_FRAME_TEST: PASS\r\n");
    say("SEH_FINALLY_TEST: PASS\r\n");
    return 0;
}
