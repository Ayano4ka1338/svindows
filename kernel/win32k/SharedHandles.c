/* Clear only a GUI metadata cell in its owning process, without changing CR3
 * or temporarily making a client read-only mapping writable. */
#include <svindows/Win32k.h>
#include <svindows/Ke.h>
#include <svindows/Ps.h>
#include <svindows/Mm.h>
#include <svindows/Rtl.h>

BOOLEAN W32kClearSharedHandle(PKPROCESS process, BOOLEAN gdi, UINT16 index)
{
    if (!process || !process->DirectoryTableBase) return FALSE;
    UINT64 va = (gdi ? PROCESS_GDI_SHARED_TABLE_VA : PROCESS_USER_SHARED_TABLE_VA)
                + (UINT64)index * 24;
    UINT64 first = MmGetPhysicalAddressInAddressSpace(process->DirectoryTableBase, va);
    SIZE_T bytes = PAGE_SIZE - (va & PAGE_MASK);
    if (bytes > 24) bytes = 24;
    UINT64 second = MM_INVALID_PHYS;
    if (bytes < 24) {
        second = MmGetPhysicalAddressInAddressSpace(process->DirectoryTableBase, va + bytes);
        if (second == MM_INVALID_PHYS) return FALSE;
    }
    if (first == MM_INVALID_PHYS) return FALSE;
    memset(MmPhysToVirt(first), 0, bytes);
    if (bytes < 24) memset(MmPhysToVirt(second), 0, 24 - bytes);
    return TRUE;
}
