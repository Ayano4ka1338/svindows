/* Win32 job API backed by process-manager job objects and native handles. */
__declspec(dllexport) HANDLE CreateJobObjectW(void *security_attributes,
                                              const WCHAR *name)
{
    (void)security_attributes;
    WCHAR path[260];
    UNICODE_STRING_K unicode;
    OBJECT_ATTRIBUTES attributes;
    if (name && !event_name_attributes(name, path, &unicode, &attributes))
        return 0;
    HANDLE job = 0;
    NTSTATUS status = NtCreateJobObject(&job, 0x001F003Fu,
                                        name ? &attributes : 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status)); return 0;
    }
    SetLastError(status == 0x40000000u ? 183 : 0);
    return job;
}

__declspec(dllexport) BOOL AssignProcessToJobObject(HANDLE job,
                                                     HANDLE process)
{
    NTSTATUS status = NtAssignProcessToJobObject(job, process);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status)); return 0;
    }
    return 1;
}

__declspec(dllexport) BOOL SetInformationJobObject(HANDLE job,
                                                   DWORD info_class,
                                                   const void *buffer,
                                                   DWORD length)
{
    NTSTATUS status = NtSetInformationJobObject(job, info_class,
                                                buffer, length);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status)); return 0;
    }
    return 1;
}

__declspec(dllexport) BOOL QueryInformationJobObject(HANDLE job,
                                                     DWORD info_class,
                                                     void *buffer,
                                                     DWORD length,
                                                     DWORD *return_length)
{
    NTSTATUS status = NtQueryInformationJobObject(job, info_class, buffer,
                                                  length, return_length);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status)); return 0;
    }
    return 1;
}
