#ifndef _USER_NTSEH_H_
#define _USER_NTSEH_H_

typedef unsigned long DWORD;
typedef unsigned long long U64;
typedef long NTSTATUS;
typedef long LONG;
typedef unsigned long ULONG;

#define EXCEPTION_CONTINUE_EXECUTION ((LONG)-1)
#define EXCEPTION_CONTINUE_SEARCH    ((LONG)0)
#define STATUS_ACCESS_VIOLATION       ((NTSTATUS)0xC0000005L)
#define STATUS_INTEGER_DIVIDE_BY_ZERO ((NTSTATUS)0xC0000094L)

typedef struct _NT_CONTEXT64 {
    U64 P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    DWORD ContextFlags, MxCsr;
    unsigned short SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    DWORD EFlags;
    U64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    U64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    U64 R8, R9, R10, R11, R12, R13, R14, R15;
    U64 Rip;
} NT_CONTEXT64;

typedef struct _NT_EXCEPTION_RECORD64 {
    NTSTATUS ExceptionCode;
    DWORD ExceptionFlags;
    U64 ExceptionRecord;
    U64 ExceptionAddress;
    DWORD NumberParameters;
    DWORD Padding;
    U64 ExceptionInformation[15];
} NT_EXCEPTION_RECORD64;

typedef struct _NT_EXCEPTION_POINTERS64 {
    NT_EXCEPTION_RECORD64 *ExceptionRecord;
    NT_CONTEXT64 *ContextRecord;
} NT_EXCEPTION_POINTERS64;

typedef LONG (*NT_VECTORED_EXCEPTION_HANDLER)(NT_EXCEPTION_POINTERS64 *);

#endif
