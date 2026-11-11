#include <assert.h>
#include <stdio.h>
#include <svindows/Mm.h>

UINT64 MmDirectMapBase;
UINT64 MmTotalPages(void) { return 1024; }

#include "../../kernel/mm/AddressSpace.c"

int main(void)
{
    static UINT64 tables[5][512] __attribute__((aligned(4096)));
    MmDirectMapBase = (UINT64)tables;
    UINT64 user = PTE_PRESENT | PTE_USER;
    tables[1][0] = 2 * PAGE_SIZE | user;
    tables[1][256] = 2 * PAGE_SIZE | user;
    tables[2][0] = 3 * PAGE_SIZE | user;
    tables[3][0] = 4 * PAGE_SIZE | user;
    tables[3][1] = 4 * PAGE_SIZE | PTE_PRESENT;
    tables[4][0] = 6 * PAGE_SIZE | user;
    tables[4][1] = 6 * PAGE_SIZE | user; /* shared physical page, second mapping */
    tables[4][2] = 7 * PAGE_SIZE | PTE_PRESENT;
    tables[4][3] = 8 * PAGE_SIZE | PTE_USER;
    tables[4][4] = 1024 * PAGE_SIZE | user;
    assert(MmCountResidentUserPages(PAGE_SIZE) == 2);
    tables[3][2] = 512 * PAGE_SIZE | user | PTE_LARGE;
    assert(MmCountResidentUserPages(PAGE_SIZE) == 514);
    tables[4][0] = 0;
    assert(MmCountResidentUserPages(PAGE_SIZE) == 513);
    assert(!MmCountResidentUserPages(0) && !MmCountResidentUserPages(MM_INVALID_PHYS));
    puts("PASS resident user mappings exclude supervisor, absent and out-of-RAM pages");
}
