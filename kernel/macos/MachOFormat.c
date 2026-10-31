#include "MachO.h"
#include <svindows/Mm.h>
#include <svindows/Rtl.h>

static UINT32 macho_read_be32(const UINT8 *bytes)
{
    return ((UINT32)bytes[0] << 24) | ((UINT32)bytes[1] << 16) |
           ((UINT32)bytes[2] << 8) | bytes[3];
}

static UINT64 macho_read_be64(const UINT8 *bytes)
{
    return ((UINT64)macho_read_be32(bytes) << 32) | macho_read_be32(bytes + 4);
}

static BOOLEAN macho_copy_command_string(const MACHO_LOAD_COMMAND *command,
                                         UINT32 offset, char *output,
                                         SIZE_T capacity)
{
    if (!output || capacity < 2 || offset < sizeof(*command) + sizeof(UINT32) ||
        offset >= command->Size)
        return FALSE;
    const char *source = (const char *)command + offset;
    SIZE_T remaining = command->Size - offset;
    SIZE_T length = 0;
    while (length < remaining && source[length]) length++;
    if (!length || length == remaining || length >= capacity) return FALSE;
    memcpy(output, source, length);
    output[length] = 0;
    return TRUE;
}

static BOOLEAN macho_is_dylib_command(UINT32 command)
{
    return command == MACHO_COMMAND_LOAD_DYLIB ||
           command == MACHO_COMMAND_LAZY_LOAD_DYLIB ||
           command == MACHO_COMMAND_LOAD_WEAK_DYLIB ||
           command == MACHO_COMMAND_REEXPORT_DYLIB ||
           command == MACHO_COMMAND_LOAD_UPWARD_DYLIB;
}

BOOLEAN MacOsSelectMachO64(const void *image, SIZE_T size,
                           const void **slice_out, SIZE_T *slice_size_out,
                           BOOLEAN *universal_out)
{
    if (!image || !slice_out || !slice_size_out ||
        size < sizeof(MACHO_HEADER_64))
        return FALSE;
    const UINT8 *bytes = image;
    if (((const MACHO_HEADER_64 *)image)->Magic == MACHO_MAGIC_64) {
        if (((const MACHO_HEADER_64 *)image)->CpuType !=
            MACHO_CPU_TYPE_X86_64)
            return FALSE;
        *slice_out = image;
        *slice_size_out = size;
        if (universal_out) *universal_out = FALSE;
        return TRUE;
    }
    if (size < 8) return FALSE;
    UINT32 magic = macho_read_be32(bytes);
    BOOLEAN fat64;
    if (magic == MACHO_FAT_MAGIC) fat64 = FALSE;
    else if (magic == MACHO_FAT_MAGIC_64) fat64 = TRUE;
    else return FALSE;

    UINT32 count = macho_read_be32(bytes + 4);
    UINT32 record_size = fat64 ? 32u : 20u;
    if (!count || count > MACHO_MAX_ARCHITECTURES ||
        (UINT64)count * record_size > size - 8)
        return FALSE;
    UINT64 table_end = 8 + (UINT64)count * record_size;
    UINT64 offsets[MACHO_MAX_ARCHITECTURES];
    UINT64 lengths[MACHO_MAX_ARCHITECTURES];
    INT32 cpu_types[MACHO_MAX_ARCHITECTURES];
    UINT64 selected_offset = 0, selected_length = 0;
    UINT32 selected_count = 0;
    for (UINT32 i = 0; i < count; i++) {
        const UINT8 *record = bytes + 8 + (UINT64)i * record_size;
        cpu_types[i] = (INT32)macho_read_be32(record);
        UINT64 offset = fat64 ? macho_read_be64(record + 8) :
                                macho_read_be32(record + 8);
        UINT64 length = fat64 ? macho_read_be64(record + 16) :
                                macho_read_be32(record + 12);
        UINT32 alignment = macho_read_be32(record + (fat64 ? 24 : 16));
        if ((!fat64 && alignment > 31) || (fat64 && alignment > 63) ||
            !length || offset < table_end || offset > size ||
            length > size - offset ||
            (offset & ((1ULL << alignment) - 1)))
            return FALSE;
        offsets[i] = offset;
        lengths[i] = length;
        for (UINT32 j = 0; j < i; j++) {
            if (offset < offsets[j] + lengths[j] &&
                offsets[j] < offset + length)
                return FALSE;
        }
        if (cpu_types[i] == MACHO_CPU_TYPE_X86_64) {
            selected_offset = offset;
            selected_length = length;
            selected_count++;
        }
    }
    if (selected_count != 1 ||
        selected_length < sizeof(MACHO_HEADER_64))
        return FALSE;
    const MACHO_HEADER_64 *header =
        (const MACHO_HEADER_64 *)(bytes + selected_offset);
    if (header->Magic != MACHO_MAGIC_64 ||
        header->CpuType != MACHO_CPU_TYPE_X86_64)
        return FALSE;
    *slice_out = bytes + selected_offset;
    *slice_size_out = (SIZE_T)selected_length;
    if (universal_out) *universal_out = TRUE;
    return TRUE;
}

