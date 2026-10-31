/* Registry enumeration and value deletion over native Cm services. */
extern NTSTATUS NtDeleteValueKey(HANDLE key, UNICODE_STRING *name);
extern NTSTATUS NtEnumerateKey(HANDLE key, DWORD index, DWORD cls,
                               void *buffer, DWORD size, DWORD *required);
extern NTSTATUS NtEnumerateValueKey(HANDLE key, DWORD index, DWORD cls,
                                    void *buffer, DWORD size, DWORD *required);
extern NTSTATUS NtQueryKey(HANDLE key, DWORD cls, void *buffer, DWORD size,
                           DWORD *required);
extern void *LocalFree(void *memory);

__declspec(dllexport) LONG RegDeleteValueW(HKEY key, const WCHAR *value_name)
{
    UNICODE_STRING name;
    init_ustr(&name, (WCHAR *)(value_name ? value_name : L""),
              wide_len(value_name));
    NTSTATUS st = NtDeleteValueKey((HANDLE)key, &name);
    return NT_SUCCESS(st) ? ERROR_SUCCESS : (LONG)RtlNtStatusToDosError(st);
}

__declspec(dllexport) LONG RegEnumKeyExW(HKEY key, DWORD index, WCHAR *name,
                                          DWORD *name_chars, DWORD *reserved,
                                          WCHAR *class_name,
                                          DWORD *class_chars,
                                          void *last_write)
{
    (void)reserved;
    if (!name || !name_chars || (class_name && !class_chars)) return 87;
    BYTE buffer[256];
    DWORD required = 0;
    NTSTATUS st = NtEnumerateKey((HANDLE)key, index, 0, buffer,
                                 sizeof(buffer), &required);
    if (!NT_SUCCESS(st)) return (LONG)RtlNtStatusToDosError(st);
    DWORD chars = *(DWORD *)(buffer + 12) / sizeof(WCHAR);
    DWORD capacity = *name_chars;
    *name_chars = chars;
    if (capacity <= chars) return 234; /* ERROR_MORE_DATA */
    const WCHAR *source = (const WCHAR *)(buffer + 16);
    for (DWORD i = 0; i < chars; i++) name[i] = source[i];
    name[chars] = 0;
    if (class_chars) {
        DWORD class_capacity = *class_chars;
        *class_chars = 0; /* Cm keys have no class string. */
        if (class_name && !class_capacity) return 234;
        if (class_name) class_name[0] = 0;
    }
    if (last_write) {
        BYTE *time = last_write;
        for (int i = 0; i < 8; i++) time[i] = 0;
    }
    return ERROR_SUCCESS;
}

__declspec(dllexport) LONG RegEnumValueW(HKEY key, DWORD index,
                                          WCHAR *value_name,
                                          DWORD *name_chars, DWORD *reserved,
                                          DWORD *type, BYTE *data,
                                          DWORD *data_size)
{
    (void)reserved;
    if (!value_name || !name_chars || (data && !data_size)) return 87;
    DWORD required = 0;
    NTSTATUS st = NtEnumerateValueKey((HANDLE)key, index, 1, 0, 0,
                                      &required);
    if (!NT_SUCCESS(st) && (DWORD)st != 0xC0000023u)
        return (LONG)RtlNtStatusToDosError(st);
    BYTE *buffer = LocalAlloc(0, required);
    if (!buffer) return 8;
    st = NtEnumerateValueKey((HANDLE)key, index, 1, buffer, required,
                             &required);
    if (!NT_SUCCESS(st)) {
        LocalFree(buffer);
        return (LONG)RtlNtStatusToDosError(st);
    }
    DWORD chars = *(DWORD *)(buffer + 16) / sizeof(WCHAR);
    DWORD capacity = *name_chars;
    *name_chars = chars;
    DWORD size = *(DWORD *)(buffer + 12);
    if (type) *type = *(DWORD *)(buffer + 4);
    DWORD data_capacity = data_size ? *data_size : 0;
    if (data_size) *data_size = size;
    if (capacity <= chars || (data && data_capacity < size)) {
        LocalFree(buffer);
        return 234;
    }
    const WCHAR *source = (const WCHAR *)(buffer + 20);
    for (DWORD i = 0; i < chars; i++) value_name[i] = source[i];
    value_name[chars] = 0;
    if (data && size) {
        const BYTE *src = buffer + *(DWORD *)(buffer + 8);
        for (DWORD i = 0; i < size; i++) data[i] = src[i];
    }
    LocalFree(buffer);
    return ERROR_SUCCESS;
}

__declspec(dllexport) LONG RegQueryInfoKeyW(
    HKEY key, WCHAR *class_name, DWORD *class_chars, DWORD *reserved,
    DWORD *subkeys, DWORD *max_subkey_chars, DWORD *max_class_chars,
    DWORD *values, DWORD *max_value_name_chars, DWORD *max_value_bytes,
    DWORD *security_descriptor_bytes, void *last_write)
{
    if (reserved || (class_name && !class_chars)) return 87;
    BYTE buffer[44];
    DWORD required = 0;
    NTSTATUS st = NtQueryKey((HANDLE)key, 2, buffer, sizeof(buffer),
                             &required);
    if (!NT_SUCCESS(st)) return (LONG)RtlNtStatusToDosError(st);
    if (class_chars) {
        DWORD capacity = *class_chars;
        *class_chars = 0;
        if (class_name && !capacity) return 234;
        if (class_name) class_name[0] = 0;
    }
    if (subkeys) *subkeys = *(DWORD *)(buffer + 20);
    if (max_subkey_chars) *max_subkey_chars = *(DWORD *)(buffer + 24) / 2;
    if (max_class_chars) *max_class_chars = *(DWORD *)(buffer + 28) / 2;
    if (values) *values = *(DWORD *)(buffer + 32);
    if (max_value_name_chars)
        *max_value_name_chars = *(DWORD *)(buffer + 36) / 2;
    if (max_value_bytes) *max_value_bytes = *(DWORD *)(buffer + 40);
    if (security_descriptor_bytes) *security_descriptor_bytes = 0;
    if (last_write) {
        const BYTE *source = buffer;
        BYTE *target = last_write;
        for (int i = 0; i < 8; i++) target[i] = source[i];
    }
    return ERROR_SUCCESS;
}
