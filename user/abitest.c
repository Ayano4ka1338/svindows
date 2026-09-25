/* Win64 syscall-boundary and context-switch ABI regression. */
typedef void *HANDLE;
typedef void *LPVOID;
typedef unsigned long DWORD;
typedef unsigned long long U64;
typedef int BOOL;

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define V(n) (0xA810000000000000ULL + (n))
#define EXPECT(n) (0xA8100000ULL + (n))

__declspec(dllimport) HANDLE GetStdHandle(DWORD);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) HANDLE CreateMutexW(LPVOID, BOOL, const unsigned short *);
__declspec(dllimport) BOOL ReleaseMutex(HANDLE);
__declspec(dllimport) DWORD WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) HANDLE CreateThread(LPVOID, U64, DWORD (*)(LPVOID),
                                          LPVOID, DWORD, DWORD *);
__declspec(dllimport) BOOL CloseHandle(HANDLE);
__declspec(dllimport) LPVOID VirtualAlloc(LPVOID, U64, DWORD, DWORD);
__declspec(dllimport) BOOL VirtualFree(LPVOID, U64, DWORD);
__declspec(dllimport) U64 VirtualQuery(const void *, void *, U64);

__declspec(dllimport) U64 NtAbiProbe0(void);
__declspec(dllimport) U64 NtAbiProbe1(U64);
__declspec(dllimport) U64 NtAbiProbe2(U64, U64);
__declspec(dllimport) U64 NtAbiProbe3(U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe4(U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe5(U64, U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe6(U64, U64, U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe7(U64, U64, U64, U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe8(U64, U64, U64, U64, U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe9(U64, U64, U64, U64, U64, U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe10(U64, U64, U64, U64, U64, U64, U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe11(U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe12(U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe13(U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe14(U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe15(U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiProbe16(U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64);
__declspec(dllimport) U64 NtAbiShadowProbe(void);

int abi_check_registers(int blocking);

static void say(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, 0);
}

#define CHECK(call, n) do { if ((call) != EXPECT(n)) return (n) + 1; } while (0)

static int check_syscall_arities(void)
{
    CHECK(NtAbiProbe0(), 0);
    CHECK(NtAbiProbe1(V(0)), 1);
    CHECK(NtAbiProbe2(V(0), V(1)), 2);
    CHECK(NtAbiProbe3(V(0), V(1), V(2)), 3);
    CHECK(NtAbiProbe4(V(0), V(1), V(2), V(3)), 4);
    CHECK(NtAbiProbe5(V(0), V(1), V(2), V(3), V(4)), 5);
    CHECK(NtAbiProbe6(V(0), V(1), V(2), V(3), V(4), V(5)), 6);
    CHECK(NtAbiProbe7(V(0), V(1), V(2), V(3), V(4), V(5), V(6)), 7);
    CHECK(NtAbiProbe8(V(0), V(1), V(2), V(3), V(4), V(5), V(6), V(7)), 8);
    CHECK(NtAbiProbe9(V(0), V(1), V(2), V(3), V(4), V(5), V(6), V(7), V(8)), 9);
    CHECK(NtAbiProbe10(V(0), V(1), V(2), V(3), V(4), V(5), V(6), V(7), V(8), V(9)), 10);
    CHECK(NtAbiProbe11(V(0), V(1), V(2), V(3), V(4), V(5), V(6), V(7), V(8), V(9), V(10)), 11);
    CHECK(NtAbiProbe12(V(0), V(1), V(2), V(3), V(4), V(5), V(6), V(7), V(8), V(9), V(10), V(11)), 12);
    CHECK(NtAbiProbe13(V(0), V(1), V(2), V(3), V(4), V(5), V(6), V(7), V(8), V(9), V(10), V(11), V(12)), 13);
    CHECK(NtAbiProbe14(V(0), V(1), V(2), V(3), V(4), V(5), V(6), V(7), V(8), V(9), V(10), V(11), V(12), V(13)), 14);
    CHECK(NtAbiProbe15(V(0), V(1), V(2), V(3), V(4), V(5), V(6), V(7), V(8), V(9), V(10), V(11), V(12), V(13), V(14)), 15);
    CHECK(NtAbiProbe16(V(0), V(1), V(2), V(3), V(4), V(5), V(6), V(7), V(8), V(9), V(10), V(11), V(12), V(13), V(14), V(15)), 16);
    return 0;
}

static DWORD abandon_mutex(LPVOID parameter)
{
    return WaitForSingleObject((HANDLE)parameter, (DWORD)-1) == 0 ? 0 : 1;
}

static int check_mutant(void)
{
    HANDLE mutex = CreateMutexW(0, 1, 0);
    if (!mutex || WaitForSingleObject(mutex, 0) != 0)
        return 0;
    if (!ReleaseMutex(mutex) || !ReleaseMutex(mutex) || ReleaseMutex(mutex))
        return 0;

    HANDLE thread = CreateThread(0, 0, abandon_mutex, mutex, 0, 0);
    if (!thread || WaitForSingleObject(thread, (DWORD)-1) != 0)
        return 0;
    CloseHandle(thread);
    if (WaitForSingleObject(mutex, (DWORD)-1) != 0x80)
        return 0;
    if (!ReleaseMutex(mutex))
        return 0;
    CloseHandle(mutex);
    return 1;
}

typedef struct _VM_INFO {
    void *BaseAddress;
    void *AllocationBase;
    DWORD AllocationProtect;
    DWORD PartitionId;
    U64 RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
    DWORD Reserved;
} VM_INFO;

static int check_virtual_memory(void)
{
    VM_INFO info;
    char *base = (char *)VirtualAlloc(0, 8192, 0x2000, 0x04);
    if (!base || VirtualQuery(base, &info, sizeof(info)) != sizeof(info) ||
        info.State != 0x2000 || info.RegionSize < 8192)
        return 0;
    if (VirtualAlloc(base, 4096, 0x1000, 0x04) != base ||
        VirtualQuery(base, &info, sizeof(info)) != sizeof(info) ||
        info.State != 0x1000)
        return 0;
    base[0] = 0x5a;
    if (!VirtualFree(base, 4096, 0x4000) ||
        VirtualQuery(base, &info, sizeof(info)) != sizeof(info) ||
        info.State != 0x2000)
        return 0;
    if (!VirtualFree(base, 0, 0x8000) ||
        VirtualQuery(base, &info, sizeof(info)) != sizeof(info) ||
        info.State != 0x10000)
        return 0;
    return 1;
}

int main(void)
{
    int arity = check_syscall_arities();
    if (arity || NtAbiShadowProbe() != 0xA8105A5AULL ||
        !abi_check_registers(0) || !abi_check_registers(1)) {
        say("ABI_TEST: FAIL\r\n");
        return arity ? arity : 100;
    }
    if (!check_mutant()) {
        say("MUTANT_TEST: FAIL\r\n");
        return 101;
    }
    say("MUTANT_TEST: PASS\r\n");
    if (!check_virtual_memory()) {
        say("VM_TEST: FAIL\r\n");
        return 102;
    }
    say("VM_TEST: PASS\r\n");
    say("ABI_TEST: PASS\r\n");
    return 0;
}
