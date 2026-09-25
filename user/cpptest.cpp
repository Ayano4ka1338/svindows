/*
 * user/cpptest.cpp - C++ exception regression.
 *
 * Exercises the MSVC x64 C++ personality (__CxxFrameHandler3) and
 * _CxxThrowException over the existing .pdata unwinder: a typed catch, a
 * catch-all, a base-class catch through the thrown object's CatchableType
 * array, and local destructor ordering (a try-block destructor must run
 * before its own catch body).
 */

extern "C" void NtDisplayString(const char *text);
extern "C" void NtDisplayNumber(unsigned long long value);

struct MyError {
    int code;
};

struct Base {
    int base_value;
};

struct Derived : Base {
    int derived_value;
};

static int g_caught;
static int g_catch_all;
static int g_base_caught;
static int g_destructed;

static int g_same_frame_order;
static int g_same_frame_catch_order = -1;

struct Guard {
    ~Guard() { ++g_destructed; }
};

struct FrameGuard {
    ~FrameGuard() { ++g_same_frame_order; }
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

static void throw_derived(void)
{
    Derived value;
    value.base_value = 11;
    value.derived_value = 22;
    throw value;
}

static void catch_in_same_frame(void)
{
    try {
        FrameGuard guard;
        (void)guard;
        throw 1;
    } catch (...) {
        /* The FrameGuard destructor must already have run. */
        g_same_frame_catch_order = g_same_frame_order;
    }
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

    try {
        throw_derived();
    } catch (Base &base) {
        g_base_caught = base.base_value;
    }

    catch_in_same_frame();

    if (g_caught != 42 || g_destructed != 1 || !g_catch_all ||
        g_base_caught != 11 || g_same_frame_catch_order != 1) {
        NtDisplayString("[cpp] caught="); NtDisplayNumber(g_caught);
        NtDisplayString(" destructed="); NtDisplayNumber(g_destructed);
        NtDisplayString(" catch_all="); NtDisplayNumber(g_catch_all);
        NtDisplayString(" base="); NtDisplayNumber(g_base_caught);
        NtDisplayString(" same_order="); NtDisplayNumber(g_same_frame_catch_order);
        NtDisplayString("\n");
        NtDisplayString("CPP_EH_TEST: FAIL\n");
        return 1;
    }
    NtDisplayString("CPP_EH_TEST: PASS\n");
    return 0;
}
