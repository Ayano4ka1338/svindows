/* Win32 completion ports over native FIFO packet objects. */
__declspec(dllexport) HANDLE CreateIoCompletionPort(HANDLE file,
                                                     HANDLE existing_port,
                                                     ULONGLONG key,
                                                     DWORD concurrency)
{
    HANDLE port = existing_port;
    BOOL created = 0;
    if (!port) {
        NTSTATUS status = NtCreateIoCompletion(&port, 0, 0, concurrency);
        if (!NT_SUCCESS(status)) {
            SetLastError(ntstatus_to_win32(status));
            return 0;
        }
        created = 1;
    }
    if (file && file != NT_INVALID_HANDLE) {
        NTSTATUS status = NtAssociateIoCompletionFile(file, port, key);
        if (!NT_SUCCESS(status)) {
            if (created) NtClose(port);
            SetLastError(ntstatus_to_win32(status));
            return 0;
        }
    }
    return port;
}

__declspec(dllexport) BOOL PostQueuedCompletionStatus(HANDLE port,
                                                       DWORD bytes,
                                                       ULONGLONG key,
                                                       void *overlapped)
{
    NTSTATUS status = NtSetIoCompletion(port, (void *)key, overlapped,
                                        0, bytes);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    return 1;
}

__declspec(dllexport) BOOL GetQueuedCompletionStatus(HANDLE port,
                                                      DWORD *bytes,
                                                      ULONGLONG *key,
                                                      void **overlapped,
                                                      DWORD milliseconds)
{
    if (!bytes || !key || !overlapped) {
        SetLastError(87);
        return 0;
    }
    *overlapped = 0;
    long long relative = -(long long)milliseconds * 10000;
    const long long *timeout = milliseconds == (DWORD)-1 ? 0 : &relative;
    void *packet_key = 0, *packet_overlapped = 0;
    IO_STATUS_BLOCK result = {0};
    NTSTATUS status = NtRemoveIoCompletion(port, &packet_key,
                                            &packet_overlapped, &result,
                                            timeout);
    if (status == 0x102) {
        SetLastError(258); /* WAIT_TIMEOUT */
        return 0;
    }
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    *bytes = (DWORD)result.Information;
    *key = (ULONGLONG)packet_key;
    *overlapped = packet_overlapped;
    if (!NT_SUCCESS(result.Status)) {
        SetLastError(ntstatus_to_win32(result.Status));
        return 0;
    }
    return 1;
}