BOOLEAN MacOsIsMachO64(const void *image, SIZE_T size)
{
    const void *slice;
    SIZE_T slice_size;
    return MacOsSelectMachO64(image, size, &slice, &slice_size, NULL);
}

static NTSTATUS macho_validate(const void *image, SIZE_T size,
                               MACHO_SEGMENT_64 segments[MACHO_MAX_SEGMENTS],
                               UINT32 *segment_count,
                               UINT64 *entry_offset,
                               BOOLEAN *requires_dyld,
                               char dynamic_linker[260],
                               UINT32 *dependency_count,
                               UINT32 *runpath_count)
{
    if (!MacOsIsMachO64(image, size)) return STATUS_INVALID_IMAGE_FORMAT;
    const MACHO_HEADER_64 *header = image;
    if (header->CpuType != MACHO_CPU_TYPE_X86_64 ||
        header->FileType != MACHO_FILE_EXECUTE ||
        !header->CommandCount || header->CommandCount > MACHO_MAX_COMMANDS ||
        header->CommandBytes > size - sizeof(*header))
        return STATUS_INVALID_IMAGE_FORMAT;

    const UINT8 *bytes = image;
    UINT64 cursor = sizeof(*header);
    UINT64 command_end = cursor + header->CommandBytes;
    UINT32 count = 0;
    UINT64 main_offset = ~(UINT64)0;
    *requires_dyld = FALSE;
    *dependency_count = 0;
    *runpath_count = 0;
    memset(dynamic_linker, 0, 260);
    for (UINT32 i = 0; i < header->CommandCount; i++) {
        if (cursor > command_end || command_end - cursor <
                                   sizeof(MACHO_LOAD_COMMAND))
            return STATUS_INVALID_IMAGE_FORMAT;
        const MACHO_LOAD_COMMAND *command =
            (const MACHO_LOAD_COMMAND *)(bytes + cursor);
        if (command->Size < sizeof(*command) || (command->Size & 3u) ||
            command->Size > command_end - cursor)
            return STATUS_INVALID_IMAGE_FORMAT;
        if (command->Command == MACHO_COMMAND_SEGMENT_64) {
            if (command->Size < sizeof(MACHO_SEGMENT_64) ||
                count == MACHO_MAX_SEGMENTS)
                return STATUS_INVALID_IMAGE_FORMAT;
            MACHO_SEGMENT_64 segment;
            memcpy(&segment, command, sizeof(segment));
            if (segment.SectionCount >
                (command->Size - sizeof(segment)) / 80u)
                return STATUS_INVALID_IMAGE_FORMAT;
            if (command->Size != sizeof(segment) +
                                 segment.SectionCount * 80u)
                return STATUS_INVALID_IMAGE_FORMAT;
            segments[count++] = segment;
        } else if (command->Command == MACHO_COMMAND_MAIN) {
            if (main_offset != ~(UINT64)0 ||
                command->Size != sizeof(MACHO_MAIN_COMMAND))
                return STATUS_INVALID_IMAGE_FORMAT;
            const MACHO_MAIN_COMMAND *main = (const void *)command;
            main_offset = main->EntryOffset;
        } else if (macho_is_dylib_command(command->Command)) {
            if (command->Size < 24 ||
                !macho_copy_command_string(command,
                    *(const UINT32 *)(bytes + cursor + 8),
                    (char[260]){0}, 260))
                return STATUS_INVALID_IMAGE_FORMAT;
            (*dependency_count)++;
            *requires_dyld = TRUE;
        } else if (command->Command == MACHO_COMMAND_LOAD_DYLINKER) {
            if (command->Size < 12 || dynamic_linker[0] ||
                !macho_copy_command_string(command,
                    *(const UINT32 *)(bytes + cursor + 8),
                    dynamic_linker, 260))
                return STATUS_INVALID_IMAGE_FORMAT;
            *requires_dyld = TRUE;
        } else if (command->Command == MACHO_COMMAND_RPATH) {
            char runpath[260];
            if (command->Size < 12 ||
                !macho_copy_command_string(command,
                    *(const UINT32 *)(bytes + cursor + 8),
                    runpath, sizeof(runpath)))
                return STATUS_INVALID_IMAGE_FORMAT;
            (*runpath_count)++;
            *requires_dyld = TRUE;
        }
        cursor += command->Size;
    }
    if (cursor != command_end || !count || main_offset == ~(UINT64)0)
        return STATUS_INVALID_IMAGE_FORMAT;
    *segment_count = count;
    *entry_offset = main_offset;
    return STATUS_SUCCESS;
}

