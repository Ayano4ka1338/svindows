/* NT object paths and DOS paths for the single C: volume. */
typedef struct _RTL_BUFFER_LOCAL {
    BYTE *Buffer;
    BYTE *StaticBuffer;
    SIZE_T Size;
    SIZE_T StaticSize;
    SIZE_T ReservedForAllocatedSize;
    void *ReservedForIMalloc;
} RTL_BUFFER_LOCAL;

typedef struct _RTL_UNICODE_STRING_BUFFER_LOCAL {
    UNICODE_STRING_NT String;
    RTL_BUFFER_LOCAL ByteBuffer;
    BYTE MinimumStaticBufferForTerminalNul[sizeof(WCHAR)];
} RTL_UNICODE_STRING_BUFFER_LOCAL;

_Static_assert(sizeof(RTL_BUFFER_LOCAL) == 48, "Win64 RTL_BUFFER layout");
_Static_assert(sizeof(RTL_UNICODE_STRING_BUFFER_LOCAL) == 72,
               "Win64 RTL_UNICODE_STRING_BUFFER layout");

NTSTATUS RtlpEnsureBufferSize(ULONG flags, RTL_BUFFER_LOCAL *buffer,
                              SIZE_T required)
{
    if (!buffer || (flags & ~1u)) return STATUS_INVALID_PARAMETER;
    if (required <= buffer->Size) return STATUS_SUCCESS;
    BYTE *replacement = RtlAllocateHeap(rtl_process_heap(), 0, required);
    if (!replacement) return STATUS_NO_MEMORY;
    if (!(flags & 1u) && buffer->Buffer)
        memcpy(replacement, buffer->Buffer, buffer->Size);
    if (buffer->Buffer && buffer->Buffer != buffer->StaticBuffer)
        RtlFreeHeap(rtl_process_heap(), 0, buffer->Buffer);
    buffer->Buffer = replacement;
    buffer->Size = required;
    buffer->ReservedForAllocatedSize = required;
    return STATUS_SUCCESS;
}

