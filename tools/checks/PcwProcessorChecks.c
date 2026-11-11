#include <assert.h>
#include <stdio.h>
#include <string.h>
#define __declspec(x)
#include "../../user/w/taskservices/pcwum.c"

static PCW_PROCESSOR_SAMPLE sample = {100000, 240000, 160000, 0, 0, 0, 0};
static int token, configured;
NTSTATUS SvNtProcessorCounterQuery(ULONG operation, void *query, void *out, ULONG size)
{
    if (operation == PCW_CREATE_QUERY) {
        assert(size == sizeof(void *));
        *(void **)out = &token;
        return 0;
    }
    if (query != &token) return (NTSTATUS)0xc0000008;
    if (operation == PCW_ADD_PROCESSOR_INFORMATION) {
        assert(size == sizeof(ULONG));
        configured = 1;
        *(ULONG *)out = 0;
        return 0;
    }
    assert(operation == PCW_READ_PROCESSOR_SAMPLE && size == sizeof(sample));
    if (!configured) return PCW_INVALID_PARAMETER;
    memcpy(out, &sample, size);
    return 0;
}

int main(void)
{
    void *query;
    ULONG used;
    assert(!PcwCreateQuery(&query, 0));
    assert(PcwCollectData(query, NULL, 0, &used) == PCW_INVALID_PARAMETER && used == 0);
    static const WCHAR name[] = L"Processor Information";
    PCW_STRING provider = {sizeof(name)-2, sizeof(name), name};
    PCW_STRING wildcard = {2, 4, L"*"};
    ULONG item[] = {123, 456};
    assert(PcwAddQueryItem(item, query, 0, &provider, &wildcard, ~0u, 5, 0) == PCW_NOT_SUPPORTED);
    assert(!PcwAddQueryItem(item, query, 1, &provider, &wildcard, ~0u, 5, 0));
    assert(item[0] == 0 && item[1] == 456);
    PCW_PROCESSOR_RESULT result;
    memset(&result, 0xaa, sizeof(result));
    assert(PcwCollectData(query, &result, sizeof(result)-1, &used) == PCW_BUFFER_OVERFLOW);
    assert(used == sizeof(result) && result.Size == 0xaaaaaaaau);
    assert(!PcwCollectData(query, &result, sizeof(result), &used));
    assert(result.ItemOffset == 8 && result.InstancesOffset == 16 && result.InstanceCount == 3);
    assert(result.Instances[0].Name[0] == '_' && result.Instances[1].Name[2] == '_' &&
           result.Instances[2].Name[2] == '0');
    for (unsigned i = 0; i < 3; ++i) {
        const PCW_INSTANCE *p = &result.Instances[i];
        assert(p->Id == i && p->Size == 72 && p->CounterOffset == 40);
        assert(p->Idle.Value == 100000 && p->Privileged.Value == 140000);
    }
    sample.IdleTime += 100000;
    sample.KernelTime += 150000;
    assert(!PcwCollectData(query, &result, sizeof(result), &used));
    assert(result.Instances[0].Idle.Value == 200000 && result.Instances[0].Privileged.Value == 190000);
    puts("PASS PCW instance layout, buffer negotiation, item width and changing native samples");
}
