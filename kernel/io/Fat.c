/*
 * io/Fat.c - a compact FAT32 driver.
 *
 * Parses the BPB, walks the root directory with VFAT long-file-name support,
 * and follows cluster chains to read files into pool buffers.
 */
#include <svindows/Io.h>
#include <svindows/Ex.h>
#include <svindows/Ke.h>
#include <svindows/Rtl.h>

/* BIOS Parameter Block (FAT32). */
typedef struct PACKED _FAT_BPB {
    UINT8  Jump[3];
    UINT8  OemName[8];
    UINT16 BytesPerSector;      /* 0x0B */
    UINT8  SectorsPerCluster;   /* 0x0D */
    UINT16 ReservedSectors;     /* 0x0E */
    UINT8  NumFats;             /* 0x10 */
    UINT16 RootEntryCount;      /* 0x11 (0 for FAT32) */
    UINT16 TotalSectors16;      /* 0x13 */
    UINT8  Media;               /* 0x15 */
    UINT16 FatSize16;           /* 0x16 (0 for FAT32) */
    UINT16 SectorsPerTrack;     /* 0x18 */
    UINT16 NumHeads;            /* 0x1A */
    UINT32 HiddenSectors;       /* 0x1C */
    UINT32 TotalSectors32;      /* 0x20 */
    UINT32 FatSize32;           /* 0x24 */
    UINT16 ExtFlags;            /* 0x28 */
    UINT16 FsVersion;           /* 0x2A */
    UINT32 RootCluster;         /* 0x2C */
    UINT16 FsInfoSector;        /* 0x30 */
    UINT16 BackupBootSector;    /* 0x32 */
} FAT_BPB;

typedef struct PACKED _FAT_FSINFO {
    UINT32 LeadSignature;
    UINT8  Reserved1[480];
    UINT32 StructSignature;
    UINT32 FreeClusterCount;
    UINT32 NextFreeCluster;
    UINT8  Reserved2[12];
    UINT32 TrailSignature;
} FAT_FSINFO;

/* 8.3 directory entry. */
typedef struct PACKED _FAT_DIRENT {
    UINT8  Name[11];
    UINT8  Attr;
    UINT8  NtReserved;
    UINT8  CreateTimeTenth;
    UINT16 CreateTime;
    UINT16 CreateDate;
    UINT16 AccessDate;
    UINT16 FirstClusterHigh;
    UINT16 WriteTime;
    UINT16 WriteDate;
    UINT16 FirstClusterLow;
    UINT32 FileSize;
} FAT_DIRENT;

typedef struct PACKED _FAT_LFN_ENTRY {
    UINT8  Order;
    UINT16 Name1[5];
    UINT8  Attr;
    UINT8  Type;
    UINT8  Checksum;
    UINT16 Name2[6];
    UINT16 FirstClusterLow;
    UINT16 Name3[2];
} FAT_LFN_ENTRY;

#define ATTR_LONG_NAME 0x0F
#define FAT_EOC        0x0FFFFFF8u
#define FAT_BAD        0x0FFFFFF7u

typedef struct _FAT_ENTRY_LOCATION {
    UINT32 Cluster;
    UINT32 Index;
} FAT_ENTRY_LOCATION;

typedef struct _FAT_LFN_STATE {
    UINT16 Name[260];
    UINT8 Checksum;
    UINT8 NextOrdinal;
    BOOLEAN Valid;
} FAT_LFN_STATE;

static struct {
    BOOLEAN Mounted;
    UINT32  FatStartLba;
    UINT32  FatSectors;
    UINT32  NumFats;
    UINT32  DataStartLba;
    UINT32  TotalClusters;
    UINT32  SectorsPerCluster;
    UINT32  BytesPerCluster;
    UINT32  RootCluster;
    UINT16  FsInfoSector;
    UINT16  BackupBootSector;
} g_fat;

static BOOLEAN fat_update_fsinfo(INT32 free_delta, UINT32 next_free)
{
    if (!g_fat.FsInfoSector)
        return TRUE;

    UINT8 sector[ATA_SECTOR_SIZE];
    if (!IoBlockReadSectors(g_fat.FsInfoSector, 1, sector))
        return FALSE;
    FAT_FSINFO *info = (FAT_FSINFO *)sector;
    if (info->LeadSignature != 0x41615252u ||
        info->StructSignature != 0x61417272u ||
        info->TrailSignature != 0xAA550000u)
        return TRUE;

    if (info->FreeClusterCount != 0xffffffffu) {
        if (free_delta < 0) {
            UINT32 used = (UINT32)(-free_delta);
            info->FreeClusterCount = info->FreeClusterCount > used
                                   ? info->FreeClusterCount - used : 0;
        } else if (free_delta > 0) {
            UINT32 freed = (UINT32)free_delta;
            UINT32 room = g_fat.TotalClusters - info->FreeClusterCount;
            info->FreeClusterCount += freed < room ? freed : room;
        }
    }
    if (next_free >= 2 && next_free < g_fat.TotalClusters + 2)
        info->NextFreeCluster = next_free;

    if (!IoBlockWriteSectors(g_fat.FsInfoSector, 1, sector))
        return FALSE;
    if (g_fat.BackupBootSector) {
        UINT32 backup = g_fat.BackupBootSector + g_fat.FsInfoSector;
        if (!IoBlockWriteSectors(backup, 1, sector))
            return FALSE;
    }
    return TRUE;
}

static BOOLEAN fat_set_fsinfo_counts(UINT32 free_count, UINT32 next_free)
{
    if (!g_fat.FsInfoSector)
        return TRUE;
    UINT8 sector[ATA_SECTOR_SIZE];
    if (!IoBlockReadSectors(g_fat.FsInfoSector, 1, sector))
        return FALSE;
    FAT_FSINFO *info = (FAT_FSINFO *)sector;
    if (info->LeadSignature != 0x41615252u ||
        info->StructSignature != 0x61417272u ||
        info->TrailSignature != 0xAA550000u)
        return TRUE;
    info->FreeClusterCount = free_count;
    info->NextFreeCluster = next_free;
    if (!IoBlockWriteSectors(g_fat.FsInfoSector, 1, sector))
        return FALSE;
    if (g_fat.BackupBootSector) {
        UINT32 backup = g_fat.BackupBootSector + g_fat.FsInfoSector;
        if (!IoBlockWriteSectors(backup, 1, sector))
            return FALSE;
    }
    return TRUE;
}

static UINT32 cluster_to_lba(UINT32 cluster)
{
    return g_fat.DataStartLba + (cluster - 2) * g_fat.SectorsPerCluster;
}

/* Read the next cluster in the chain from the FAT. */
static BOOLEAN fat_read_cluster(UINT32 cluster, UINT32 *value)
{
    UINT8 sector[ATA_SECTOR_SIZE];
    UINT32 fat_offset = cluster * 4;
    UINT32 lba = g_fat.FatStartLba + fat_offset / ATA_SECTOR_SIZE;
    UINT32 in_sector = fat_offset % ATA_SECTOR_SIZE;

    if (!IoBlockReadSectors(lba, 1, sector))
        return FALSE;
    *value = *(UINT32 *)&sector[in_sector] & 0x0FFFFFFFu;
    return TRUE;
}

static UINT32 fat_next_cluster(UINT32 cluster)
{
    UINT32 value;
    return fat_read_cluster(cluster, &value) ? value : FAT_EOC;
}

static BOOLEAN fat_set_cluster(UINT32 cluster, UINT32 value)
{
    UINT8 sector[ATA_SECTOR_SIZE];
    UINT32 offset = cluster * 4;
    UINT32 sector_index = offset / ATA_SECTOR_SIZE;
    UINT32 in_sector = offset % ATA_SECTOR_SIZE;
    for (UINT32 fat = 0; fat < g_fat.NumFats; fat++) {
        UINT32 lba = g_fat.FatStartLba + fat * g_fat.FatSectors +
                     sector_index;
        if (!IoBlockReadSectors(lba, 1, sector))
            return FALSE;
        UINT32 old = *(UINT32 *)&sector[in_sector];
        *(UINT32 *)&sector[in_sector] = (old & 0xF0000000u) |
                                        (value & 0x0FFFFFFFu);
        if (!IoBlockWriteSectors(lba, 1, sector))
            return FALSE;
    }
    return TRUE;
}

static NTSTATUS fat_allocate_cluster(UINT32 *cluster_out)
{
    UINT32 selected = 0;
    for (UINT32 cluster = 2; cluster < g_fat.TotalClusters + 2; cluster++) {
        UINT32 value;
        if (!fat_read_cluster(cluster, &value))
            return STATUS_DEVICE_NOT_READY;
        if (value == 0) {
            selected = cluster;
            break;
        }
    }
    if (!selected)
        return STATUS_DISK_FULL;

    UINT8 *zero = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                         'zClF');
    if (!zero)
        return STATUS_NO_MEMORY;
    memset(zero, 0, g_fat.BytesPerCluster);
    BOOLEAN written = IoBlockWriteSectors(cluster_to_lba(selected),
        (UINT8)g_fat.SectorsPerCluster, zero);
    ExFreePool(zero);
    if (!written || !NT_SUCCESS(FatFlush()))
        return STATUS_DEVICE_NOT_READY;

    if (!IoBlockBeginTransaction())
        return STATUS_INTERNAL_ERROR;
    if (!fat_set_cluster(selected, 0x0FFFFFFFu) ||
        !fat_update_fsinfo(-1, selected + 1) ||
        !IoBlockCommitTransaction()) {
        IoBlockRollbackTransaction();
        return STATUS_DEVICE_NOT_READY;
    }
    *cluster_out = selected;
    return STATUS_SUCCESS;
}

