/*
 * io/BlockCache.c - small write-back cache for the boot disk.
 *
 * The current system is uniprocessor and all filesystem I/O is synchronous.
 * Dirty entries remain memory-resident until an explicit flush, close-time
 * filesystem flush, or dirty eviction. Reads always observe cached writes.
 */
#include <svindows/Io.h>
#include <svindows/Rtl.h>

#define IO_BLOCK_CACHE_ENTRIES 64
#ifndef IO_FAULT_AFTER_WRITES
#define IO_FAULT_AFTER_WRITES (-1)
#endif

typedef struct _IO_BLOCK_CACHE_ENTRY {
    UINT32 Lba;
    UINT32 Stamp;
    BOOLEAN Valid;
    BOOLEAN Dirty;
    UINT8 Data[ATA_SECTOR_SIZE];
} IO_BLOCK_CACHE_ENTRY;

typedef struct _IO_BLOCK_BACKUP {
    UINT32 Lba;
    UINT8 Data[ATA_SECTOR_SIZE];
} IO_BLOCK_BACKUP;

static IO_BLOCK_CACHE_ENTRY g_block_cache[IO_BLOCK_CACHE_ENTRIES];
static UINT32 g_block_stamp;
static INT64 g_write_fault_after = IO_FAULT_AFTER_WRITES;
static INT64 g_physical_write_count;
static IO_BLOCK_BACKUP g_transaction_backup[IO_BLOCK_CACHE_ENTRIES];
static UINT32 g_transaction_count;
static BOOLEAN g_transaction_active;

static BOOLEAN IopWriteSector(UINT32 lba, const void *data)
{
    if (g_write_fault_after == 0) {
        /* One-shot failure: later writes remain available to rollback paths. */
        g_write_fault_after = -1;
        return FALSE;
    }
    if (!AtaWriteSectors(lba, 1, data))
        return FALSE;
    g_physical_write_count++;
    if (g_write_fault_after > 0)
        g_write_fault_after--;
    return TRUE;
}

void IoBlockSetWriteFault(INT64 count)
{
    g_write_fault_after = count;
}

INT64 IoBlockGetWriteCount(void)
{
    return g_physical_write_count;
}

static IO_BLOCK_CACHE_ENTRY *IopFindBlock(UINT32 lba)
{
    for (UINT32 i = 0; i < IO_BLOCK_CACHE_ENTRIES; i++)
        if (g_block_cache[i].Valid && g_block_cache[i].Lba == lba)
            return &g_block_cache[i];
    return NULL;
}

static IO_BLOCK_CACHE_ENTRY *IopAcquireBlock(UINT32 lba, BOOLEAN load)
{
    IO_BLOCK_CACHE_ENTRY *entry = IopFindBlock(lba);
    if (entry) {
        entry->Stamp = ++g_block_stamp;
        return entry;
    }

    entry = &g_block_cache[0];
    for (UINT32 i = 0; i < IO_BLOCK_CACHE_ENTRIES; i++) {
        if (!g_block_cache[i].Valid) {
            entry = &g_block_cache[i];
            break;
        }
        if (g_block_cache[i].Stamp < entry->Stamp)
            entry = &g_block_cache[i];
    }
    if (entry->Valid && entry->Dirty) {
        if (!IopWriteSector(entry->Lba, entry->Data) ||
            !AtaFlushCache())
            return NULL;
        entry->Dirty = FALSE;
    }
    /* A failed ATA read may partially overwrite the buffer. Do not leave
     * those bytes published under the evicted block's old identity. */
    entry->Valid = FALSE;
    if (load && !AtaReadSectors(lba, 1, entry->Data))
        return NULL;
    entry->Lba = lba;
    entry->Stamp = ++g_block_stamp;
    entry->Valid = TRUE;
    entry->Dirty = FALSE;
    return entry;
}

BOOLEAN IoBlockReadSectors(UINT32 lba, UINT8 count, void *buffer)
{
    if (!count || !buffer)
        return FALSE;
    UINT8 *out = (UINT8 *)buffer;
    for (UINT32 i = 0; i < count; i++) {
        IO_BLOCK_CACHE_ENTRY *entry = IopAcquireBlock(lba + i, TRUE);
        if (!entry)
            return FALSE;
        memcpy(out + i * ATA_SECTOR_SIZE, entry->Data, ATA_SECTOR_SIZE);
    }
    return TRUE;
}

BOOLEAN IoBlockWriteSectors(UINT32 lba, UINT8 count, const void *buffer)
{
    if (!count || !buffer)
        return FALSE;
    const UINT8 *in = (const UINT8 *)buffer;
    for (UINT32 i = 0; i < count; i++) {
        UINT32 sector = lba + i;
        IO_BLOCK_CACHE_ENTRY *entry = IopAcquireBlock(sector,
                                                       g_transaction_active);
        if (!entry)
            return FALSE;
        if (g_transaction_active) {
            BOOLEAN saved = FALSE;
            for (UINT32 j = 0; j < g_transaction_count; j++) {
                if (g_transaction_backup[j].Lba == sector) {
                    saved = TRUE;
                    break;
                }
            }
            if (!saved) {
                if (g_transaction_count == IO_BLOCK_CACHE_ENTRIES)
                    return FALSE;
                IO_BLOCK_BACKUP *backup =
                    &g_transaction_backup[g_transaction_count++];
                backup->Lba = sector;
                memcpy(backup->Data, entry->Data, ATA_SECTOR_SIZE);
            }
        }
        memcpy(entry->Data, in + i * ATA_SECTOR_SIZE, ATA_SECTOR_SIZE);
        entry->Dirty = TRUE;
    }
    return TRUE;
}

BOOLEAN IoBlockFlush(void)
{
    BOOLEAN dirty = FALSE;
    for (UINT32 i = 0; i < IO_BLOCK_CACHE_ENTRIES; i++) {
        IO_BLOCK_CACHE_ENTRY *entry = &g_block_cache[i];
        if (!entry->Valid || !entry->Dirty)
            continue;
        dirty = TRUE;
        if (!IopWriteSector(entry->Lba, entry->Data))
            return FALSE;
    }
    if (dirty && !AtaFlushCache())
        return FALSE;
    for (UINT32 i = 0; i < IO_BLOCK_CACHE_ENTRIES; i++)
        g_block_cache[i].Dirty = FALSE;
    return TRUE;
}

BOOLEAN IoBlockBeginTransaction(void)
{
    if (g_transaction_active)
        return FALSE;
    g_transaction_count = 0;
    g_transaction_active = TRUE;
    return TRUE;
}

BOOLEAN IoBlockCommitTransaction(void)
{
    if (!g_transaction_active)
        return FALSE;
    if (!IoBlockFlush())
        return FALSE;
    g_transaction_active = FALSE;
    g_transaction_count = 0;
    return TRUE;
}

BOOLEAN IoBlockRollbackTransaction(void)
{
    if (!g_transaction_active)
        return FALSE;
    for (UINT32 i = 0; i < g_transaction_count; i++) {
        IO_BLOCK_BACKUP *backup = &g_transaction_backup[i];
        IO_BLOCK_CACHE_ENTRY *entry = IopAcquireBlock(backup->Lba, FALSE);
        if (!entry)
            return FALSE;
        memcpy(entry->Data, backup->Data, ATA_SECTOR_SIZE);
        entry->Dirty = TRUE;
    }
    if (!IoBlockFlush())
        return FALSE;
    g_transaction_active = FALSE;
    g_transaction_count = 0;
    return TRUE;
}
