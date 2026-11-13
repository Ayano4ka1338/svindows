/* Native file deletion uses the same share/delete-pending rules as opened
 * files. Closing the temporary DELETE handle completes the operation. */
typedef struct { void *Status; ULONGLONG Information; } FILE_COMPAT_STATUS;
extern NTSTATUS NtOpenFile(HANDLE *, ULONG, const void *, void *, ULONG, ULONG);
extern NTSTATUS NtSetInformationFile(HANDLE, void *, const void *, ULONG, ULONG);
extern NTSTATUS NtClose(HANDLE);

NTSTATUS NtDeleteFile(const void *attributes)
{
    HANDLE file = 0;
    FILE_COMPAT_STATUS io = {0};
    NTSTATUS status = NtOpenFile(&file, 0x110000, attributes, &io, 7, 0x20);
    if (!NT_SUCCESS(status)) return status;
    BYTE delete_file = 1;
    status = NtSetInformationFile(file, &io, &delete_file, sizeof(delete_file), 13);
    NtClose(file);
    return status;
}

NTSTATUS RtlAdjustPrivilege(ULONG privilege, BYTE enable, BYTE thread, BYTE *previous)
{
    (void)privilege; (void)enable; (void)thread; (void)previous;
    return STATUS_NOT_IMPLEMENTED;
}
NTSTATUS RtlCreateSystemVolumeInformationFolder(const UNICODE_STRING_NT *root)
{
    (void)root;
    return STATUS_NOT_IMPLEMENTED;
}
