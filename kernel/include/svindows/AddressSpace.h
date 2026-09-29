/* Per-process x86-64 address spaces. */
#ifndef _SVINDOWS_ADDRESS_SPACE_H_
#define _SVINDOWS_ADDRESS_SPACE_H_

#include <nt/Ntdef.h>

UINT64  MmGetCurrentAddressSpace(void);
UINT64  MmCreateAddressSpace(void);
void    MmDestroyAddressSpace(UINT64 directory_table_base);
void    MmSwitchAddressSpace(UINT64 directory_table_base);
BOOLEAN MmMapPageInAddressSpace(UINT64 directory_table_base, UINT64 virt,
                                UINT64 phys, UINT64 flags);

#endif
