/* A query handle is a real process-owned object so CloseHandle, process
 * teardown and foreign-handle validation follow the existing native rules. */
static POBJECT_TYPE g_processor_counter_query_type;
typedef struct {
    BOOLEAN ProcessorInformation;
} KI_PROCESSOR_COUNTER_QUERY;

static UINT64 SvNtProcessorCounterQuery(UINT64 *args)
{
    ULONG operation = (ULONG)args[0];
    HANDLE handle = (HANDLE)args[1];
    void *out = (void *)args[2];
    if (operation == 0) {
        if (!MmProbeForWrite((UINT64)out, sizeof(HANDLE)))
            return (UINT64)STATUS_ACCESS_VIOLATION;
        if (!g_processor_counter_query_type)
            g_processor_counter_query_type = ObCreateObjectType("ProcessorCounterQuery", NULL);
        if (!g_processor_counter_query_type) return (UINT64)STATUS_NO_MEMORY;
        POBJECT object;
        NTSTATUS status = ObCreateObject(g_processor_counter_query_type,
                                         sizeof(KI_PROCESSOR_COUNTER_QUERY), &object);
        if (!NT_SUCCESS(status)) return (UINT64)status;
        ((KI_PROCESSOR_COUNTER_QUERY *)object)->ProcessorInformation = FALSE;
        HANDLE created = NULL;
        status = ObCreateHandle(object, GENERIC_ALL, &created);
        ObDereferenceObject(object);
        if (NT_SUCCESS(status)) *(HANDLE *)out = created;
        return (UINT64)status;
    }
    if (operation != 1 && operation != 2) return (UINT64)STATUS_INVALID_PARAMETER;
    if (!g_processor_counter_query_type) return (UINT64)STATUS_INVALID_HANDLE;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_processor_counter_query_type, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    KI_PROCESSOR_COUNTER_QUERY *query = object;
    if (operation == 1) {
        if (query->ProcessorInformation) status = STATUS_NOT_SUPPORTED;
        else if (!MmProbeForWrite((UINT64)out, sizeof(ULONG))) status = STATUS_ACCESS_VIOLATION;
        else {
            query->ProcessorInformation = TRUE;
            *(ULONG *)out = 0; /* first query item index */
        }
    } else {
        status = query->ProcessorInformation
            ? KiQueryProcessorPerformance(out, (ULONG)args[3], NULL)
            : STATUS_INVALID_PARAMETER;
    }
    ObDereferenceObject(object);
    return (UINT64)status;
}
