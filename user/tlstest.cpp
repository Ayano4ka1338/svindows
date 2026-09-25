/*
 * user/tlstest.cpp - static PE thread-local storage regression.
 *
 * Exercises the loader's TLS path: the image carries an IMAGE_TLS_DIRECTORY64
 * (via `_tls_used`) naming its .tls template and the `_tls_index` the loader
 * must fill, and `thread_local` variables are reached through
 * `gs:[0x58]` (TEB.ThreadLocalStoragePointer). The test checks that the main
 * thread sees the template values, that writes stick, and that a second thread
 * gets its own independent copy.
 */

extern "C" void NtDisplayString(const char *text);

/* Filled by the loader with this module's TLS index. */
extern "C" unsigned long _tls_index = 0;

/* Bracket the module's .tls template. The compiler emits SECREL offsets
 * relative to the start of the .tls section, so `_tls_start` must sit in the
 * bare `.tls` section (which lld orders before the compiler's `.tls$...`);
 * `_tls_end` closes the template in `.tls$ZZZ`. */
extern "C" char _tls_start[], _tls_end[];
__asm__(
".section .tls,\"dw\"\n"
".globl _tls_start\n"
"_tls_start:\n"
".byte 0\n"
".section .tls$ZZZ,\"dw\"\n"
".globl _tls_end\n"
"_tls_end:\n"
".byte 0\n"
".text\n"
);

typedef struct _IMAGE_TLS_DIRECTORY64 {
    unsigned long long StartAddressOfRawData;
    unsigned long long EndAddressOfRawData;
    unsigned long long AddressOfIndex;
    unsigned long long AddressOfCallBacks;
    unsigned int       SizeOfZeroFill;
    unsigned int       Characteristics;
} IMAGE_TLS_DIRECTORY64;

/* A TLS callback runs before any DllMain (and before main) with
 * DLL_PROCESS_ATTACH, receiving the module base as its first argument. */
static int g_callback_count;
static int g_callback_reason;
static void *g_callback_module;

extern "C" void __stdcall tls_callback(void *module, unsigned long reason,
                                       void *reserved)
{
    (void)reserved;
    g_callback_count++;
    g_callback_reason = (int)reason;
    g_callback_module = module;
}

extern "C" void *tls_callbacks[] = { (void *)tls_callback, 0 };

/* The PE TLS directory; lld-link places it in DataDirectory[9]. */
extern "C" IMAGE_TLS_DIRECTORY64 _tls_used = {
    (unsigned long long)_tls_start,
    (unsigned long long)_tls_end,
    (unsigned long long)&_tls_index,
    (unsigned long long)&tls_callbacks,
    0, 0
};

thread_local int g_value = 7;
thread_local int g_zero;

typedef void      *HANDLE;
typedef void      *LPVOID;
typedef unsigned long DWORD;
typedef DWORD (__stdcall *LPTHREAD_START_ROUTINE)(LPVOID);

extern "C" HANDLE CreateThread(LPVOID, unsigned long long,
                               LPTHREAD_START_ROUTINE, LPVOID, DWORD, DWORD *);
extern "C" DWORD  WaitForSingleObject(HANDLE, DWORD);
extern "C" int    CloseHandle(HANDLE);

static int g_worker_saw_template;
static int g_worker_saw_independent;

static DWORD __stdcall worker(LPVOID)
{
    g_worker_saw_template = (g_value == 7 && g_zero == 0);
    g_value = 99;
    g_zero = 1;
    g_worker_saw_independent = (g_value == 99 && g_zero == 1);
    return 0;
}

int main(void)
{
    int template_ok = (g_value == 7 && g_zero == 0);

    g_value = 42;
    g_zero = 5;
    int write_ok = (g_value == 42 && g_zero == 5);

    HANDLE thread = CreateThread(0, 0, worker, 0, 0, 0);
    if (thread) {
        WaitForSingleObject(thread, (DWORD)-1);
        CloseHandle(thread);
    }

    /* The worker's writes must not touch the main thread's copy. */
    int isolation_ok = (g_value == 42 && g_zero == 5);

    int callback_ok = (g_callback_count == 1 && g_callback_reason == 1 &&
                       g_callback_module != 0);

    if (!template_ok || !write_ok || !g_worker_saw_template ||
        !g_worker_saw_independent || !isolation_ok || !callback_ok) {
        NtDisplayString("TLS_TEST: FAIL\n");
        return 1;
    }
    NtDisplayString("TLS_TEST: PASS\n");
    return 0;
}
