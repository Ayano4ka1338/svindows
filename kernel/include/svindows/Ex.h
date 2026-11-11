/*
 * svindows/Ex.h - Executive support (Ex): the kernel pool allocator.
 *
 * ExAllocatePool is the kernel's general-purpose heap, the NT equivalent of
 * kmalloc/malloc. The paged/non-paged distinction is part of the interface for
 * source compatibility, but for now every allocation is non-paged (backed by
 * resident RAM).
 */
#ifndef _SVINDOWS_EX_H_
#define _SVINDOWS_EX_H_

#include <nt/Ntdef.h>

typedef enum _POOL_TYPE {
    NonPagedPool = 0,
    PagedPool    = 1,
} POOL_TYPE;

/* Bring up the pool. Requires Mm (MmMapPage) to be functional. */
void ExInitializePool(void);

PVOID ExAllocatePool(POOL_TYPE type, SIZE_T size);
PVOID ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T size, ULONG tag);
void  ExFreePool(PVOID p);

/* Diagnostics. */
SIZE_T ExPoolBytesInUse(void);
typedef struct { UINT64 Pages, Allocations, Frees; } EX_POOL_USAGE;
void ExQueryPoolUsage(EX_POOL_USAGE *usage);

#endif /* _SVINDOWS_EX_H_ */
