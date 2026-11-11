typedef struct {
    short Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday;
} RTL_ELAPSED_FIELDS;

void RtlTimeToElapsedTimeFields(const LONGLONG *time, RTL_ELAPSED_FIELDS *fields)
{
    if (!time || !fields) return;
    LONGLONG milliseconds = *time / 10000;
    fields->Year = fields->Month = fields->Weekday = 0;
    fields->Milliseconds = (short)(milliseconds % 1000);
    LONGLONG seconds = milliseconds / 1000;
    fields->Second = (short)(seconds % 60);
    LONGLONG minutes = seconds / 60;
    fields->Minute = (short)(minutes % 60);
    LONGLONG hours = minutes / 60;
    fields->Hour = (short)(hours % 24);
    fields->Day = (short)(hours / 24);
}
