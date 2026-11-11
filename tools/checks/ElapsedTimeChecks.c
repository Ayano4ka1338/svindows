#include <assert.h>
#include <stdio.h>
typedef long long LONGLONG;
#include "../../user/w/ntdll/Rtl/ElapsedTime.inc"
int main(void)
{
    LONGLONG time = ((1ll * 86400 + 2 * 3600 + 3 * 60 + 4) * 1000 + 5) * 10000;
    RTL_ELAPSED_FIELDS fields;
    RtlTimeToElapsedTimeFields(&time, &fields);
    assert(!fields.Year && !fields.Month && !fields.Weekday);
    assert(fields.Day == 1 && fields.Hour == 2 && fields.Minute == 3);
    assert(fields.Second == 4 && fields.Milliseconds == 5);
    time = 9999;
    RtlTimeToElapsedTimeFields(&time, &fields);
    assert(!fields.Day && !fields.Second && !fields.Milliseconds);
    puts("PASS elapsed durations retain total days and subsecond precision");
}
