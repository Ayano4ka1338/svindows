/*
 * io/fat.c - a compact FAT32 driver.
 *
 * Parses the BPB, walks the root directory with VFAT long-file-name support,
 * and follows cluster chains to read files into pool buffers.
 */
#include <svindows/io.h>
#include <svindows/ex.h>
#include <svindows/ke.h>
#include <svindows/rtl.h>

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
    if (!AtaReadSectors(g_fat.FsInfoSector, 1, sector))
        return FALSE;
    FAT_FSINFO *info = (FAT_FSINFO *)sector;
    if (info->LeadSignature != 0x41615252u ||
        info->StructSignature != 0x61417272u ||
        info->TrailSignature != 0xAA550000u)
        return TRUE;

    if (info->FreeClusterCount != 0xffffffffu) {
        if (free_delta < 0 && info->FreeClusterCount)
            info->FreeClusterCount--;
        else if (free_delta > 0 &&
                 info->FreeClusterCount < g_fat.TotalClusters)
            info->FreeClusterCount++;
    }
    if (next_free >= 2 && next_free < g_fat.TotalClusters + 2)
        info->NextFreeCluster = next_free;

    if (!AtaWriteSectors(g_fat.FsInfoSector, 1, sector))
        return FALSE;
    if (g_fat.BackupBootSector) {
        UINT32 backup = g_fat.BackupBootSector + g_fat.FsInfoSector;
        if (!AtaWriteSectors(backup, 1, sector))
            return FALSE;
    }
    return TRUE;
}

static UINT32 cluster_to_lba(UINT32 cluster)
{
    return g_fat.DataStartLba + (cluster - 2) * g_fat.SectorsPerCluster;
}

/* Read the next cluster in the chain from the FAT. */
static UINT32 fat_next_cluster(UINT32 cluster)
{
    UINT8 sector[ATA_SECTOR_SIZE];
    UINT32 fat_offset = cluster * 4;
    UINT32 lba = g_fat.FatStartLba + fat_offset / ATA_SECTOR_SIZE;
    UINT32 in_sector = fat_offset % ATA_SECTOR_SIZE;

    if (!AtaReadSectors(lba, 1, sector))
        return FAT_EOC;
    UINT32 value = *(UINT32 *)&sector[in_sector];
    return value & 0x0FFFFFFFu;
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
        if (!AtaReadSectors(lba, 1, sector))
            return FALSE;
        UINT32 old = *(UINT32 *)&sector[in_sector];
        *(UINT32 *)&sector[in_sector] = (old & 0xF0000000u) |
                                        (value & 0x0FFFFFFFu);
        if (!AtaWriteSectors(lba, 1, sector))
            return FALSE;
    }
    return TRUE;
}

static UINT32 fat_allocate_cluster(void)
{
    for (UINT32 cluster = 2; cluster < g_fat.TotalClusters + 2; cluster++) {
        if (fat_next_cluster(cluster) == 0) {
            if (!fat_set_cluster(cluster, 0x0FFFFFFFu))
                return 0;
            UINT8 *zero = ExAllocatePoolWithTag(NonPagedPool,
                                                 g_fat.BytesPerCluster,
                                                 'zClF');
            if (!zero) {
                fat_set_cluster(cluster, 0);
                return 0;
            }
            memset(zero, 0, g_fat.BytesPerCluster);
            BOOLEAN written = AtaWriteSectors(cluster_to_lba(cluster),
                (UINT8)g_fat.SectorsPerCluster, zero);
            ExFreePool(zero);
            if (!written) {
                fat_set_cluster(cluster, 0);
                return 0;
            }
            if (!fat_update_fsinfo(-1, cluster + 1)) {
                fat_set_cluster(cluster, 0);
                return 0;
            }
            return cluster;
        }
    }
    return 0;
}