static WCHAR path_fold(WCHAR value)
{
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static BOOLEAN path_prefix(const WCHAR *path, SIZE_T length,
                           const WCHAR *prefix, SIZE_T prefix_length)
{
    if (length < prefix_length) return FALSE;
    for (SIZE_T i = 0; i < prefix_length; i++)
        if (path_fold(path[i]) != path_fold(prefix[i])) return FALSE;
    return TRUE;
}

NTSTATUS RtlNtPathNameToDosPathName(ULONG flags,
                                    RTL_UNICODE_STRING_BUFFER_LOCAL *path,
                                    ULONG *disposition, WCHAR **file_part)
{
    if (flags || !path || !path->String.Buffer ||
        (path->String.Length & 1) ||
        path->String.Length > path->String.MaximumLength)
        return STATUS_INVALID_PARAMETER;

    const WCHAR *source = path->String.Buffer;
    SIZE_T length = path->String.Length / sizeof(WCHAR);
    SIZE_T skip = 0, extra = 0;
    ULONG kind = 0;
    static const WCHAR nt_dos[] = {'\\','?','?','\\'};
    static const WCHAR dos_devices[] =
        {'\\','D','o','s','D','e','v','i','c','e','s','\\'};
    static const WCHAR unc[] = {'U','N','C','\\'};
    static const WCHAR system_root[] =
        {'\\','S','y','s','t','e','m','R','o','o','t','\\'};
    static const WCHAR device_volume[] =
        {'\\','D','e','v','i','c','e','\\','H','a','r','d','d','i','s','k',
         'V','o','l','u','m','e'};
    static const WCHAR windows[] =
        {'C',':','\\','W','I','N','D','O','W','S','\\'};

    if (length >= 2 && source[1] == ':') {
        kind = 4; /* already DOS */
    } else if (path_prefix(source, length, nt_dos, 4) ||
               path_prefix(source, length, dos_devices, 12)) {
        skip = path_prefix(source, length, nt_dos, 4) ? 4 : 12;
        if (path_prefix(source + skip, length - skip, unc, 4)) {
            skip += 4;
            extra = 2; /* \\server\share */
            kind = 2;
        } else if (length >= skip + 2 && source[skip + 1] == ':') {
            kind = 3;
        } else {
            return STATUS_NOT_FOUND;
        }
    } else if (path_prefix(source, length, system_root, 12)) {
        skip = 12;
        extra = sizeof(windows) / sizeof(windows[0]);
        kind = 3;
    } else if (path_prefix(source, length, device_volume, 22)) {
        SIZE_T end = 22;
        if (end == length || source[end] < '0' || source[end] > '9')
            return STATUS_NOT_FOUND;
        while (end < length && source[end] >= '0' && source[end] <= '9') end++;
        if (end < length && source[end] != '\\') return STATUS_NOT_FOUND;
        skip = end;
        extra = 2; /* C: plus the original separator */
        if (end == length) extra = 3; /* C:\ */
        kind = 3;
    } else if (length >= 2 && source[0] == '\\' && source[1] == '\\') {
        kind = 2;
    } else if (!length || source[0] != '\\') {
        kind = 1; /* ambiguous relative name */
    } else {
        return STATUS_NOT_FOUND;
    }

    SIZE_T result_length = length - skip + extra;
    if (result_length > 0x7ffe) return STATUS_INVALID_PARAMETER;
    WCHAR *replacement = RtlAllocateHeap(rtl_process_heap(), 0,
                                          (result_length + 1) * sizeof(WCHAR));
    if (!replacement) return STATUS_NO_MEMORY;
    SIZE_T out = 0;
    if (kind == 2 && skip) {
        replacement[out++] = '\\';
        replacement[out++] = '\\';
    } else if (extra == sizeof(windows) / sizeof(windows[0])) {
        for (SIZE_T i = 0; i < extra; i++) replacement[out++] = windows[i];
    } else if (extra && kind == 3) {
        replacement[out++] = 'C';
        replacement[out++] = ':';
        if (skip == length) replacement[out++] = '\\';
    }
    for (SIZE_T i = skip; i < length; i++) replacement[out++] = source[i];
    replacement[out] = 0;

    SIZE_T bytes = (out + 1) * sizeof(WCHAR);
    WCHAR *destination = path->String.Buffer;
    SIZE_T capacity = path->String.MaximumLength;
    if (path->ByteBuffer.Buffer == (BYTE *)destination &&
        path->ByteBuffer.Size < capacity)
        capacity = path->ByteBuffer.Size;
    if (bytes > capacity) {
        /* RTL_BUFFER owns its heap storage when it differs from StaticBuffer. */
        BYTE *old = path->ByteBuffer.Buffer;
        if (old && old != path->ByteBuffer.StaticBuffer &&
            old == (BYTE *)destination)
            RtlFreeHeap(rtl_process_heap(), 0, old);
        destination = replacement;
        path->ByteBuffer.Buffer = (BYTE *)destination;
        path->ByteBuffer.Size = bytes;
        path->ByteBuffer.ReservedForAllocatedSize = bytes;
        path->String.MaximumLength = (WORD)bytes;
    } else {
        memcpy(destination, replacement, bytes);
        RtlFreeHeap(rtl_process_heap(), 0, replacement);
    }
    path->String.Buffer = destination;
    path->String.Length = (WORD)(out * sizeof(WCHAR));
    if (disposition) *disposition = kind;
    if (file_part) {
        WCHAR *part = destination;
        for (SIZE_T i = 0; i < out; i++)
            if (destination[i] == '\\' || destination[i] == '/')
                part = destination + i + 1;
        *file_part = part;
    }
    return STATUS_SUCCESS;
}

/* Allocate a Win32-style NT name. RtlFreeUnicodeString releases Buffer. */
NTSTATUS RtlDosPathNameToNtPathName_U_WithStatus(const WCHAR *dos,
    UNICODE_STRING_NT *nt, WCHAR **file_part, void *reserved)
{
    if (!dos || !nt || reserved) return STATUS_INVALID_PARAMETER;
    SIZE_T length = 0;
    while (dos[length] && length < 0x7ff0) length++;
    if (!length || length == 0x7ff0) return STATUS_INVALID_PARAMETER;

    static const WCHAR nt_prefix[] = {'\\','?','?','\\'};
    SIZE_T skip = 0;
    BOOLEAN unc = FALSE;
    if (length >= 4 && dos[0] == '\\' && dos[1] == '\\' &&
        (dos[2] == '?' || dos[2] == '.') && dos[3] == '\\') {
        skip = 4;
        static const WCHAR unc_marker[] = {'U','N','C','\\'};
        if (path_prefix(dos + skip, length - skip, unc_marker, 4)) {
            skip += 4;
            unc = TRUE;
        } else if (length < skip + 3 || dos[skip + 1] != ':' ||
                   dos[skip + 2] != '\\') {
            return STATUS_NOT_FOUND;
        }
    } else if (length >= 3 && dos[1] == ':' && dos[2] == '\\') {
        /* absolute drive path */
    } else if (length >= 3 && dos[0] == '\\' && dos[1] == '\\' &&
               dos[2] != '\\') {
        skip = 2;
        unc = TRUE;
    } else if (dos[0] == '\\' && (length == 1 || dos[1] != '\\')) {
        /* root-relative path on the sole mounted C: volume */
    } else {
        return STATUS_NOT_FOUND;
    }
    SIZE_T extra = 4 + (unc ? 4 : dos[0] == '\\' && !skip ? 2 : 0);
    SIZE_T units = extra + length - skip;
    if (units > 0x7ffe) return STATUS_INVALID_PARAMETER;
    WCHAR *out = RtlAllocateHeap(rtl_process_heap(), 0, (units + 1) * sizeof(WCHAR));
    if (!out) return STATUS_NO_MEMORY;
    SIZE_T pos = 0;
    for (SIZE_T i = 0; i < 4; i++) out[pos++] = nt_prefix[i];
    if (unc) {
        out[pos++] = 'U'; out[pos++] = 'N';
        out[pos++] = 'C'; out[pos++] = '\\';
    } else if (dos[0] == '\\' && !skip) {
        out[pos++] = 'C'; out[pos++] = ':';
    }
    for (SIZE_T i = skip; i < length; i++)
        out[pos++] = dos[i] == '/' ? '\\' : dos[i];
    out[pos] = 0;
    nt->Length = (WORD)(pos * sizeof(WCHAR));
    nt->MaximumLength = (WORD)((pos + 1) * sizeof(WCHAR));
    nt->Buffer = out;
    if (file_part) {
        WCHAR *part = out;
        for (SIZE_T i = 0; i < pos; i++)
            if (out[i] == '\\') part = out + i + 1;
        *file_part = part;
    }
    return STATUS_SUCCESS;
}

BOOLEAN RtlDosPathNameToNtPathName_U(const WCHAR *dos,
    UNICODE_STRING_NT *nt, WCHAR **file_part, void *reserved)
{
    return NT_SUCCESS(RtlDosPathNameToNtPathName_U_WithStatus(
        dos, nt, file_part, reserved));
}

typedef struct {
    UNICODE_STRING_NT Name;
    HANDLE Directory;
    void *CurrentDirectoryReference;
} RELATIVE_NAME_LOCAL;

BOOLEAN RtlDosPathNameToRelativeNtPathName_U(const WCHAR *dos,
    UNICODE_STRING_NT *nt, WCHAR **file_part, RELATIVE_NAME_LOCAL *relative)
{
    /* An absolute NT name is sufficient for NtOpenFile even when a caller
     * accepts an optional directory-relative optimization. */
    if (relative) memset(relative, 0, sizeof(*relative));
    return RtlDosPathNameToNtPathName_U(dos, nt, file_part, 0);
}
void RtlReleaseRelativeName(RELATIVE_NAME_LOCAL *relative)
{
    /* This implementation never acquires a directory reference. The full
     * allocated NT name is separately owned by RtlFreeUnicodeString. */
    if (relative) memset(relative, 0, sizeof(*relative));
}
