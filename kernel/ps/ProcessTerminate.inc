UINT64 NtTerminateProcess(UINT64 *arguments)
{
    PKTHREAD caller = KeGetCurrentThread();
    if (!caller || !caller->Process) return STATUS_INVALID_HANDLE;
    HANDLE handle = (HANDLE)(ULONG_PTR)arguments[0];
    NTSTATUS exit_status = (NTSTATUS)arguments[1];
    POBJECT referenced = NULL;
    PKPROCESS process;
    if ((ULONG_PTR)handle == ~(ULONG_PTR)0) {
        process = caller->Process;
    } else {
        NTSTATUS status = ObReferenceObjectByHandle(handle, 1, NULL, &referenced);
        if (!NT_SUCCESS(status)) return (UINT64)status;
        process = PsGetProcessFromObject(referenced);
        if (!process) {
            ObDereferenceObject(referenced);
            return STATUS_OBJECT_TYPE_MISMATCH;
        }
    }
    if (process == caller->Process) {
        /* Self-termination from a worker must also end its sibling threads. */
        if (referenced) ObDereferenceObject(referenced);
        caller->ExitStatus = exit_status;
        caller->ProcessMainThread = TRUE;
        KeTerminateThread();
    }
    NTSTATUS status = KeTerminateProcess(process, exit_status);
    ObDereferenceObject(referenced);
    return (UINT64)status;
}