NTSTATUS MacOsPrepareMachO64(const void *image, SIZE_T size,
                                    const void **slice_out,
                                    SIZE_T *slice_size_out,
                                    MACHO_SEGMENT_64 segments[MACHO_MAX_SEGMENTS],
                                    MACOS_MACHO_INFO *info)
{
    BOOLEAN universal = FALSE;
    const void *slice = NULL;
    SIZE_T slice_size = 0;
    if (!MacOsSelectMachO64(image, size, &slice, &slice_size, &universal))
        return STATUS_INVALID_IMAGE_FORMAT;

    UINT32 count = 0, dependency_count = 0, runpath_count = 0;
    UINT64 entry_offset = 0;
    BOOLEAN requires_dyld = FALSE;
    char dynamic_linker[260];
    NTSTATUS status = macho_validate(slice, slice_size, segments, &count,
                                     &entry_offset, &requires_dyld,
                                     dynamic_linker, &dependency_count,
                                     &runpath_count);
    if (!NT_SUCCESS(status)) return status;

    UINT64 mapped_bytes = 0, image_end = 0, entry = 0;
    BOOLEAN entry_found = FALSE;
    for (UINT32 i = 0; i < count; i++) {
        MACHO_SEGMENT_64 *segment = &segments[i];
        if (segment->FileSize > segment->VmSize ||
            segment->FileOffset > slice_size ||
            segment->FileSize > slice_size - segment->FileOffset ||
            (segment->InitialProtection & ~7) ||
            (segment->MaximumProtection & ~7) ||
            (segment->InitialProtection & ~segment->MaximumProtection))
            return STATUS_INVALID_IMAGE_FORMAT;
        if (!segment->VmSize || !segment->InitialProtection) continue;
        if (segment->VmAddress < MACHO_USER_LOW ||
            segment->VmAddress >= MACHO_USER_HIGH ||
            segment->VmSize > MACHO_USER_HIGH - segment->VmAddress ||
            ((segment->VmAddress - segment->FileOffset) & PAGE_MASK))
            return STATUS_INVALID_IMAGE_FORMAT;
        UINT64 first = PAGE_ALIGN(segment->VmAddress);
        UINT64 last = PAGE_ALIGN_UP(segment->VmAddress + segment->VmSize);
        if (last < first || last - first > MACHO_MAX_MAPPED_BYTES ||
            mapped_bytes > MACHO_MAX_MAPPED_BYTES - (last - first))
            return STATUS_INVALID_IMAGE_FORMAT;
        mapped_bytes += last - first;
        if (last > image_end) image_end = last;
        if ((segment->InitialProtection & MACHO_PROTECTION_EXECUTE) &&
            entry_offset >= segment->FileOffset &&
            entry_offset - segment->FileOffset < segment->FileSize) {
            entry = segment->VmAddress +
                    (entry_offset - segment->FileOffset);
            entry_found = TRUE;
        }
        for (UINT32 j = 0; j < i; j++) {
            if (!segments[j].VmSize || !segments[j].InitialProtection)
                continue;
            UINT64 other_first = PAGE_ALIGN(segments[j].VmAddress);
            UINT64 other_last = PAGE_ALIGN_UP(segments[j].VmAddress +
                                              segments[j].VmSize);
            if (first < other_last && other_first < last)
                return STATUS_INVALID_IMAGE_FORMAT;
        }
    }
    if (!entry_found) return STATUS_INVALID_IMAGE_FORMAT;

    memset(info, 0, sizeof(*info));
    info->Entry = entry;
    info->ImageEnd = image_end;
    info->SegmentCount = count;
    info->PositionIndependent =
        (((const MACHO_HEADER_64 *)slice)->Flags & MACHO_FLAG_PIE) != 0;
    info->RequiresDyld = requires_dyld;
    info->UniversalBinary = universal;
    info->DependencyCount = dependency_count;
    info->RunPathCount = runpath_count;
    memcpy(info->DynamicLinker, dynamic_linker, sizeof(info->DynamicLinker));
    *slice_out = slice;
    *slice_size_out = slice_size;
    return STATUS_SUCCESS;
}

