/*
 * user/cpptest.cpp - C++ exception regression.
 *
 * Exercises the MSVC x64 C++ personality (__CxxFrameHandler3) and
 * _CxxThrowException over the existing .pdata unwinder: a typed catch, a
 * catch-all, and a local destructor that must run while the throw unwinds
 * through its frame.
 */

extern "C" void NtDisplayString(const char *text);

struct MyError {
    int code;
};

static int g_caught;
static int g_destructed;
static int g_catch_all;

struct Guard {
    ~Guard() { ++g_destructed; }
};

static void throw_typed(void)
{
    Guard guard;
    (void)guard;
    throw MyError{42};
}

static void throw_other(void)
{
    throw 7;
}

int main(void)
{
    try {
        throw_typed();
    } catch (MyError &error) {
        g_caught = error.code;
    }

    try {
        throw_other();
    } catch (...) {
        g_catch_all = 1;
    }

    if (g_caught != 42 || g_destructed != 1 || !g_catch_all) {
        NtDisplayString("CPP_EH_TEST: FAIL\n");
        return 1;
    }
    NtDisplayString("CPP_EH_TEST: PASS\n");
    return 0;
}
