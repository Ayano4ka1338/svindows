#ifndef _NT_EXCEPTION_H_
#define _NT_EXCEPTION_H_

#include <nt/Ntdef.h>

#define EXCEPTION_MAXIMUM_PARAMETERS 15
#define CONTEXT_AMD64_FLAG           0x00100000u
#define CONTEXT_CONTROL              (CONTEXT_AMD64_FLAG | 0x00000001u)
#define CONTEXT_INTEGER              (CONTEXT_AMD64_FLAG | 0x00000002u)

/* The integer/control prefix of the Windows AMD64 CONTEXT structure.  The
 * offsets through Rip are ABI-defined and are also consumed by Ntdll.asm. */
typedef struct _CONTEXT_AMD64 {
    UINT64 P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    UINT32 ContextFlags;
    UINT32 MxCsr;
    UINT16 SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    UINT32 EFlags;
    UINT64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    UINT64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    UINT64 R8, R9, R10, R11, R12, R13, R14, R15;
    UINT64 Rip;
} CONTEXT_AMD64, *PCONTEXT_AMD64;

typedef struct _EXCEPTION_RECORD64 {
    NTSTATUS ExceptionCode;
    UINT32 ExceptionFlags;
    UINT64 ExceptionRecord;
    UINT64 ExceptionAddress;
    UINT32 NumberParameters;
    UINT32 Padding;
    UINT64 ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD64, *PEXCEPTION_RECORD64;

#endif
