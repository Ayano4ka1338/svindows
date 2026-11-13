/* Captured process creation transport shared by kernel32 and Ps. All pointers
 * refer to UTF-16 strings (environment is a double-NUL block). */
#ifndef SV_PROCESS_CREATE_ABI_H
#define SV_PROCESS_CREATE_ABI_H
typedef struct _SV_PROCESS_CREATE_REQUEST {
    unsigned int Size, Flags, InheritHandles, Reserved;
    unsigned long long Image, CommandLine, CurrentDirectory, Desktop, Environment;
    unsigned long long StandardInput, StandardOutput, StandardError;
    unsigned long long ProcessAttributes, ThreadAttributes;
    unsigned int StartupFlags, ShowWindow;
    unsigned int X, Y, XSize, YSize, XChars, YChars, FillAttribute, Padding;
} SV_PROCESS_CREATE_REQUEST;
typedef struct _SV_PROCESS_CREATE_RESULT {
    unsigned long long Process, Thread;
    unsigned int ProcessId, ThreadId;
} SV_PROCESS_CREATE_RESULT;
_Static_assert(sizeof(SV_PROCESS_CREATE_REQUEST) == 136, "process request ABI");
_Static_assert(sizeof(SV_PROCESS_CREATE_RESULT) == 24, "process result ABI");
#endif
