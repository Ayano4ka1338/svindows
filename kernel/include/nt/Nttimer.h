#ifndef _NT_NTTIMER_H_
#define _NT_NTTIMER_H_
#include <nt/Ntdef.h>
#define TIMER_QUERY_STATE 0x0001u
#define TIMER_MODIFY_STATE 0x0002u
#define TIMER_ALL_ACCESS (0x000F0000u | SYNCHRONIZE | TIMER_QUERY_STATE | TIMER_MODIFY_STATE)
typedef struct _TIMER_BASIC_INFORMATION {
    INT64 RemainingTime;
    BOOLEAN TimerState;
    UINT8 Padding[7];
} TIMER_BASIC_INFORMATION;
_Static_assert(sizeof(TIMER_BASIC_INFORMATION) == 16, "x64 timer information ABI");
#endif
