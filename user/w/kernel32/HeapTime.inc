#include "HeapApi.inc"

/* ------------------------------------------------------------------ */
/* Time: KUSER_SHARED_DATA (the read-only page at 0x7FFE0000)          */
/* ------------------------------------------------------------------ */

#define KUSD ((volatile unsigned char *)(ULONGLONG)0x7FFE0000ULL)

/* Read a KSYSTEM_TIME (Low, High1, High2) lock-free from the shared page. */
static ULONGLONG read_ksystem_time(unsigned off)
{
    for (;;) {
        int high1 = *(volatile int *)(KUSD + off + 0x4);
        unsigned low = *(volatile unsigned *)(KUSD + off + 0x0);
        int high2 = *(volatile int *)(KUSD + off + 0x8);
        if (high1 == high2)
            return ((ULONGLONG)(unsigned)high1 << 32) | low;
    }
}

__declspec(dllexport) ULONGLONG GetTickCount64(void)
{
    ULONGLONG ticks = read_ksystem_time(0x320);          /* TickCount     */
    unsigned mult = *(volatile unsigned *)(KUSD + 0x004); /* Multiplier    */
    return (ticks * mult) >> 24;                          /* -> milliseconds */
}

__declspec(dllexport) DWORD GetTickCount(void)
{
    return (DWORD)GetTickCount64();
}

__declspec(dllexport) void GetSystemTimeAsFileTime(void *lpFileTime)
{
    ULONGLONG t = read_ksystem_time(0x014); /* SystemTime, 100 ns units */
    if (lpFileTime) {
        ((DWORD *)lpFileTime)[0] = (DWORD)t;
        ((DWORD *)lpFileTime)[1] = (DWORD)(t >> 32);
    }
}

__declspec(dllexport) BOOL FileTimeToSystemTime(const FILETIME *file_time,
                                                SYSTEMTIME *system_time);
__declspec(dllexport) BOOL FileTimeToLocalFileTime(const FILETIME *utc,
                                                    FILETIME *local);

__declspec(dllexport) void GetSystemTime(SYSTEMTIME *time)
{
    if (!time) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    FileTimeToSystemTime(&now, time);
}

__declspec(dllexport) void GetLocalTime(SYSTEMTIME *time)
{
    if (!time) return;
    FILETIME utc, local;
    GetSystemTimeAsFileTime(&utc);
    if (FileTimeToLocalFileTime(&utc, &local))
        FileTimeToSystemTime(&local, time);
}

__declspec(dllexport) BOOL FileTimeToSystemTime(const FILETIME *file_time,
                                                SYSTEMTIME *system_time)
{
    static const BYTE month_days[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!file_time || !system_time) {
        SetLastError(87);
        return 0;
    }
    ULONGLONG ticks = ((ULONGLONG)file_time->High << 32) | file_time->Low;
    ULONGLONG total_seconds = ticks / 10000000ULL;
    ULONGLONG days = total_seconds / 86400ULL;
    DWORD seconds = (DWORD)(total_seconds % 86400ULL);
    DWORD day_of_week = (DWORD)((days + 1) % 7); /* 1601-01-01 was Monday */

    DWORD year = 1601;
    while (year < 30828) {
        DWORD count = leap_year(year) ? 366 : 365;
        if (days < count) break;
        days -= count;
        year++;
    }
    if (year == 30828) {
        SetLastError(87);
        return 0;
    }
    DWORD month = 1;
    while (month <= 12) {
        DWORD count = month_days[month - 1];
        if (month == 2 && leap_year(year)) count++;
        if (days < count) break;
        days -= count;
        month++;
    }
    memset(system_time, 0, sizeof(*system_time));
    system_time->Year = (WORD)year;
    system_time->Month = (WORD)month;
    system_time->Day = (WORD)(days + 1);
    system_time->DayOfWeek = (WORD)day_of_week;
    system_time->Hour = (WORD)(seconds / 3600);
    system_time->Minute = (WORD)((seconds / 60) % 60);
    system_time->Second = (WORD)(seconds % 60);
    system_time->Milliseconds = (WORD)((ticks % 10000000ULL) / 10000ULL);
    return 1;
}

__declspec(dllexport) BOOL SystemTimeToFileTime(const SYSTEMTIME *system_time,
                                                FILETIME *file_time)
{
    static const BYTE month_days[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!system_time || !file_time || system_time->Year < 1601 ||
        system_time->Year > 30827 || system_time->Month < 1 ||
        system_time->Month > 12 || system_time->Hour > 23 ||
        system_time->Minute > 59 || system_time->Second > 59 ||
        system_time->Milliseconds > 999) {
        SetLastError(87);
        return 0;
    }
    DWORD month_length = month_days[system_time->Month - 1];
    if (system_time->Month == 2 && leap_year(system_time->Year))
        month_length++;
    if (system_time->Day < 1 || system_time->Day > month_length) {
        SetLastError(87);
        return 0;
    }
    ULONGLONG days = 0;
    for (DWORD year = 1601; year < system_time->Year; year++)
        days += leap_year(year) ? 366 : 365;
    for (DWORD month = 1; month < system_time->Month; month++)
        days += month_days[month - 1] +
                (month == 2 && leap_year(system_time->Year) ? 1 : 0);
    days += system_time->Day - 1;
    ULONGLONG ticks = (((days * 24 + system_time->Hour) * 60 +
                       system_time->Minute) * 60 +
                       system_time->Second) * 10000000ULL +
                       (ULONGLONG)system_time->Milliseconds * 10000;
    file_time->Low = (DWORD)ticks;
    file_time->High = (DWORD)(ticks >> 32);
    return 1;
}

