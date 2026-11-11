/* The Windows 7 processor counter query used by the performance UI.
 * Query handles and samples are owned by the native object manager and scheduler. */
typedef unsigned int ULONG;
typedef int NTSTATUS;
typedef unsigned short WCHAR;
typedef unsigned long long U64;

typedef struct {
    unsigned short Length, MaximumLength;
    const WCHAR *Buffer;
} PCW_STRING;

typedef struct {
    U64 IdleTime, KernelTime, UserTime, DpcTime, InterruptTime;
    ULONG InterruptCount, Reserved;
} PCW_PROCESSOR_SAMPLE;

__declspec(dllimport) NTSTATUS SvNtProcessorCounterQuery(ULONG operation,
    void *query, void *out, ULONG out_size);

enum {
    PCW_CREATE_QUERY, PCW_ADD_PROCESSOR_INFORMATION, PCW_READ_PROCESSOR_SAMPLE
};
#define PCW_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define PCW_NOT_SUPPORTED ((NTSTATUS)0xc00000bb)
#define PCW_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)

static int pcw_string_equal(const PCW_STRING *s, const WCHAR *value, ULONG chars)
{
    if (!s || !s->Buffer || s->Length != chars * sizeof(WCHAR) ||
        s->MaximumLength < s->Length) return 0;
    for (ULONG i = 0; i < chars; ++i)
        if (s->Buffer[i] != value[i]) return 0;
    return 1;
}

__declspec(dllexport) NTSTATUS PcwCreateQuery(void **query, U64 flags)
{
    if (!query) return PCW_INVALID_PARAMETER;
    *query = 0;
    if (flags) return PCW_NOT_SUPPORTED;
    return SvNtProcessorCounterQuery(PCW_CREATE_QUERY, 0, query, sizeof(*query));
}

__declspec(dllexport) NTSTATUS PcwAddQueryItem(ULONG *item, void *query,
    unsigned char selector, const PCW_STRING *name, const PCW_STRING *instance,
    ULONG instance_id, U64 flags, U64 user_data)
{
    static const WCHAR processor[] = L"Processor Information";
    if (!item) return PCW_INVALID_PARAMETER;
    if (selector != 1 || instance_id != 0xffffffffUL || flags != 5 || user_data ||
        !pcw_string_equal(name, processor, sizeof(processor) / sizeof(WCHAR) - 1) ||
        !pcw_string_equal(instance, L"*", 1)) return PCW_NOT_SUPPORTED;
    return SvNtProcessorCounterQuery(PCW_ADD_PROCESSOR_INFORMATION, query, item, sizeof(*item));
}

typedef struct {
    ULONG Size, DataSize;
    U64 Value;
} PCW_COUNTER_DATA;

typedef struct {
    ULONG Size, Id, CounterOffset, Reserved;
    WCHAR Name[12];
    PCW_COUNTER_DATA Idle, Privileged;
} PCW_INSTANCE;

typedef struct {
    ULONG Size, ItemOffset;
    ULONG ItemSize, InstancesOffset, InstanceCount, Reserved;
    PCW_INSTANCE Instances[3];
} PCW_PROCESSOR_RESULT;

#define OFFSET(type, field) ((ULONG)__builtin_offsetof(type, field))
_Static_assert(sizeof(PCW_PROCESSOR_SAMPLE) == 48, "native processor sample ABI");
_Static_assert(sizeof(PCW_INSTANCE) == 72, "Win7 PCW instance stride");
_Static_assert(OFFSET(PCW_INSTANCE, Name) == 16, "Win7 PCW instance name offset");
_Static_assert(sizeof(PCW_PROCESSOR_RESULT) == 240, "Win7 processor query response");

__declspec(dllexport) NTSTATUS PcwCollectData(void *query, void *buffer, ULONG bytes, ULONG *used)
{
    if (!used) return PCW_INVALID_PARAMETER;
    *used = 0;
    PCW_PROCESSOR_SAMPLE cpu;
    NTSTATUS status = SvNtProcessorCounterQuery(PCW_READ_PROCESSOR_SAMPLE, query, &cpu, sizeof(cpu));
    if (status < 0) return status;
    *used = sizeof(PCW_PROCESSOR_RESULT);
    if (!buffer || bytes < *used) return PCW_BUFFER_OVERFLOW;

    PCW_PROCESSOR_RESULT result = {0};
    result.Size = sizeof(result);
    result.ItemOffset = OFFSET(PCW_PROCESSOR_RESULT, ItemSize);
    result.ItemSize = result.Size - result.ItemOffset;
    result.InstancesOffset = OFFSET(PCW_PROCESSOR_RESULT, Instances) - result.ItemOffset;
    result.InstanceCount = 3;
    /* The active BSP is the sole member of processor group 0. */
    static const WCHAR names[3][12] = {L"_Total", L"0,_Total", L"0,0"};
    for (ULONG i = 0; i < result.InstanceCount; ++i) {
        PCW_INSTANCE *instance = &result.Instances[i];
        instance->Size = sizeof(*instance);
        instance->Id = i;
        instance->CounterOffset = OFFSET(PCW_INSTANCE, Idle);
        for (ULONG c = 0; c < 12; ++c) instance->Name[c] = names[i][c];
        instance->Idle = (PCW_COUNTER_DATA){sizeof(PCW_COUNTER_DATA), sizeof(U64), cpu.IdleTime};
        U64 privileged = cpu.KernelTime > cpu.IdleTime ? cpu.KernelTime - cpu.IdleTime : 0;
        instance->Privileged = (PCW_COUNTER_DATA){sizeof(PCW_COUNTER_DATA), sizeof(U64), privileged};
    }
    unsigned char *out = buffer;
    const unsigned char *input = (const unsigned char *)&result;
    for (ULONG i = 0; i < sizeof(result); ++i) out[i] = input[i];
    return 0;
}