static BOOLEAN fat_free_chain(UINT32 cluster)
{
    while (cluster >= 2 && cluster < FAT_EOC) {
        UINT32 next = fat_next_cluster(cluster);
        if (!fat_set_cluster(cluster, 0))
            return FALSE;
        if (!fat_update_fsinfo(1, cluster))
            KeLog("[io]   warning: failed to update FAT32 FSInfo\n");
        cluster = next;
    }
    return TRUE;
}

static BOOLEAN fat_free_chain_committed(UINT32 cluster)
{
    if (!IoBlockBeginTransaction())
        return FALSE;
    if (!fat_free_chain(cluster) || !IoBlockCommitTransaction()) {
        IoBlockRollbackTransaction();
        return FALSE;
    }
    return TRUE;
}

/* Reserve free cluster numbers in memory without making them reachable on
 * disk. FAT operations are synchronous on the current uniprocessor kernel, so
 * no competing allocator can claim them before this operation publishes the
 * chain. */
static NTSTATUS fat_reserve_clusters(UINT32 count, UINT32 *clusters)
{
    UINT32 found = 0;
    for (UINT32 cluster = 2;
         cluster < g_fat.TotalClusters + 2 && found < count; cluster++) {
        UINT32 value;
        if (!fat_read_cluster(cluster, &value))
            return STATUS_DEVICE_NOT_READY;
        if (value == 0)
            clusters[found++] = cluster;
    }
    return found == count ? STATUS_SUCCESS : STATUS_DISK_FULL;
}

static void fat_rollback_chain(const UINT32 *clusters, UINT32 count)
{
    for (UINT32 i = 0; i < count; i++)
        fat_set_cluster(clusters[i], 0);
}

/* Publish a completely written set of data clusters as one FAT chain. */
static BOOLEAN fat_publish_chain(const UINT32 *clusters, UINT32 count)
{
    UINT32 published = 0;
    for (; published < count; published++) {
        UINT32 next = published + 1 < count ? clusters[published + 1]
                                            : 0x0FFFFFFFu;
        if (!fat_set_cluster(clusters[published], next))
            break;
    }
    if (published != count) {
        fat_rollback_chain(clusters, published + 1);
        return FALSE;
    }
    return TRUE;
}