static void fat_free_chain(UINT32 cluster)
{
    while (cluster >= 2 && cluster < FAT_EOC) {
        UINT32 next = fat_next_cluster(cluster);
        if (!fat_set_cluster(cluster, 0))
            return;
        if (!fat_update_fsinfo(1, cluster))
            KeLog("[io]   warning: failed to update FAT32 FSInfo\n");
        cluster = next;
    }
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

NTSTATUS FatMount(void)
{
    UINT8 sector[ATA_SECTOR_SIZE];
    if (!AtaReadSectors(0, 1, sector))
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

    KeLog("[io]   fat32 mounted: %u sec/cluster, fat@%u data@%u root=cluster %u\n",
          (unsigned)g_fat.SectorsPerCluster, (unsigned)g_fat.FatStartLba,
          (unsigned)g_fat.DataStartLba, (unsigned)g_fat.RootCluster);
    return STATUS_SUCCESS;
}

/* Search the directory chain starting at `dir_cluster` for `name83`. */
static BOOLEAN name_equal_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return FALSE;
    }
    return (BOOLEAN)(*a == *b);
}

static void lfn_put_fragment(char out[260], const FAT_LFN_ENTRY *lfn)
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
            out[pos + i] = c <= 0x7f ? (char)c : '?';
    }
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
        if (in_ext) {
            if (++ext > 3)
                return FALSE;
        } else {
            if (++base > 8)
                return FALSE;
        }
    }
    return base != 0;
}

typedef struct _FAT_ENTRY_LOCATION {
    UINT32 Cluster;
    UINT32 Index;
} FAT_ENTRY_LOCATION;

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
    char long_name[260];
    BOOLEAN have_lfn = FALSE;
    memset(long_name, 0, sizeof(long_name));
    UINT32 cluster = dir_cluster;
    while (cluster < FAT_EOC && !found) {
        if (!AtaReadSectors(cluster_to_lba(cluster),
                            (UINT8)g_fat.SectorsPerCluster, cluster_buf))
            break;

        FAT_DIRENT *ents = (FAT_DIRENT *)cluster_buf;
        UINT32 n = g_fat.BytesPerCluster / sizeof(FAT_DIRENT);
        for (UINT32 i = 0; i < n; i++) {
            FAT_DIRENT *e = &ents[i];
            if (e->Name[0] == 0x00)
                goto done; /* end of directory */
            if (e->Name[0] == 0xE5) {
                have_lfn = FALSE;
                continue;
            }
            if (e->Attr == ATTR_LONG_NAME) {
                FAT_LFN_ENTRY *lfn = (FAT_LFN_ENTRY *)e;
                if (lfn->Order & 0x40) {
                    memset(long_name, 0, sizeof(long_name));
                    have_lfn = TRUE;
                }
                if (have_lfn)
                    lfn_put_fragment(long_name, lfn);
                continue;
            }
            if ((have_lfn && name_equal_ci(long_name, name)) ||
                (short_name_usable && memcmp(e->Name, name83, 11) == 0)) {
                *out_entry = *e;
                if (out_location) {
                    out_location->Cluster = cluster;
                    out_location->Index = i;
                }
                found = TRUE;
                break;
            }
            have_lfn = FALSE;
        }
        cluster = fat_next_cluster(cluster);
    }
done:
    ExFreePool(cluster_buf);
    return found;
}