__declspec(dllexport) long CompareFileTime(const FILETIME *left,
                                           const FILETIME *right)
{
    ULONGLONG a = ((ULONGLONG)left->High << 32) | left->Low;
    ULONGLONG b = ((ULONGLONG)right->High << 32) | right->Low;
    return a < b ? -1 : (a > b ? 1 : 0);
}

__declspec(dllexport) BOOL FileTimeToLocalFileTime(const FILETIME *utc,
                                                    FILETIME *local)
{
    if (!utc || !local) {
        SetLastError(87);
        return 0;
    }
    /* SVINDOWS has no configured time-zone database yet, so local time is UTC. */
    *local = *utc;
    return 1;
}

typedef struct _TIME_ZONE_INFORMATION_K {
    long Bias;
    WCHAR StandardName[32];
    SYSTEMTIME StandardDate;
    long StandardBias;
    WCHAR DaylightName[32];
    SYSTEMTIME DaylightDate;
    long DaylightBias;
} TIME_ZONE_INFORMATION_K;

typedef struct _DYNAMIC_TIME_ZONE_INFORMATION_K {
    TIME_ZONE_INFORMATION_K Base;
    WCHAR TimeZoneKeyName[128];
    BYTE DynamicDaylightTimeDisabled;
    BYTE Padding[3];
} DYNAMIC_TIME_ZONE_INFORMATION_K;

static void fill_utc_time_zone(TIME_ZONE_INFORMATION_K *zone)
{
    memset(zone, 0, sizeof(*zone));
    zone->StandardName[0] = 'U';
    zone->StandardName[1] = 'T';
    zone->StandardName[2] = 'C';
    zone->DaylightName[0] = 'U';
    zone->DaylightName[1] = 'T';
    zone->DaylightName[2] = 'C';
}

__declspec(dllexport) DWORD GetTimeZoneInformation(TIME_ZONE_INFORMATION_K *zone)
{
    if (!zone) { SetLastError(87); return 0xFFFFFFFFu; }
    fill_utc_time_zone(zone);
    return 0; /* TIME_ZONE_ID_UNKNOWN: UTC has no daylight transition. */
}

__declspec(dllexport) DWORD GetDynamicTimeZoneInformation(
    DYNAMIC_TIME_ZONE_INFORMATION_K *zone)
{
    if (!zone) { SetLastError(87); return 0xFFFFFFFFu; }
    memset(zone, 0, sizeof(*zone));
    fill_utc_time_zone(&zone->Base);
    zone->TimeZoneKeyName[0] = 'U';
    zone->TimeZoneKeyName[1] = 'T';
    zone->TimeZoneKeyName[2] = 'C';
    zone->DynamicDaylightTimeDisabled = 1;
    return 0;
}

static void put_dec_w(WCHAR *out, DWORD value, DWORD digits)
{
    while (digits) {
        out[--digits] = (WCHAR)('0' + value % 10);
        value /= 10;
    }
}

static int copy_formatted_time(const WCHAR *value, int length,
                               WCHAR *out, int capacity)
{
    int required = length + 1;
    if (!out || capacity == 0)
        return required;
    if (capacity < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    for (int i = 0; i <= length; i++) out[i] = value[i];
    return required;
}

__declspec(dllexport) int GetDateFormatW(DWORD locale, DWORD flags,
                                         const SYSTEMTIME *time,
                                         const WCHAR *format, WCHAR *out,
                                         int capacity)
{
    WCHAR value[11];
    (void)locale; (void)flags; (void)format;
    if (!time) {
        SetLastError(87);
        return 0;
    }
    put_dec_w(value, time->Year, 4);
    value[4] = '-';
    put_dec_w(value + 5, time->Month, 2);
    value[7] = '-';
    put_dec_w(value + 8, time->Day, 2);
    value[10] = 0;
    return copy_formatted_time(value, 10, out, capacity);
}

__declspec(dllexport) int GetTimeFormatW(DWORD locale, DWORD flags,
                                         const SYSTEMTIME *time,
                                         const WCHAR *format, WCHAR *out,
                                         int capacity)
{
    WCHAR value[9];
    (void)locale; (void)flags; (void)format;
    if (!time) {
        SetLastError(87);
        return 0;
    }
    put_dec_w(value, time->Hour, 2);
    value[2] = ':';
    put_dec_w(value + 3, time->Minute, 2);
    value[5] = ':';
    put_dec_w(value + 6, time->Second, 2);
    value[8] = 0;
    return copy_formatted_time(value, 8, out, capacity);
}

__declspec(dllexport) void Sleep(DWORD ms)
{
    /* Relative delay: negative 100 ns units. */
    long long interval = ms == (DWORD)-1 ?
                         (-0x7fffffffffffffffLL - 1) : -(long long)ms * 10000;
    NtDelayExecution(0, &interval);
}

__declspec(dllexport) DWORD SleepEx(DWORD ms, BOOL alertable)
{
    long long interval = ms == (DWORD)-1 ?
                         (-0x7fffffffffffffffLL - 1) : -(long long)ms * 10000;
    return NtDelayExecution(alertable, &interval) == 0xC0 ? 0xC0 : 0;
}
