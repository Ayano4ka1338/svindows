/*
 * svindows/Trace.h - user-mode boundary tracing for genuine-DLL bring-up.
 *
 * The loader wraps selected USER32 imports in small trampolines that notify
 * the kernel (NtTraceCall) and tail-jump to the real function. The kernel
 * logs the call site and the first four arguments, which is the visibility we
 * need when a real Microsoft DLL silently changes behavior client-side.
 */
#ifndef SVINDOWS_KERNEL_INCLUDE_SVINDOWS_TRACE_H
#define SVINDOWS_KERNEL_INCLUDE_SVINDOWS_TRACE_H

/* Private syscall number used only by the loader-generated trampolines. */
#define SVINDOWS_TRACE_SYSCALL 0xFB

/* Fixed user-mode address where a trampoline stores its tag before the
 * syscall; the kernel reads it from there. */
#define SVINDOWS_TRACE_TAG_VA  0x000000000006FF80ULL

enum {
    SVINDOWS_TRACE_RegisterClassW = 1,
    SVINDOWS_TRACE_RegisterClassExW,
    SVINDOWS_TRACE_CreateWindowExW,
    SVINDOWS_TRACE_DestroyWindow,
    SVINDOWS_TRACE_PostThreadMessageW,
    SVINDOWS_TRACE_PostMessageW,
    SVINDOWS_TRACE_DefWindowProcW,
    SVINDOWS_TRACE_DispatchMessageW,
    SVINDOWS_TRACE_RegisterClassWEntry,
    SVINDOWS_TRACE_COUNT
};

#endif /* SVINDOWS_KERNEL_INCLUDE_SVINDOWS_TRACE_H */
