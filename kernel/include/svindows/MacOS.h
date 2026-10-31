#ifndef _SVINDOWS_MACOS_H_
#define _SVINDOWS_MACOS_H_

#include <nt/Ntdef.h>
#include <nt/Ntstatus.h>

typedef struct _MACOS_MACHO_INFO {
    UINT64 Entry;
    UINT64 ImageEnd;
    UINT32 SegmentCount;
    UINT32 DependencyCount;
    UINT32 RunPathCount;
    BOOLEAN PositionIndependent;
    BOOLEAN RequiresDyld;
    BOOLEAN UniversalBinary;
    char DynamicLinker[260];
} MACOS_MACHO_INFO;

/* The initial native macOS image path targets little-endian x86-64 MH_EXECUTE
 * files with LC_MAIN. Fat binaries and dyld-dependent startup are separate
 * loader stages. */
BOOLEAN MacOsIsMachO64(const void *image, SIZE_T size);
BOOLEAN MacOsSelectMachO64(const void *image, SIZE_T size,
                           const void **slice_out, SIZE_T *slice_size_out,
                           BOOLEAN *universal_out);
NTSTATUS MacOsInspectMachO64(const void *image, SIZE_T size,
                             MACOS_MACHO_INFO *info);
NTSTATUS MacOsQueryDependency(const void *image, SIZE_T size, UINT32 index,
                              char *path, SIZE_T capacity, BOOLEAN *weak);
NTSTATUS MacOsQueryRunPath(const void *image, SIZE_T size, UINT32 index,
                           char *path, SIZE_T capacity);
NTSTATUS MacOsLoadMachO64(const void *image, SIZE_T size,
                          MACOS_MACHO_INFO *info);

#endif