/* Convert "name.ext" to the 11-byte padded 8.3 form ("NAME    EXT"). */
static void to_83(const char *name, UINT8 out[11])
{
    for (int i = 0; i < 11; i++)
        out[i] = ' ';

    int i = 0;
    while (*name && *name != '.' && i < 8) {
        char c = *name++;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        out[i++] = (UINT8)c;
    }
    while (*name && *name != '.')
        name++;
    if (*name == '.') {
        name++;
        int j = 8;
        while (*name && j < 11) {
            char c = *name++;
            if (c >= 'a' && c <= 'z')
                c -= 32;
            out[j++] = (UINT8)c;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Long file names (VFAT)                                              */
/* ------------------------------------------------------------------ */

static BOOLEAN fat_write_dirent(FAT_ENTRY_LOCATION location,
                                const FAT_DIRENT *entry);
static BOOLEAN name_is_83(const char *name);
static BOOLEAN lfn_consume(FAT_LFN_STATE *state,
                           const FAT_LFN_ENTRY *entry);
static BOOLEAN lfn_matches_short(const FAT_LFN_STATE *state,
                                 const UINT8 short83[11]);

/* The checksum stored in every LFN entry for a short name. */
static UINT8 lfn_checksum(const UINT8 name83[11])
{
    UINT8 sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (UINT8)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name83[i]);
    return sum;
}

static void lfn_reset(FAT_LFN_STATE *state)
{
    memset(state, 0, sizeof(*state));
}

static BOOLEAN utf8_decode(const char **cursor, UINT32 *codepoint)
{
    const UINT8 *s = (const UINT8 *)*cursor;
    UINT32 cp;
    UINT32 count;
    if (s[0] < 0x80) {
        cp = s[0];
        count = 1;
    } else if ((s[0] & 0xe0) == 0xc0) {
        cp = s[0] & 0x1f;
        count = 2;
        if (cp < 2) return FALSE; /* overlong */
    } else if ((s[0] & 0xf0) == 0xe0) {
        cp = s[0] & 0x0f;
        count = 3;
    } else if ((s[0] & 0xf8) == 0xf0) {
        cp = s[0] & 0x07;
        count = 4;
    } else {
        return FALSE;
    }
    for (UINT32 i = 1; i < count; i++) {
        if (!s[i] || (s[i] & 0xc0) != 0x80)
            return FALSE;
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    if ((count == 3 && cp < 0x800) || (count == 4 && cp < 0x10000) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return FALSE;
    *cursor += count;
    *codepoint = cp;
    return TRUE;
}

static BOOLEAN utf8_to_utf16(const char *text, UINT16 out[260],
                             UINT32 *units_out)
{
    UINT32 units = 0;
    while (*text) {
        UINT32 cp;
        if (!utf8_decode(&text, &cp))
            return FALSE;
        if (cp <= 0xffff) {
            if (units >= 255) return FALSE;
            out[units++] = (UINT16)cp;
        } else {
            if (units >= 254) return FALSE;
            cp -= 0x10000;
            out[units++] = (UINT16)(0xd800 + (cp >> 10));
            out[units++] = (UINT16)(0xdc00 + (cp & 0x3ff));
        }
    }
    out[units] = 0;
    *units_out = units;
    return TRUE;
}

static BOOLEAN utf16_to_utf8(const UINT16 *text, char *out, UINT32 capacity)
{
    UINT32 used = 0;
    for (UINT32 i = 0; text[i]; i++) {
        UINT32 cp = text[i];
        if (cp >= 0xd800 && cp <= 0xdbff) {
            UINT32 low = text[++i];
            if (low < 0xdc00 || low > 0xdfff)
                return FALSE;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return FALSE;
        }
        UINT32 bytes = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (used + bytes >= capacity)
            return FALSE;
        if (bytes == 1) out[used++] = (char)cp;
        else if (bytes == 2) {
            out[used++] = (char)(0xc0 | (cp >> 6));
            out[used++] = (char)(0x80 | (cp & 0x3f));
        } else if (bytes == 3) {
            out[used++] = (char)(0xe0 | (cp >> 12));
            out[used++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[used++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[used++] = (char)(0xf0 | (cp >> 18));
            out[used++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            out[used++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[used++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    out[used] = 0;
    return TRUE;
}

static UINT32 lfn_entry_count(const char *name)
{
    UINT16 wide[260];
    UINT32 units;
    return utf8_to_utf16(name, wide, &units) ? (units + 12) / 13 : 0;
}

/* Read one directory entry at an absolute location. */
static BOOLEAN fat_read_dirent(FAT_ENTRY_LOCATION location, FAT_DIRENT *entry)
{
    UINT8 *buf = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                       'dRdF');
    if (!buf)
        return FALSE;
    BOOLEAN ok = IoBlockReadSectors(cluster_to_lba(location.Cluster),
                                (UINT8)g_fat.SectorsPerCluster, buf);
    if (ok)
        *entry = ((FAT_DIRENT *)buf)[location.Index];
    ExFreePool(buf);
    return ok;
}

/* Mark one directory entry deleted. */
static BOOLEAN fat_mark_deleted(FAT_ENTRY_LOCATION location)
{
    FAT_DIRENT entry;
    if (!fat_read_dirent(location, &entry))
        return FALSE;
    entry.Name[0] = 0xE5;
    return fat_write_dirent(location, &entry);
}

/* Advance `offset` entries forward from `start`, following the directory
 * cluster chain. */
static BOOLEAN fat_advance_location(FAT_ENTRY_LOCATION start, UINT32 offset,
                                    FAT_ENTRY_LOCATION *out)
{
    UINT32 per = g_fat.BytesPerCluster / sizeof(FAT_DIRENT);
    UINT32 cluster = start.Cluster;
    UINT32 index = start.Index + offset;
    while (index >= per) {
        index -= per;
        cluster = fat_next_cluster(cluster);
        if (cluster < 2 || cluster >= FAT_EOC)
            return FALSE;
    }
    out->Cluster = cluster;
    out->Index = index;
    return TRUE;
}

/* Whether an 11-byte short name is already taken in `directory`. */
static BOOLEAN short_name_exists(UINT32 directory, const UINT8 name83[11])
{
    UINT8 *buf = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                       'sNeF');
    if (!buf)
        return TRUE; /* be conservative: pretend it is taken */
    BOOLEAN found = FALSE;
    UINT32 cluster = directory;
    while (cluster >= 2 && cluster < FAT_EOC && !found) {
        if (!IoBlockReadSectors(cluster_to_lba(cluster),
                            (UINT8)g_fat.SectorsPerCluster, buf))
            break;
        FAT_DIRENT *ents = (FAT_DIRENT *)buf;
        UINT32 count = g_fat.BytesPerCluster / sizeof(FAT_DIRENT);
        for (UINT32 i = 0; i < count; i++) {
            if (ents[i].Name[0] == 0x00)
                goto done;
            if (ents[i].Name[0] == 0xE5 || ents[i].Attr == ATTR_LONG_NAME)
                continue;
            if (memcmp(ents[i].Name, name83, 11) == 0) {
                found = TRUE;
                break;
            }
        }
        if (!found)
            cluster = fat_next_cluster(cluster);
    }
done:
    ExFreePool(buf);
    return found;
}

/* Build a unique 8.3 short name for `long_name` in `directory`. */
static void make_short_name(const char *long_name, UINT32 directory,
                            UINT8 out[11])
{
    char base[9];
    char ext[4];
    int bn = 0, en = 0;
    const char *dot = 0;
    for (const char *p = long_name; *p; p++)
        if (*p == '.')
            dot = p;

    for (const char *p = long_name; *p && p != dot && bn < 8; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '$' || c == '~' || c == '!' ||
              c == '#' || c == '%' || c == '&' || c == '(' || c == ')' ||
              c == '@' || c == '^' || c == '\'' || c == '{' || c == '}' ||
              c == '`' || c == '+'))
            c = '_';
        base[bn++] = c;
    }
    if (!bn)
        base[bn++] = '_';
    base[bn] = 0;

    if (dot) {
        for (const char *p = dot + 1; *p && en < 3; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c -= 32;
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
                c = '_';
            ext[en++] = c;
        }
    }
    ext[en] = 0;

    for (UINT32 n = 1; n < 1000000; n++) {
        char number[8];
        int nl = 0;
        UINT32 value = n;
        do {
            number[nl++] = (char)('0' + value % 10);
            value /= 10;
        } while (value);
        for (int i = 0; i < nl / 2; i++) {
            char t = number[i];
            number[i] = number[nl - 1 - i];
            number[nl - 1 - i] = t;
        }

        UINT8 candidate[11];
        memset(candidate, ' ', 11);
        int keep = 8 - 1 - nl;
        if (keep < 1)
            keep = 1;
        int cn = 0;
        for (int i = 0; i < bn && cn < keep; i++)
            candidate[cn++] = (UINT8)base[i];
        candidate[cn++] = '~';
        for (int i = 0; i < nl && cn < 8; i++)
            candidate[cn++] = (UINT8)number[i];
        for (int i = 0; i < en; i++)
            candidate[8 + i] = (UINT8)ext[i];

        if (!short_name_exists(directory, candidate)) {
            memcpy(out, candidate, 11);
            return;
        }
    }
    /* Fallback: a truncated, non-unique name. */
    memset(out, ' ', 11);
    for (int i = 0; i < bn && i < 8; i++)
        out[i] = (UINT8)base[i];
    for (int i = 0; i < en; i++)
        out[8 + i] = (UINT8)ext[i];
}

/* Find `count` consecutive free directory slots, extending the cluster chain
 * (with zeroed clusters) as needed. */
static BOOLEAN fat_find_free_run(UINT32 directory, UINT32 count,
                                 FAT_ENTRY_LOCATION *location)
{
    if (!count)
        return FALSE;
    UINT8 *buf = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                       'rFrF');
    if (!buf)
        return FALSE;
    UINT32 per = g_fat.BytesPerCluster / sizeof(FAT_DIRENT);

    for (UINT32 attempt = 0; attempt < 8; attempt++) {
        UINT32 cluster = directory;
        UINT32 run = 0, start_cluster = 0, start_index = 0;
        BOOLEAN saw_end = FALSE;
        for (;;) {
            if (!IoBlockReadSectors(cluster_to_lba(cluster),
                                (UINT8)g_fat.SectorsPerCluster, buf)) {
                ExFreePool(buf);
                return FALSE;
            }
            FAT_DIRENT *ents = (FAT_DIRENT *)buf;
            for (UINT32 i = 0; i < per; i++) {
                if (ents[i].Name[0] == 0x00)
                    saw_end = TRUE;
                BOOLEAN free = saw_end || ents[i].Name[0] == 0xE5;
                if (free) {
                    if (!run) {
                        start_cluster = cluster;
                        start_index = i;
                    }
                    if (++run == count) {
                        location->Cluster = start_cluster;
                        location->Index = start_index;
                        ExFreePool(buf);
                        return TRUE;
                    }
                } else {
                    run = 0;
                }
            }
            UINT32 next = fat_next_cluster(cluster);
            if (next >= 2 && next < FAT_EOC) {
                cluster = next;
                continue;
            }
            break; /* end of chain */
        }

        /* Extend the directory by one zeroed cluster and rescan. */
        UINT32 extension;
        if (!NT_SUCCESS(fat_allocate_cluster(&extension)))
            break;
        if (!IoBlockBeginTransaction() ||
            !fat_set_cluster(cluster, extension) ||
            !IoBlockCommitTransaction()) {
            IoBlockRollbackTransaction();
            if (IoBlockBeginTransaction()) {
                fat_set_cluster(extension, 0);
                fat_update_fsinfo(1, extension);
                IoBlockCommitTransaction();
            }
            break;
        }
    }
    ExFreePool(buf);
    return FALSE;
}

/* Write the run of long-name entries that precede a short entry at `start`. */
static BOOLEAN fat_write_lfn(FAT_ENTRY_LOCATION start, const char *long_name,
                             const UINT8 short83[11])
{
    UINT16 wide_name[260];
    UINT32 len;
    if (!utf8_to_utf16(long_name, wide_name, &len))
        return FALSE;
    UINT32 count = (len + 12) / 13;
    if (!count || count > 20)
        return FALSE;
    UINT8 checksum = lfn_checksum(short83);

    for (UINT32 ordinal = count; ordinal >= 1; ordinal--) {
        UINT32 offset = count - ordinal; /* highest ordinal is written first */
        FAT_LFN_ENTRY entry;
        memset(&entry, 0, sizeof(entry));
        entry.Order = (UINT8)ordinal;
        if (ordinal == count)
            entry.Order |= 0x40;
        entry.Attr = ATTR_LONG_NAME;
        entry.Checksum = checksum;

        UINT32 base = (ordinal - 1) * 13;
        UINT16 part[13];
        for (UINT32 i = 0; i < 13; i++) {
            UINT32 ci = base + i;
            if (ci < len)
                part[i] = wide_name[ci];
            else if (ci == len)
                part[i] = 0x0000;
            else
                part[i] = 0xFFFF;
        }
        for (int i = 0; i < 5; i++) entry.Name1[i] = part[i];
        for (int i = 0; i < 6; i++) entry.Name2[i] = part[5 + i];
        for (int i = 0; i < 2; i++) entry.Name3[i] = part[11 + i];

        FAT_ENTRY_LOCATION location;
        if (!fat_advance_location(start, offset, &location) ||
            !fat_write_dirent(location, (FAT_DIRENT *)&entry)) {
            /* A short entry is never published after a partial LFN run. Mark
             * every fragment already written deleted so a later create can
             * safely reuse the slots. */
            for (UINT32 rollback = 0; rollback < offset; rollback++) {
                FAT_ENTRY_LOCATION old;
                if (fat_advance_location(start, rollback, &old))
                    fat_mark_deleted(old);
            }
            return FALSE;
        }
    }
    return TRUE;
}

/* Mark a short entry and the run of long-name entries immediately preceding it
 * deleted. */
static BOOLEAN fat_erase_with_lfn(UINT32 directory, FAT_ENTRY_LOCATION target)
{
    UINT8 *buf = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                       'eLfF');
    if (!buf)
        return FALSE;
    UINT32 per = g_fat.BytesPerCluster / sizeof(FAT_DIRENT);
    FAT_ENTRY_LOCATION lfn[21];
    UINT32 lfn_count = 0;
    FAT_LFN_STATE lfn_state;
    lfn_reset(&lfn_state);
    UINT32 cluster = directory;
    BOOLEAN ok = FALSE;

    while (cluster >= 2 && cluster < FAT_EOC) {
        if (!IoBlockReadSectors(cluster_to_lba(cluster),
                            (UINT8)g_fat.SectorsPerCluster, buf))
            break;
        FAT_DIRENT *ents = (FAT_DIRENT *)buf;
        for (UINT32 i = 0; i < per; i++) {
            if (ents[i].Name[0] == 0x00) {
                ExFreePool(buf);
                return FALSE;
            }
            if (ents[i].Attr == ATTR_LONG_NAME && ents[i].Name[0] != 0xE5) {
                FAT_LFN_ENTRY *entry = (FAT_LFN_ENTRY *)&ents[i];
                if (entry->Order & 0x40)
                    lfn_count = 0;
                if (lfn_consume(&lfn_state, entry) && lfn_count < 20) {
                    lfn[lfn_count].Cluster = cluster;
                    lfn[lfn_count].Index = i;
                    lfn_count++;
                } else {
                    lfn_count = 0;
                }
                continue;
            }
            if (ents[i].Name[0] == 0xE5) {
                lfn_count = 0;
                lfn_reset(&lfn_state);
                continue;
            }
            if (cluster == target.Cluster && i == target.Index) {
                ok = TRUE;
                if (lfn_matches_short(&lfn_state, ents[i].Name)) {
                    for (UINT32 k = 0; k < lfn_count && ok; k++)
                        ok = fat_mark_deleted(lfn[k]);
                }
                if (ok)
                    ok = fat_mark_deleted(target);
                ExFreePool(buf);
                return ok;
            }
            lfn_count = 0;
            lfn_reset(&lfn_state);
        }
        cluster = fat_next_cluster(cluster);
    }
    ExFreePool(buf);
    return FALSE;
}

/* Store a short entry, writing its long-name entries first when `long_name` is
 * not already a valid 8.3 name. `start` must point at a run of
 * lfn_count(long_name) + 1 free slots. */
static BOOLEAN fat_write_named_entry(UINT32 directory,
                                     FAT_ENTRY_LOCATION start,
                                     const char *long_name,
                                     const FAT_DIRENT *short_entry)
{
    if (name_is_83(long_name))
        return fat_write_dirent(start, short_entry);

    UINT8 short83[11];
    memcpy(short83, short_entry->Name, 11);
    if (!fat_write_lfn(start, long_name, short83))
        return FALSE;
    UINT32 count = lfn_entry_count(long_name);
    if (!count)
        return FALSE;
    FAT_ENTRY_LOCATION short_loc;
    if (!fat_advance_location(start, count, &short_loc))
        return FALSE;
    if (fat_write_dirent(short_loc, short_entry))
        return TRUE;
    for (UINT32 rollback = 0; rollback < count; rollback++) {
        FAT_ENTRY_LOCATION old;
        if (fat_advance_location(start, rollback, &old))
            fat_mark_deleted(old);
    }
    return FALSE;
}

static BOOLEAN fat_cluster_valid(UINT32 cluster)
{
    return cluster >= 2 && cluster < g_fat.TotalClusters + 2;
}

static BOOLEAN fat_bitmap_test(const UINT8 *bitmap, UINT32 cluster)
{
    return (bitmap[cluster >> 3] & (UINT8)(1u << (cluster & 7))) != 0;
}

static void fat_bitmap_set(UINT8 *bitmap, UINT32 cluster)
{
    bitmap[cluster >> 3] |= (UINT8)(1u << (cluster & 7));
}

static BOOLEAN fat_recovery_mark_chain(UINT32 first, UINT8 *reachable)
{
    UINT32 cluster = first;
    while (fat_cluster_valid(cluster)) {
        if (fat_bitmap_test(reachable, cluster))
            return TRUE;
        fat_bitmap_set(reachable, cluster);
        UINT32 next;
        if (!fat_read_cluster(cluster, &next))
            return FALSE;
        if (next >= FAT_EOC)
            return TRUE;
        if (!fat_cluster_valid(next))
            return FALSE;
        cluster = next;
    }
    return TRUE;
}

static BOOLEAN fat_recovery_scan_directory(UINT32 first, UINT8 *reachable,
                                             UINT32 *queue, UINT32 *tail,
                                             UINT32 capacity, UINT8 *buffer)
{
    UINT32 cluster = first;
    BOOLEAN entries_finished = FALSE;
    while (fat_cluster_valid(cluster)) {
        if (fat_bitmap_test(reachable, cluster))
            return TRUE;
        fat_bitmap_set(reachable, cluster);
        if (!IoBlockReadSectors(cluster_to_lba(cluster),
                            (UINT8)g_fat.SectorsPerCluster, buffer))
            return FALSE;

        if (!entries_finished) {
            FAT_DIRENT *entries = (FAT_DIRENT *)buffer;
            UINT32 count = g_fat.BytesPerCluster / sizeof(FAT_DIRENT);
            for (UINT32 i = 0; i < count; i++) {
                FAT_DIRENT *entry = &entries[i];
                if (entry->Name[0] == 0x00) {
                    entries_finished = TRUE;
                    break;
                }
                if (entry->Name[0] == 0xE5 ||
                    entry->Attr == ATTR_LONG_NAME || (entry->Attr & 0x08))
                    continue;
                if (entry->Name[0] == '.' &&
                    (entry->Name[1] == ' ' || entry->Name[1] == '.'))
                    continue;
                UINT32 child = ((UINT32)entry->FirstClusterHigh << 16) |
                               entry->FirstClusterLow;
                child &= 0x0FFFFFFFu;
                if (!fat_cluster_valid(child))
                    continue; /* Empty files legitimately use cluster zero. */
                if (entry->Attr & 0x10) {
                    if (*tail == capacity)
                        return FALSE;
                    queue[(*tail)++] = child;
                } else if (!fat_recovery_mark_chain(child, reachable)) {
                    return FALSE;
                }
            }
        }

        UINT32 next;
        if (!fat_read_cluster(cluster, &next))
            return FALSE;
        if (next >= FAT_EOC)
            return TRUE;
        if (!fat_cluster_valid(next))
            return FALSE;
        cluster = next;
    }
    return TRUE;
}

/* Reclaim allocations which cannot be reached from the root directory. This
 * repairs the safe failure mode used by ordered FAT updates: a crash may leak
 * a newly allocated chain, but never publishes a directory entry pointing at
 * incomplete data. */
static NTSTATUS fat_recover_orphans(void)
{
    UINT32 limit = g_fat.TotalClusters + 2;
    SIZE_T bitmap_bytes = (limit + 7) / 8;
    UINT8 *reachable = ExAllocatePoolWithTag(NonPagedPool, bitmap_bytes,
                                              'bRcF');
    UINT32 *queue = ExAllocatePoolWithTag(NonPagedPool,
                                           g_fat.TotalClusters * sizeof(UINT32),
                                           'qRcF');
    UINT8 *buffer = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                           'dRcF');
    if (!reachable || !queue || !buffer) {
        if (reachable) ExFreePool(reachable);
        if (queue) ExFreePool(queue);
        if (buffer) ExFreePool(buffer);
        return STATUS_NO_MEMORY;
    }
    memset(reachable, 0, bitmap_bytes);
    UINT32 head = 0, tail = 1;
    queue[0] = g_fat.RootCluster;
    BOOLEAN ok = TRUE;
    while (head < tail && ok) {
        UINT32 directory = queue[head++];
        if (!fat_bitmap_test(reachable, directory))
            ok = fat_recovery_scan_directory(directory, reachable, queue,
                                               &tail, g_fat.TotalClusters,
                                               buffer);
    }
    if (!ok) {
        ExFreePool(buffer);
        ExFreePool(queue);
        ExFreePool(reachable);
        return STATUS_DEVICE_NOT_READY;
    }

    UINT32 reclaimed = 0, batch = 0;
    for (UINT32 cluster = 2; cluster < limit; cluster++) {
        UINT32 value;
        if (!fat_read_cluster(cluster, &value)) {
            ok = FALSE;
            break;
        }
        BOOLEAN reserved = value >= 0x0FFFFFF0u && value < FAT_EOC;
        if (!value || value == FAT_BAD || reserved ||
            fat_bitmap_test(reachable, cluster))
            continue;
        if (!batch && !IoBlockBeginTransaction()) {
            ok = FALSE;
            break;
        }
        if (!fat_set_cluster(cluster, 0)) {
            ok = FALSE;
            break;
        }
        reclaimed++;
        if (++batch == 16) {
            if (!IoBlockCommitTransaction()) {
                ok = FALSE;
                break;
            }
            batch = 0;
        }
    }
    if (ok && batch)
        ok = IoBlockCommitTransaction();
    if (!ok && batch)
        IoBlockRollbackTransaction();

    UINT32 free_count = 0, next_free = 0xffffffffu;
    if (ok) {
        for (UINT32 cluster = 2; cluster < limit; cluster++) {
            UINT32 value;
            if (!fat_read_cluster(cluster, &value)) {
                ok = FALSE;
                break;
            }
            if (!value) {
                free_count++;
                if (next_free == 0xffffffffu)
                    next_free = cluster;
            }
        }
    }
    if (ok) {
        if (!IoBlockBeginTransaction() ||
            !fat_set_fsinfo_counts(free_count, next_free) ||
            !IoBlockCommitTransaction()) {
            IoBlockRollbackTransaction();
            ok = FALSE;
        }
    }
    if (reclaimed)
        KeLog("[io]   fat32 recovery: reclaimed %u orphan cluster(s)\n",
              (unsigned)reclaimed);
    ExFreePool(buffer);
    ExFreePool(queue);
    ExFreePool(reachable);
    return ok ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

NTSTATUS FatMount(void)
{
    UINT8 sector[ATA_SECTOR_SIZE];
    if (!IoBlockReadSectors(0, 1, sector))
        return STATUS_DEVICE_NOT_READY;

    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return STATUS_UNSUCCESSFUL; /* no boot signature */

    const FAT_BPB *bpb = (const FAT_BPB *)sector;
    if (bpb->BytesPerSector != ATA_SECTOR_SIZE || bpb->FatSize16 != 0)
        return STATUS_NOT_SUPPORTED; /* expect a 512-byte-sector FAT32 volume */

    g_fat.SectorsPerCluster = bpb->SectorsPerCluster;
    g_fat.BytesPerCluster = bpb->SectorsPerCluster * ATA_SECTOR_SIZE;
    g_fat.FatStartLba = bpb->ReservedSectors;
    g_fat.FatSectors = bpb->FatSize32;
    g_fat.NumFats = bpb->NumFats;
    g_fat.DataStartLba = bpb->ReservedSectors + bpb->NumFats * bpb->FatSize32;
    UINT32 total_sectors = bpb->TotalSectors16 ? bpb->TotalSectors16
                                                : bpb->TotalSectors32;
    g_fat.TotalClusters =
        (total_sectors - g_fat.DataStartLba) / bpb->SectorsPerCluster;
    g_fat.RootCluster = bpb->RootCluster;
    g_fat.FsInfoSector = bpb->FsInfoSector;
    g_fat.BackupBootSector = bpb->BackupBootSector;
    g_fat.Mounted = TRUE;

    NTSTATUS recovery_status = fat_recover_orphans();
    if (!NT_SUCCESS(recovery_status)) {
        g_fat.Mounted = FALSE;
        return recovery_status;
    }

    KeLog("[io]   fat32 mounted: %u sec/cluster, fat@%u data@%u root=cluster %u\n",
          (unsigned)g_fat.SectorsPerCluster, (unsigned)g_fat.FatStartLba,
          (unsigned)g_fat.DataStartLba, (unsigned)g_fat.RootCluster);
    return STATUS_SUCCESS;
}

NTSTATUS FatFlush(void)
{
    if (!g_fat.Mounted)
        return STATUS_DEVICE_NOT_READY;
    return IoBlockFlush() ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

static void lfn_put_fragment(UINT16 out[260], const FAT_LFN_ENTRY *lfn)
{
    UINT32 ordinal = lfn->Order & 0x1f;
    if (!ordinal || ordinal > 20)
        return;
    UINT32 pos = (ordinal - 1) * 13;
    UINT16 chars[13];
    for (int i = 0; i < 5; i++) chars[i] = lfn->Name1[i];
    for (int i = 0; i < 6; i++) chars[5 + i] = lfn->Name2[i];
    for (int i = 0; i < 2; i++) chars[11 + i] = lfn->Name3[i];
    for (int i = 0; i < 13 && pos + (UINT32)i < 259; i++) {
        UINT16 c = chars[i];
        if (c == 0x0000) {
            out[pos + i] = 0;
            break;
        }
        if (c != 0xffff)
            out[pos + i] = c;
    }
}

/* Consume one on-disk LFN entry. Entries must appear as N|LAST, N-1, ... 1
 * and all carry the checksum of the following short entry. A malformed run is
 * ignored instead of being accidentally attached to the next file. */
static BOOLEAN lfn_consume(FAT_LFN_STATE *state, const FAT_LFN_ENTRY *lfn)
{
    UINT8 ordinal = lfn->Order & 0x1f;
    BOOLEAN last = (lfn->Order & 0x40) != 0;
    if (!ordinal || ordinal > 20 || lfn->Type != 0 ||
        lfn->FirstClusterLow != 0) {
        lfn_reset(state);
        return FALSE;
    }
    if (last) {
        lfn_reset(state);
        state->Valid = TRUE;
        state->Checksum = lfn->Checksum;
        state->NextOrdinal = ordinal;
    }
    if (!state->Valid || state->NextOrdinal != ordinal ||
        state->Checksum != lfn->Checksum) {
        lfn_reset(state);
        return FALSE;
    }
    lfn_put_fragment(state->Name, lfn);
    state->NextOrdinal--;
    return TRUE;
}

static BOOLEAN lfn_matches_short(const FAT_LFN_STATE *state,
                                 const UINT8 short83[11])
{
    return state->Valid && state->NextOrdinal == 0 && state->Name[0] &&
           state->Checksum == lfn_checksum(short83);
}

static BOOLEAN lfn_name_equal(const FAT_LFN_STATE *state, const char *name)
{
    UINT16 wide[260];
    UINT32 units;
    if (!utf8_to_utf16(name, wide, &units))
        return FALSE;
    for (UINT32 i = 0; i <= units; i++) {
        UINT16 left = state->Name[i], right = wide[i];
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right)
            return FALSE;
        if (!left)
            return TRUE;
    }
    return FALSE;
}

/*
 * Whether `name` can be represented as a FAT 8.3 short name at all: at most one
 * dot, a base of 1..8 characters and an extension of at most 3.
 *
 * This matters because to_83 truncates rather than failing. "notepad.exe.mui"
 * collapses to "NOTEPAD EXE" -- byte for byte the short name of notepad.exe --
 * so a lookup for the .mui resource module happily returned the executable
 * instead, and the loader then ran a second copy of the program as if it were a
 * library. A name that cannot be 8.3 has to be matched against long names only.
 */
static BOOLEAN name_is_83(const char *name)
{
    UINT32 base = 0, ext = 0;
    BOOLEAN in_ext = FALSE;
    for (UINT32 i = 0; name[i]; i++) {
        if (name[i] == '.') {
            if (in_ext)
                return FALSE; /* a second dot */
            in_ext = TRUE;
            continue;
        }
        unsigned char c = (unsigned char)name[i];
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '*' || c == '+' ||
            c == ',' || c == '/' || c == ':' || c == ';' || c == '<' ||
            c == '=' || c == '>' || c == '?' || c == '[' || c == '\\' ||
            c == ']' || c == '|')
            return FALSE;
        if (in_ext) {
            if (++ext > 3)
                return FALSE;
        } else {
            if (++base > 8)
                return FALSE;
        }
    }
    return base != 0 && (!in_ext || ext != 0);
}

static BOOLEAN fat_find(UINT32 dir_cluster, const char *name,
                        FAT_DIRENT *out_entry,
                        FAT_ENTRY_LOCATION *out_location)
{
    UINT8 *cluster_buf = ExAllocatePoolWithTag(NonPagedPool,
                                               g_fat.BytesPerCluster, 'taF');
    if (!cluster_buf)
        return FALSE;

    BOOLEAN found = FALSE;
    UINT8 name83[11];
    to_83(name, name83);
    BOOLEAN short_name_usable = name_is_83(name);
    FAT_LFN_STATE lfn_state;
    lfn_reset(&lfn_state);
    UINT32 cluster = dir_cluster;
    while (cluster < FAT_EOC && !found) {
        if (!IoBlockReadSectors(cluster_to_lba(cluster),
                            (UINT8)g_fat.SectorsPerCluster, cluster_buf))
            break;

        FAT_DIRENT *ents = (FAT_DIRENT *)cluster_buf;
        UINT32 n = g_fat.BytesPerCluster / sizeof(FAT_DIRENT);
        for (UINT32 i = 0; i < n; i++) {
            FAT_DIRENT *e = &ents[i];
            if (e->Name[0] == 0x00)
                goto done; /* end of directory */
            if (e->Name[0] == 0xE5) {
                lfn_reset(&lfn_state);
                continue;
            }
            if (e->Attr == ATTR_LONG_NAME) {
                lfn_consume(&lfn_state, (FAT_LFN_ENTRY *)e);
                continue;
            }
            BOOLEAN valid_lfn = lfn_matches_short(&lfn_state, e->Name);
            if ((valid_lfn && lfn_name_equal(&lfn_state, name)) ||
                (short_name_usable && memcmp(e->Name, name83, 11) == 0)) {
                *out_entry = *e;
                if (out_location) {
                    out_location->Cluster = cluster;
                    out_location->Index = i;
                }
                found = TRUE;
                break;
            }
            lfn_reset(&lfn_state);
        }
        cluster = fat_next_cluster(cluster);
    }
done:
    ExFreePool(cluster_buf);
    return found;
}

static NTSTATUS fat_resolve_parent(const char *path, UINT32 *directory,
                                   char leaf[260])
{
    if (!path || !*path)
        return STATUS_INVALID_PARAMETER;
    UINT32 current = g_fat.RootCluster;
    UINT32 pos = 0;
    if (path[0] && path[1] == ':')
        pos = 2;
    while (path[pos] == '\\' || path[pos] == '/') pos++;

    for (;;) {
        char component[260];
        UINT32 n = 0;
        while (path[pos] && path[pos] != '\\' && path[pos] != '/') {
            if (n + 1 >= sizeof(component))
                return STATUS_INVALID_PARAMETER;
            component[n++] = path[pos++];
        }
        component[n] = 0;
        while (path[pos] == '\\' || path[pos] == '/') pos++;
        if (!path[pos]) {
            if (!n)
                return STATUS_INVALID_PARAMETER;
            memcpy(leaf, component, n + 1);
            *directory = current;
            return STATUS_SUCCESS;
        }
        if (!n || (component[0] == '.' && component[1] == 0))
            continue;
        FAT_DIRENT entry;
        if (!fat_find(current, component, &entry, NULL) ||
            !(entry.Attr & 0x10))
            return STATUS_OBJECT_PATH_NOT_FOUND;
        current = ((UINT32)entry.FirstClusterHigh << 16) |
                  entry.FirstClusterLow;
        if (current < 2)
            return STATUS_OBJECT_PATH_NOT_FOUND;
    }
}

static void from_83(const UINT8 in[11], char out[13])
{
    int n = 0;
    for (int i = 0; i < 8 && in[i] != ' '; i++)
        out[n++] = (char)in[i];
    if (in[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && in[i] != ' '; i++)
            out[n++] = (char)in[i];
    }
    out[n] = 0;
}

NTSTATUS FatEnumerateDirectory(const char *path, UINT32 wanted,
                               FAT_FIND_DATA *out)
{
    if (!g_fat.Mounted || !out)
        return STATUS_DEVICE_NOT_READY;

    UINT8 *buf = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                       'neF');
    if (!buf)
        return STATUS_NO_MEMORY;

    UINT32 directory = g_fat.RootCluster;
    UINT32 path_pos = (path && path[0] && path[1] == ':') ? 2 : 0;
    while (path && (path[path_pos] == '\\' || path[path_pos] == '/')) path_pos++;
    if (path && path[path_pos]) {
        UINT32 parent; char leaf[260]; FAT_DIRENT dirent;
        NTSTATUS status = fat_resolve_parent(path, &parent, leaf);
        if (!NT_SUCCESS(status) || !fat_find(parent, leaf, &dirent, NULL) ||
            !(dirent.Attr & 0x10)) return STATUS_OBJECT_PATH_NOT_FOUND;
        directory = ((UINT32)dirent.FirstClusterHigh << 16) |
                    dirent.FirstClusterLow;
    }

    UINT32 seen = 0;
    FAT_LFN_STATE lfn_state;
    lfn_reset(&lfn_state);
    UINT32 cluster = directory;
    while (cluster < FAT_EOC) {
        if (!IoBlockReadSectors(cluster_to_lba(cluster),
                            (UINT8)g_fat.SectorsPerCluster, buf)) {
            ExFreePool(buf);
            return STATUS_DEVICE_NOT_READY;
        }
        FAT_DIRENT *entries = (FAT_DIRENT *)buf;
        UINT32 count = g_fat.BytesPerCluster / sizeof(FAT_DIRENT);
        for (UINT32 i = 0; i < count; i++) {
            FAT_DIRENT *e = &entries[i];
            if (e->Name[0] == 0x00) {
                ExFreePool(buf);
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            if (e->Name[0] == 0xE5) {
                lfn_reset(&lfn_state);
                continue;
            }
            if (e->Attr == ATTR_LONG_NAME) {
                lfn_consume(&lfn_state, (FAT_LFN_ENTRY *)e);
                continue;
            }
            if (e->Attr & 0x08) { /* volume label */
                lfn_reset(&lfn_state);
                continue;
            }
            BOOLEAN valid_lfn = lfn_matches_short(&lfn_state, e->Name);
            if (seen++ != wanted)
                goto next_entry;
            if (valid_lfn) {
                if (!utf16_to_utf8(lfn_state.Name, out->Name,
                                   sizeof(out->Name))) {
                    ExFreePool(buf);
                    return STATUS_BUFFER_TOO_SMALL;
                }
            } else {
                from_83(e->Name, out->Name);
            }
            out->Size = e->FileSize;
            out->Attributes = e->Attr;
            out->WriteDate = e->WriteDate;
            out->WriteTime = e->WriteTime;
            ExFreePool(buf);
            return STATUS_SUCCESS;
next_entry:
            lfn_reset(&lfn_state);
        }
        cluster = fat_next_cluster(cluster);
    }
    ExFreePool(buf);
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS FatEnumerateRoot(UINT32 wanted, FAT_FIND_DATA *out)
{
    return FatEnumerateDirectory("C:\\", wanted, out);
}

NTSTATUS FatLoadFile(const char *name, void **out_buffer, SIZE_T *out_size,
                     FAT_FIND_DATA *out_info)
{
    if (!g_fat.Mounted)
        return STATUS_DEVICE_NOT_READY;

    UINT32 directory;
    char leaf[260];
    NTSTATUS resolved = fat_resolve_parent(name, &directory, leaf);
    if (!NT_SUCCESS(resolved))
        return resolved;
    FAT_DIRENT entry;
    if (!fat_find(directory, leaf, &entry, NULL) || (entry.Attr & 0x10))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    UINT32 cluster = ((UINT32)entry.FirstClusterHigh << 16) |
                     entry.FirstClusterLow;
    UINT32 size = entry.FileSize;

    void *buffer = ExAllocatePoolWithTag(NonPagedPool, size ? size : 1, 'liF');
    if (!buffer)
        return STATUS_NO_MEMORY;

    UINT8 *chunk = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                         'taF');
    if (!chunk) {
        ExFreePool(buffer);
        return STATUS_NO_MEMORY;
    }

    SIZE_T pos = 0, remaining = size;
    while (cluster < FAT_EOC && remaining > 0) {
        if (!IoBlockReadSectors(cluster_to_lba(cluster),
                            (UINT8)g_fat.SectorsPerCluster, chunk)) {
            ExFreePool(chunk);
            ExFreePool(buffer);
            return STATUS_DEVICE_NOT_READY;
        }
        SIZE_T n = remaining < g_fat.BytesPerCluster ? remaining
                                                     : g_fat.BytesPerCluster;
        memcpy((UINT8 *)buffer + pos, chunk, n);
        pos += n;
        remaining -= n;
        cluster = fat_next_cluster(cluster);
    }

    ExFreePool(chunk);
    *out_buffer = buffer;
    *out_size = size;
    if (out_info) {
        UINT32 i = 0;
        for (; name[i] && i < sizeof(out_info->Name) - 1; i++)
            out_info->Name[i] = name[i];
        out_info->Name[i] = 0;
        out_info->Size = entry.FileSize;
        out_info->Attributes = entry.Attr;
        out_info->WriteDate = entry.WriteDate;
        out_info->WriteTime = entry.WriteTime;
    }
    KeLog("[io]   loaded '%s' (%lu bytes) from disk\n", name, (unsigned long)size);
    return STATUS_SUCCESS;
}

NTSTATUS FatQueryPath(const char *name, FAT_FIND_DATA *out_info)
{
    if (!g_fat.Mounted || !name || !out_info)
        return g_fat.Mounted ? STATUS_INVALID_PARAMETER : STATUS_DEVICE_NOT_READY;
    UINT32 pos = (name[0] && name[1] == ':') ? 2 : 0;
    while (name[pos] == '\\' || name[pos] == '/') pos++;
    if (!name[pos]) {
        memset(out_info, 0, sizeof(*out_info));
        out_info->Name[0] = '\\';
        out_info->Name[1] = 0;
        out_info->Attributes = 0x10;
        return STATUS_SUCCESS;
    }
    UINT32 directory;
    char leaf[260];
    NTSTATUS status = fat_resolve_parent(name, &directory, leaf);
    if (!NT_SUCCESS(status)) return status;
    FAT_DIRENT entry;
    if (!fat_find(directory, leaf, &entry, NULL))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    memset(out_info, 0, sizeof(*out_info));
    UINT32 i = 0;
    for (; leaf[i] && i + 1 < sizeof(out_info->Name); i++)
        out_info->Name[i] = leaf[i];
    out_info->Name[i] = 0;
    out_info->Size = entry.FileSize;
    out_info->Attributes = entry.Attr;
    out_info->WriteDate = entry.WriteDate;
    out_info->WriteTime = entry.WriteTime;
    return STATUS_SUCCESS;
}

static BOOLEAN fat_write_dirent(FAT_ENTRY_LOCATION location,
                                const FAT_DIRENT *entry)
{
    UINT8 *buf = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                       'dWrF');
    if (!buf)
        return FALSE;
    BOOLEAN ok = IoBlockReadSectors(cluster_to_lba(location.Cluster),
        (UINT8)g_fat.SectorsPerCluster, buf);
    if (ok) {
        ((FAT_DIRENT *)buf)[location.Index] = *entry;
        ok = IoBlockWriteSectors(cluster_to_lba(location.Cluster),
            (UINT8)g_fat.SectorsPerCluster, buf);
    }
    ExFreePool(buf);
    return ok;
}

static BOOLEAN fat_find_free_slot(UINT32 directory,
                                  FAT_ENTRY_LOCATION *location)
{
    UINT8 *buf = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                       'dFrF');
    if (!buf)
        return FALSE;
    UINT32 cluster = directory;
    UINT32 previous = 0;
    while (cluster >= 2 && cluster < FAT_EOC) {
        if (!IoBlockReadSectors(cluster_to_lba(cluster),
                            (UINT8)g_fat.SectorsPerCluster, buf)) {
            ExFreePool(buf);
            return FALSE;
        }
        FAT_DIRENT *entries = (FAT_DIRENT *)buf;
        UINT32 count = g_fat.BytesPerCluster / sizeof(FAT_DIRENT);
        for (UINT32 i = 0; i < count; i++) {
            if (entries[i].Name[0] == 0x00 || entries[i].Name[0] == 0xE5) {
                location->Cluster = cluster;
                location->Index = i;
                ExFreePool(buf);
                return TRUE;
            }
        }
        previous = cluster;
        cluster = fat_next_cluster(cluster);
    }

    UINT32 extension;
    if (!NT_SUCCESS(fat_allocate_cluster(&extension))) {
        ExFreePool(buf);
        return FALSE;
    }
    if (!IoBlockBeginTransaction() ||
        !fat_set_cluster(previous, extension) ||
        !IoBlockCommitTransaction()) {
        IoBlockRollbackTransaction();
        if (IoBlockBeginTransaction()) {
            fat_set_cluster(extension, 0);
            fat_update_fsinfo(1, extension);
            IoBlockCommitTransaction();
        }
        ExFreePool(buf);
        return FALSE;
    }
    location->Cluster = extension;
    location->Index = 0;
    ExFreePool(buf);
    return TRUE;
}

NTSTATUS FatStoreFile(const char *name, const void *buffer, SIZE_T size,
                      BOOLEAN create_if_missing)
{
    if (!g_fat.Mounted)
        return STATUS_DEVICE_NOT_READY;
    if (!name || size > 0xffffffffu)
        return STATUS_INVALID_PARAMETER;

    UINT32 directory;
    char leaf[260];
    NTSTATUS resolved = fat_resolve_parent(name, &directory, leaf);
    if (!NT_SUCCESS(resolved))
        return resolved;

    FAT_DIRENT entry;
    FAT_ENTRY_LOCATION location;
    BOOLEAN exists = fat_find(directory, leaf, &entry, &location);
    UINT8 short83[11];
    if (exists) {
        memcpy(short83, entry.Name, 11);
    } else {
        if (!create_if_missing)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        if (name_is_83(leaf)) {
            to_83(leaf, short83);
            if (!fat_find_free_slot(directory, &location))
                return STATUS_INSUFFICIENT_RESOURCES;
        } else {
            make_short_name(leaf, directory, short83);
            UINT32 count = lfn_entry_count(leaf);
            if (!count)
                return STATUS_OBJECT_NAME_INVALID;
            if (!fat_find_free_run(directory, count + 1, &location))
                return STATUS_INSUFFICIENT_RESOURCES;
        }
        memset(&entry, 0, sizeof(entry));
        memcpy(entry.Name, short83, 11);
        entry.Attr = 0x20; /* archive */
    }

    FAT_DIRENT old_entry = entry;
    UINT32 old_first = ((UINT32)entry.FirstClusterHigh << 16) |
                       entry.FirstClusterLow;
    UINT32 cluster_count = size
        ? (UINT32)((size + g_fat.BytesPerCluster - 1) /
                   g_fat.BytesPerCluster) : 0;
    UINT32 *clusters = NULL;
    UINT8 *cluster_buf = NULL;
    if (cluster_count) {
        clusters = ExAllocatePoolWithTag(NonPagedPool,
                                          cluster_count * sizeof(UINT32),
                                          'cRsF');
        cluster_buf = ExAllocatePoolWithTag(NonPagedPool,
                                             g_fat.BytesPerCluster, 'wFlF');
        if (!clusters || !cluster_buf) {
            if (clusters) ExFreePool(clusters);
            if (cluster_buf) ExFreePool(cluster_buf);
            return STATUS_NO_MEMORY;
        }
        NTSTATUS reserve_status = fat_reserve_clusters(cluster_count,
                                                        clusters);
        if (!NT_SUCCESS(reserve_status)) {
            ExFreePool(cluster_buf);
            ExFreePool(clusters);
            return reserve_status;
        }
    }

    /* Phase 1: write every new data cluster while it is still marked free.
     * A crash here can leave stale bytes only; no directory or FAT entry can
     * expose an incomplete file. */
    SIZE_T remaining = size;
    const UINT8 *source = (const UINT8 *)buffer;
    for (UINT32 i = 0; i < cluster_count; i++) {
        SIZE_T chunk = remaining < g_fat.BytesPerCluster
                         ? remaining : g_fat.BytesPerCluster;
        memset(cluster_buf, 0, g_fat.BytesPerCluster);
        memcpy(cluster_buf, source, chunk);
        if (!IoBlockWriteSectors(cluster_to_lba(clusters[i]),
                             (UINT8)g_fat.SectorsPerCluster, cluster_buf)) {
            ExFreePool(cluster_buf);
            ExFreePool(clusters);
            return STATUS_DEVICE_NOT_READY;
        }
        source += chunk;
        remaining -= chunk;
    }
    if (cluster_buf)
        ExFreePool(cluster_buf);
    if (cluster_count && !NT_SUCCESS(FatFlush())) {
        ExFreePool(clusters);
        return STATUS_DEVICE_NOT_READY;
    }

    /* Phase 2: make the fully persisted data reachable as a FAT chain. */
    if (cluster_count) {
        BOOLEAN published = fat_publish_chain(clusters, cluster_count);
        BOOLEAN accounted = published &&
            fat_update_fsinfo(-(INT32)cluster_count,
                              clusters[cluster_count - 1] + 1);
        if (!published || !accounted || !NT_SUCCESS(FatFlush())) {
            fat_rollback_chain(clusters, cluster_count);
            if (published)
                fat_update_fsinfo((INT32)cluster_count, clusters[0]);
            FatFlush();
            ExFreePool(clusters);
            return STATUS_DEVICE_NOT_READY;
        }
    }

    UINT32 first = cluster_count ? clusters[0] : 0;
    entry.FirstClusterHigh = (UINT16)(first >> 16);
    entry.FirstClusterLow = (UINT16)first;
    entry.FileSize = (UINT32)size;
    FAT_ENTRY_LOCATION published_location = location;
    if (!exists && !name_is_83(leaf)) {
        UINT32 lfn_count = lfn_entry_count(leaf);
        if (!lfn_count || !fat_advance_location(location, lfn_count,
                                                 &published_location)) {
            fat_rollback_chain(clusters, cluster_count);
            if (cluster_count)
                fat_update_fsinfo((INT32)cluster_count, first);
            FatFlush();
            if (clusters) ExFreePool(clusters);
            return STATUS_DEVICE_NOT_READY;
        }
    }

    /* Phase 3: atomically select the new chain by publishing its directory
     * entry. The old chain remains intact until this sector is durable. */
    BOOLEAN wrote = exists ? fat_write_dirent(location, &entry)
                           : fat_write_named_entry(directory, location, leaf,
                                                   &entry);
    if (!wrote || !NT_SUCCESS(FatFlush())) {
        if (exists)
            fat_write_dirent(location, &old_entry);
        else if (wrote)
            fat_erase_with_lfn(directory, published_location);
        if (cluster_count) {
            fat_rollback_chain(clusters, cluster_count);
            fat_update_fsinfo((INT32)cluster_count, first);
        }
        FatFlush();
        if (clusters) ExFreePool(clusters);
        return STATUS_DEVICE_NOT_READY;
    }

    /* Phase 4: the old allocation can only become free after the new
     * directory entry is durable. A cleanup failure leaks space rather than
     * damaging the newly committed file. */
    if (old_first >= 2 && !fat_free_chain_committed(old_first))
        KeLog("[io]   warning: old FAT chain cleanup failed for '%s'\n", name);
    if (clusters)
        ExFreePool(clusters);
    KeLog("[io]   stored '%s' (%lu bytes) on FAT32\n", name,
          (unsigned long)size);
    return STATUS_SUCCESS;
}

NTSTATUS FatDeleteFile(const char *name)
{
    UINT32 directory;
    char leaf[260];
    NTSTATUS status = fat_resolve_parent(name, &directory, leaf);
    if (!NT_SUCCESS(status))
        return status;
    FAT_DIRENT entry;
    FAT_ENTRY_LOCATION location;
    if (!fat_find(directory, leaf, &entry, &location) || (entry.Attr & 0x10))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    UINT32 first = ((UINT32)entry.FirstClusterHigh << 16) |
                   entry.FirstClusterLow;
    if (!IoBlockBeginTransaction())
        return STATUS_INTERNAL_ERROR;
    if (!fat_erase_with_lfn(directory, location) ||
        !IoBlockCommitTransaction()) {
        IoBlockRollbackTransaction();
        return STATUS_DEVICE_NOT_READY;
    }
    if (first >= 2 && !fat_free_chain_committed(first)) {
        KeLog("[io]   warning: deleted file chain cleanup failed for '%s'\n",
              name);
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

NTSTATUS FatCreateDirectory(const char *name)
{
    UINT32 parent;
    char leaf[260];
    NTSTATUS status = fat_resolve_parent(name, &parent, leaf);
    if (!NT_SUCCESS(status))
        return status;
    FAT_DIRENT existing;
    if (fat_find(parent, leaf, &existing, NULL))
        return STATUS_OBJECT_NAME_COLLISION;

    UINT8 short83[11];
    FAT_ENTRY_LOCATION location;
    if (name_is_83(leaf)) {
        to_83(leaf, short83);
        if (!fat_find_free_slot(parent, &location))
            return STATUS_INSUFFICIENT_RESOURCES;
    } else {
        make_short_name(leaf, parent, short83);
        UINT32 count = lfn_entry_count(leaf);
        if (!count)
            return STATUS_OBJECT_NAME_INVALID;
        if (!fat_find_free_run(parent, count + 1, &location))
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    UINT32 cluster;
    status = fat_allocate_cluster(&cluster);
    if (!NT_SUCCESS(status))
        return status;

    UINT8 *buf = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                       'rDiF');
    if (!buf) {
        fat_free_chain_committed(cluster);
        return STATUS_NO_MEMORY;
    }
    memset(buf, 0, g_fat.BytesPerCluster);
    FAT_DIRENT *dot = (FAT_DIRENT *)buf;
    memset(dot[0].Name, ' ', 11);
    dot[0].Name[0] = '.';
    dot[0].Attr = 0x10;
    dot[0].FirstClusterHigh = (UINT16)(cluster >> 16);
    dot[0].FirstClusterLow = (UINT16)cluster;
    memset(dot[1].Name, ' ', 11);
    dot[1].Name[0] = '.';
    dot[1].Name[1] = '.';
    dot[1].Attr = 0x10;
    /* A FAT32 child encodes the root parent as zero in its '..' entry. */
    UINT32 on_disk_parent = parent == g_fat.RootCluster ? 0 : parent;
    dot[1].FirstClusterHigh = (UINT16)(on_disk_parent >> 16);
    dot[1].FirstClusterLow = (UINT16)on_disk_parent;
    BOOLEAN ok = IoBlockWriteSectors(cluster_to_lba(cluster),
        (UINT8)g_fat.SectorsPerCluster, buf);
    ExFreePool(buf);
    if (!ok || !NT_SUCCESS(FatFlush())) {
        fat_free_chain_committed(cluster);
        return STATUS_DEVICE_NOT_READY;
    }

    FAT_DIRENT entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.Name, short83, 11);
    entry.Attr = 0x10;
    entry.FirstClusterHigh = (UINT16)(cluster >> 16);
    entry.FirstClusterLow = (UINT16)cluster;
    if (!IoBlockBeginTransaction()) {
        fat_free_chain_committed(cluster);
        return STATUS_INTERNAL_ERROR;
    }
    if (!fat_write_named_entry(parent, location, leaf, &entry) ||
        !IoBlockCommitTransaction()) {
        IoBlockRollbackTransaction();
        fat_free_chain_committed(cluster);
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

static BOOLEAN fat_directory_empty(UINT32 directory)
{
    UINT8 *buf = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                       'eDiF');
    if (!buf)
        return FALSE;
    UINT32 cluster = directory;
    while (cluster >= 2 && cluster < FAT_EOC) {
        if (!IoBlockReadSectors(cluster_to_lba(cluster),
                            (UINT8)g_fat.SectorsPerCluster, buf)) {
            ExFreePool(buf);
            return FALSE;
        }
        FAT_DIRENT *entries = (FAT_DIRENT *)buf;
        UINT32 count = g_fat.BytesPerCluster / sizeof(FAT_DIRENT);
        for (UINT32 i = 0; i < count; i++) {
            FAT_DIRENT *entry = &entries[i];
            if (entry->Name[0] == 0x00) {
                ExFreePool(buf);
                return TRUE;
            }
            if (entry->Name[0] == 0xE5 || entry->Attr == ATTR_LONG_NAME)
                continue;
            if (entry->Name[0] == '.' &&
                (entry->Name[1] == ' ' || entry->Name[1] == '.'))
                continue;
            ExFreePool(buf);
            return FALSE;
        }
        cluster = fat_next_cluster(cluster);
    }
    ExFreePool(buf);
    return TRUE;
}

static BOOLEAN fat_set_directory_parent(UINT32 directory, UINT32 parent)
{
    UINT8 *buf = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                       'pDiF');
    if (!buf)
        return FALSE;
    BOOLEAN ok = IoBlockReadSectors(cluster_to_lba(directory),
                                (UINT8)g_fat.SectorsPerCluster, buf);
    if (ok) {
        FAT_DIRENT *dotdot = &((FAT_DIRENT *)buf)[1];
        ok = dotdot->Name[0] == '.' && dotdot->Name[1] == '.';
        if (ok) {
            UINT32 on_disk_parent = parent == g_fat.RootCluster ? 0 : parent;
            dotdot->FirstClusterHigh = (UINT16)(on_disk_parent >> 16);
            dotdot->FirstClusterLow = (UINT16)on_disk_parent;
            ok = IoBlockWriteSectors(cluster_to_lba(directory),
                                 (UINT8)g_fat.SectorsPerCluster, buf);
        }
    }
    ExFreePool(buf);
    return ok;
}

static UINT32 fat_directory_parent(UINT32 directory)
{
    UINT8 *buf = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                       'gDiF');
    if (!buf)
        return 0;
    UINT32 parent = 0;
    if (IoBlockReadSectors(cluster_to_lba(directory),
                       (UINT8)g_fat.SectorsPerCluster, buf)) {
        FAT_DIRENT *dotdot = &((FAT_DIRENT *)buf)[1];
        if (dotdot->Name[0] == '.' && dotdot->Name[1] == '.')
            parent = ((UINT32)dotdot->FirstClusterHigh << 16) |
                     dotdot->FirstClusterLow;
    }
    ExFreePool(buf);
    return parent ? parent : g_fat.RootCluster;
}

static BOOLEAN fat_is_directory_descendant(UINT32 candidate, UINT32 ancestor)
{
    for (UINT32 depth = 0; depth < 256 && candidate >= 2; depth++) {
        if (candidate == ancestor)
            return TRUE;
        if (candidate == g_fat.RootCluster)
            return FALSE;
        UINT32 parent = fat_directory_parent(candidate);
        if (parent < 2 || parent == candidate)
            return FALSE;
        candidate = parent;
    }
    return FALSE;
}

NTSTATUS FatRemoveDirectory(const char *name)
{
    UINT32 parent;
    char leaf[260];
    NTSTATUS status = fat_resolve_parent(name, &parent, leaf);
    if (!NT_SUCCESS(status))
        return status;
    FAT_DIRENT entry;
    FAT_ENTRY_LOCATION location;
    if (!fat_find(parent, leaf, &entry, &location) || !(entry.Attr & 0x10))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    UINT32 cluster = ((UINT32)entry.FirstClusterHigh << 16) |
                     entry.FirstClusterLow;
    if (!fat_directory_empty(cluster))
        return STATUS_DIRECTORY_NOT_EMPTY;
    if (!IoBlockBeginTransaction())
        return STATUS_INTERNAL_ERROR;
    if (!fat_erase_with_lfn(parent, location) ||
        !IoBlockCommitTransaction()) {
        IoBlockRollbackTransaction();
        return STATUS_DEVICE_NOT_READY;
    }
    if (!fat_free_chain_committed(cluster)) {
        KeLog("[io]   warning: removed directory chain cleanup failed for '%s'\n",
              name);
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

NTSTATUS FatRenamePath(const char *old_name, const char *new_name)
{
    UINT32 old_parent, new_parent;
    char old_leaf[260], new_leaf[260];
    NTSTATUS status = fat_resolve_parent(old_name, &old_parent, old_leaf);
    if (!NT_SUCCESS(status)) return status;
    status = fat_resolve_parent(new_name, &new_parent, new_leaf);
    if (!NT_SUCCESS(status))
        return status;

    FAT_DIRENT entry, collision;
    FAT_ENTRY_LOCATION old_location, new_location;
    if (!fat_find(old_parent, old_leaf, &entry, &old_location))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (fat_find(new_parent, new_leaf, &collision, NULL))
        return STATUS_OBJECT_NAME_COLLISION;

    UINT32 moved_cluster = ((UINT32)entry.FirstClusterHigh << 16) |
                           entry.FirstClusterLow;
    BOOLEAN moving_directory = (entry.Attr & 0x10) != 0;
    if (moving_directory && fat_is_directory_descendant(new_parent,
                                                         moved_cluster))
        return STATUS_INVALID_PARAMETER;

    UINT8 short83[11];
    if (name_is_83(new_leaf)) {
        to_83(new_leaf, short83);
        if (!fat_find_free_slot(new_parent, &new_location))
            return STATUS_INSUFFICIENT_RESOURCES;
    } else {
        make_short_name(new_leaf, new_parent, short83);
        UINT32 count = lfn_entry_count(new_leaf);
        if (!count)
            return STATUS_OBJECT_NAME_INVALID;
        if (!fat_find_free_run(new_parent, count + 1, &new_location))
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    memcpy(entry.Name, short83, 11);
    if (!IoBlockBeginTransaction())
        return STATUS_INTERNAL_ERROR;
    if (!fat_write_named_entry(new_parent, new_location, new_leaf, &entry) ||
        !IoBlockCommitTransaction()) {
        IoBlockRollbackTransaction();
        return STATUS_DEVICE_NOT_READY;
    }

    FAT_ENTRY_LOCATION new_short_location = new_location;
    if (!name_is_83(new_leaf)) {
        UINT32 count = lfn_entry_count(new_leaf);
        if (!count || !fat_advance_location(new_location, count,
                                  &new_short_location))
            return STATUS_DEVICE_NOT_READY;
    }

    if (moving_directory && old_parent != new_parent) {
        if (!IoBlockBeginTransaction())
            return STATUS_INTERNAL_ERROR;
        if (!fat_set_directory_parent(moved_cluster, new_parent) ||
            !IoBlockCommitTransaction()) {
            IoBlockRollbackTransaction();
            if (IoBlockBeginTransaction()) {
                fat_erase_with_lfn(new_parent, new_short_location);
                IoBlockCommitTransaction();
            }
            return STATUS_DEVICE_NOT_READY;
        }
    }
    if (!IoBlockBeginTransaction())
        return STATUS_INTERNAL_ERROR;
    if (!fat_erase_with_lfn(old_parent, old_location) ||
        !IoBlockCommitTransaction()) {
        IoBlockRollbackTransaction();
        if (moving_directory && old_parent != new_parent &&
            IoBlockBeginTransaction()) {
            fat_set_directory_parent(moved_cluster, old_parent);
            IoBlockCommitTransaction();
        }
        if (IoBlockBeginTransaction()) {
            fat_erase_with_lfn(new_parent, new_short_location);
            IoBlockCommitTransaction();
        }
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

NTSTATUS IoInitialize(void)
{
    /* File/console objects need Ob but not the disk, so set them up regardless. */
    IoInitializeObjects();

    if (!AtaInitialize())
        return STATUS_NO_SUCH_DEVICE;
    return FatMount();
}