NTSTATUS MacOsInspectMachO64(const void *image, SIZE_T size,
                             MACOS_MACHO_INFO *info)
{
    if (!image || !info) return STATUS_INVALID_PARAMETER;
    MACHO_SEGMENT_64 segments[MACHO_MAX_SEGMENTS];
    const void *slice;
    SIZE_T slice_size;
    return MacOsPrepareMachO64(image, size, &slice, &slice_size,
                               segments, info);
}

NTSTATUS MacOsQueryDependency(const void *image, SIZE_T size, UINT32 index,
                              char *path, SIZE_T capacity, BOOLEAN *weak)
{
    if (!path || capacity < 2 || !weak) return STATUS_INVALID_PARAMETER;
    const void *slice = NULL;
    SIZE_T slice_size = 0;
    MACHO_SEGMENT_64 segments[MACHO_MAX_SEGMENTS];
    MACOS_MACHO_INFO info;
    NTSTATUS status = MacOsPrepareMachO64(image, size, &slice, &slice_size,
                                          segments, &info);
    if (!NT_SUCCESS(status)) return status;
    if (index >= info.DependencyCount) return STATUS_NO_SUCH_FILE;

    const MACHO_HEADER_64 *header = slice;
    const UINT8 *bytes = slice;
    UINT64 cursor = sizeof(*header);
    UINT32 found = 0;
    for (UINT32 i = 0; i < header->CommandCount; i++) {
        const MACHO_LOAD_COMMAND *command =
            (const MACHO_LOAD_COMMAND *)(bytes + cursor);
        if (macho_is_dylib_command(command->Command)) {
            if (found++ == index) {
                if (command->Size < 24 ||
                    !macho_copy_command_string(command,
                        *(const UINT32 *)(bytes + cursor + 8), path, capacity))
                    return STATUS_BUFFER_TOO_SMALL;
                *weak = command->Command == MACHO_COMMAND_LOAD_WEAK_DYLIB;
                return STATUS_SUCCESS;
            }
        }
        cursor += command->Size;
    }
    return STATUS_NO_SUCH_FILE;
}

NTSTATUS MacOsQueryRunPath(const void *image, SIZE_T size, UINT32 index,
                           char *path, SIZE_T capacity)
{
    if (!path || capacity < 2) return STATUS_INVALID_PARAMETER;
    const void *slice = NULL;
    SIZE_T slice_size = 0;
    MACHO_SEGMENT_64 segments[MACHO_MAX_SEGMENTS];
    MACOS_MACHO_INFO info;
    NTSTATUS status = MacOsPrepareMachO64(image, size, &slice, &slice_size,
                                          segments, &info);
    if (!NT_SUCCESS(status)) return status;
    if (index >= info.RunPathCount) return STATUS_NO_SUCH_FILE;

    const MACHO_HEADER_64 *header = slice;
    const UINT8 *bytes = slice;
    UINT64 cursor = sizeof(*header);
    UINT32 found = 0;
    for (UINT32 i = 0; i < header->CommandCount; i++) {
        const MACHO_LOAD_COMMAND *command =
            (const MACHO_LOAD_COMMAND *)(bytes + cursor);
        if (command->Command == MACHO_COMMAND_RPATH && found++ == index) {
            if (command->Size < 12 ||
                !macho_copy_command_string(command,
                    *(const UINT32 *)(bytes + cursor + 8), path, capacity))
                return STATUS_BUFFER_TOO_SMALL;
            return STATUS_SUCCESS;
        }
        cursor += command->Size;
    }
    return STATUS_NO_SUCH_FILE;
}