static NTSTATUS fat_resolve_parent(const char *path, UINT32 *directory,
                                   char leaf[64])
{
    if (!path || !*path)
        return STATUS_INVALID_PARAMETER;
    UINT32 current = g_fat.RootCluster;
    UINT32 pos = 0;
    if (path[0] && path[1] == ':')
        pos = 2;
    while (path[pos] == '\\' || path[pos] == '/') pos++;

    for (;;) {
        char component[64];
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
        UINT32 parent; char leaf[64]; FAT_DIRENT dirent;
        NTSTATUS status = fat_resolve_parent(path, &parent, leaf);
        if (!NT_SUCCESS(status) || !fat_find(parent, leaf, &dirent, NULL) ||
            !(dirent.Attr & 0x10)) return STATUS_OBJECT_PATH_NOT_FOUND;
        directory = ((UINT32)dirent.FirstClusterHigh << 16) |
                    dirent.FirstClusterLow;
    }

    UINT32 seen = 0;
    char long_name[260];
    BOOLEAN have_lfn = FALSE;
    memset(long_name, 0, sizeof(long_name));
    UINT32 cluster = directory;
    while (cluster < FAT_EOC) {
        if (!AtaReadSectors(cluster_to_lba(cluster),
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
                have_lfn = FALSE;
                continue;
            }
            if (e->Attr == ATTR_LONG_NAME) {
                FAT_LFN_ENTRY *lfn = (FAT_LFN_ENTRY *)e;
                if (lfn->Order & 0x40) {
                    memset(long_name, 0, sizeof(long_name));
                    have_lfn = TRUE;
                }
                if (have_lfn)
                    lfn_put_fragment(long_name, lfn);
                continue;
            }
            if (e->Attr & 0x08) { /* volume label */
                have_lfn = FALSE;
                continue;
            }
            if (seen++ != wanted)
                goto next_entry;
            if (have_lfn) {
                UINT32 j = 0;
                for (; long_name[j] && j < sizeof(out->Name) - 1; j++)
                    out->Name[j] = long_name[j];
                out->Name[j] = 0;
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
            have_lfn = FALSE;
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
    char leaf[64];
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
        if (!AtaReadSectors(cluster_to_lba(cluster),
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
    char leaf[64];
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
    BOOLEAN ok = AtaReadSectors(cluster_to_lba(location.Cluster),
        (UINT8)g_fat.SectorsPerCluster, buf);
    if (ok) {
        ((FAT_DIRENT *)buf)[location.Index] = *entry;
        ok = AtaWriteSectors(cluster_to_lba(location.Cluster),
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
        if (!AtaReadSectors(cluster_to_lba(cluster),
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

    UINT32 extension = fat_allocate_cluster();
    if (!extension || !fat_set_cluster(previous, extension)) {
        if (extension)
            fat_set_cluster(extension, 0);
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
    char leaf[64];
    NTSTATUS resolved = fat_resolve_parent(name, &directory, leaf);
    if (!NT_SUCCESS(resolved) || !name_is_83(leaf))
        return NT_SUCCESS(resolved) ? STATUS_INVALID_PARAMETER : resolved;

    FAT_DIRENT entry;
    FAT_ENTRY_LOCATION location;
    BOOLEAN exists = fat_find(directory, leaf, &entry, &location);
    if (!exists) {
        if (!create_if_missing)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        if (!fat_find_free_slot(directory, &location))
            return STATUS_INSUFFICIENT_RESOURCES;
        memset(&entry, 0, sizeof(entry));
        to_83(leaf, entry.Name);
        entry.Attr = 0x20; /* archive */
    }

    UINT32 old_first = ((UINT32)entry.FirstClusterHigh << 16) |
                       entry.FirstClusterLow;
    UINT32 first = 0, previous = 0;
    SIZE_T remaining = size;
    const UINT8 *source = (const UINT8 *)buffer;
    UINT8 *cluster_buf = NULL;
    if (size) {
        cluster_buf = ExAllocatePoolWithTag(NonPagedPool,
                                             g_fat.BytesPerCluster, 'wFlF');
        if (!cluster_buf)
            return STATUS_NO_MEMORY;
    }

    while (remaining) {
        UINT32 cluster = fat_allocate_cluster();
        if (!cluster) {
            if (first) fat_free_chain(first);
            ExFreePool(cluster_buf);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (!first) first = cluster;
        if (previous && !fat_set_cluster(previous, cluster)) {
            fat_free_chain(first);
            ExFreePool(cluster_buf);
            return STATUS_DEVICE_NOT_READY;
        }
        SIZE_T chunk = remaining < g_fat.BytesPerCluster
                         ? remaining : g_fat.BytesPerCluster;
        memset(cluster_buf, 0, g_fat.BytesPerCluster);
        memcpy(cluster_buf, source, chunk);
        if (!AtaWriteSectors(cluster_to_lba(cluster),
                             (UINT8)g_fat.SectorsPerCluster, cluster_buf)) {
            fat_free_chain(first);
            ExFreePool(cluster_buf);
            return STATUS_DEVICE_NOT_READY;
        }
        source += chunk;
        remaining -= chunk;
        previous = cluster;
    }
    if (cluster_buf)
        ExFreePool(cluster_buf);

    entry.FirstClusterHigh = (UINT16)(first >> 16);
    entry.FirstClusterLow = (UINT16)first;
    entry.FileSize = (UINT32)size;
    if (!fat_write_dirent(location, &entry)) {
        if (first) fat_free_chain(first);
        return STATUS_DEVICE_NOT_READY;
    }
    if (old_first >= 2)
        fat_free_chain(old_first);
    KeLog("[io]   stored '%s' (%lu bytes) on FAT32\n", name,
          (unsigned long)size);
    return STATUS_SUCCESS;
}

NTSTATUS FatDeleteFile(const char *name)
{
    UINT32 directory;
    char leaf[64];
    NTSTATUS status = fat_resolve_parent(name, &directory, leaf);
    if (!NT_SUCCESS(status))
        return status;
    FAT_DIRENT entry;
    FAT_ENTRY_LOCATION location;
    if (!fat_find(directory, leaf, &entry, &location) || (entry.Attr & 0x10))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    UINT32 first = ((UINT32)entry.FirstClusterHigh << 16) |
                   entry.FirstClusterLow;
    entry.Name[0] = 0xE5;
    if (!fat_write_dirent(location, &entry))
        return STATUS_DEVICE_NOT_READY;
    if (first >= 2)
        fat_free_chain(first);
    return STATUS_SUCCESS;
}

NTSTATUS FatCreateDirectory(const char *name)
{
    UINT32 parent;
    char leaf[64];
    NTSTATUS status = fat_resolve_parent(name, &parent, leaf);
    if (!NT_SUCCESS(status) || !name_is_83(leaf))
        return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;
    FAT_DIRENT existing;
    if (fat_find(parent, leaf, &existing, NULL))
        return STATUS_OBJECT_NAME_COLLISION;

    FAT_ENTRY_LOCATION location;
    if (!fat_find_free_slot(parent, &location))
        return STATUS_INSUFFICIENT_RESOURCES;
    UINT32 cluster = fat_allocate_cluster();
    if (!cluster)
        return STATUS_INSUFFICIENT_RESOURCES;

    UINT8 *buf = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                       'rDiF');
    if (!buf) {
        fat_free_chain(cluster);
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
    dot[1].FirstClusterHigh = (UINT16)(parent >> 16);
    dot[1].FirstClusterLow = (UINT16)parent;
    BOOLEAN ok = AtaWriteSectors(cluster_to_lba(cluster),
        (UINT8)g_fat.SectorsPerCluster, buf);
    ExFreePool(buf);
    if (!ok) {
        fat_free_chain(cluster);
        return STATUS_DEVICE_NOT_READY;
    }

    FAT_DIRENT entry;
    memset(&entry, 0, sizeof(entry));
    to_83(leaf, entry.Name);
    entry.Attr = 0x10;
    entry.FirstClusterHigh = (UINT16)(cluster >> 16);
    entry.FirstClusterLow = (UINT16)cluster;
    if (!fat_write_dirent(location, &entry)) {
        fat_free_chain(cluster);
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
        if (!AtaReadSectors(cluster_to_lba(cluster),
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

NTSTATUS FatRemoveDirectory(const char *name)
{
    UINT32 parent;
    char leaf[64];
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
    entry.Name[0] = 0xE5;
    if (!fat_write_dirent(location, &entry))
        return STATUS_DEVICE_NOT_READY;
    fat_free_chain(cluster);
    return STATUS_SUCCESS;
}

NTSTATUS FatRenamePath(const char *old_name, const char *new_name)
{
    UINT32 old_parent, new_parent;
    char old_leaf[64], new_leaf[64];
    NTSTATUS status = fat_resolve_parent(old_name, &old_parent, old_leaf);
    if (!NT_SUCCESS(status)) return status;
    status = fat_resolve_parent(new_name, &new_parent, new_leaf);
    if (!NT_SUCCESS(status) || !name_is_83(new_leaf))
        return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;

    FAT_DIRENT entry, collision;
    FAT_ENTRY_LOCATION old_location, new_location;
    if (!fat_find(old_parent, old_leaf, &entry, &old_location))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (fat_find(new_parent, new_leaf, &collision, NULL))
        return STATUS_OBJECT_NAME_COLLISION;
    if (!fat_find_free_slot(new_parent, &new_location))
        return STATUS_INSUFFICIENT_RESOURCES;
    to_83(new_leaf, entry.Name);
    if (!fat_write_dirent(new_location, &entry))
        return STATUS_DEVICE_NOT_READY;
    entry.Name[0] = 0xE5;
    if (!fat_write_dirent(old_location, &entry))
        return STATUS_DEVICE_NOT_READY;
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
