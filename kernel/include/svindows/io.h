/*
 * svindows/io.h - I/O manager (Io): block device + filesystem access.
 *
 * A polled ATA PIO driver for the primary disk and a writable FAT32 layer used
 * by native file objects and the Win32 filesystem facade.
 */
#ifndef _SVINDOWS_IO_H_
#define _SVINDOWS_IO_H_

#include <nt/ntdef.h>
#include <nt/ntstatus.h>

/* ------------------------------------------------------------------ */
/* ATA PIO block device (primary bus, master)                         */
/* ------------------------------------------------------------------ */

#define ATA_SECTOR_SIZE 512

BOOLEAN AtaInitialize(void);
/* Read `count` (1..255) 512-byte sectors starting at LBA into `buffer`. */
BOOLEAN AtaReadSectors(UINT32 lba, UINT8 count, void *buffer);
BOOLEAN AtaWriteSectors(UINT32 lba, UINT8 count, const void *buffer);

/* ------------------------------------------------------------------ */
/* FAT32 filesystem                                                   */
/* ------------------------------------------------------------------ */

NTSTATUS FatMount(void);

typedef struct _FAT_FIND_DATA {
    char   Name[260];
    UINT32 Size;
    UINT32 Attributes;
    UINT16 WriteDate;
    UINT16 WriteTime;
} FAT_FIND_DATA;

/* Read a root file into a freshly allocated pool buffer. When supplied,
 * out_info receives the directory metadata used to create its File object. */
NTSTATUS FatLoadFile(const char *name, void **out_buffer, SIZE_T *out_size,
                     FAT_FIND_DATA *out_info);

/* Enumerate visible root entries (VFAT long names when present) by zero-based
 * index. This is the directory-query primitive used by Win32 find. */
NTSTATUS FatEnumerateRoot(UINT32 index, FAT_FIND_DATA *out);
NTSTATUS FatEnumerateDirectory(const char *path, UINT32 index,
                               FAT_FIND_DATA *out);
NTSTATUS FatStoreFile(const char *name, const void *buffer, SIZE_T size,
                      BOOLEAN create_if_missing);
NTSTATUS FatDeleteFile(const char *name);
NTSTATUS FatRenamePath(const char *old_name, const char *new_name);
NTSTATUS FatCreateDirectory(const char *name);
NTSTATUS FatRemoveDirectory(const char *name);
NTSTATUS FatQueryPath(const char *name, FAT_FIND_DATA *out_info);

/* Bring up the I/O subsystem (ATA + mount FAT + I/O objects). */
NTSTATUS IoInitialize(void);

/* ------------------------------------------------------------------ */
/* File objects and the handle-based file services                    */
/* ------------------------------------------------------------------ */

/* Register the File object type and create \Device\Console. Requires Ob. */
void IoInitializeObjects(void);

/* Feed translated keyboard input to the console device.  The hardware input
 * worker is the single producer and console ReadFile is the consumer. */
void IoConsoleInputCharacter(char c);

/*
 * Handle-based Nt* file services with the real Windows signatures (via the
 * argument array the syscall entry passes). See io/file.c for the parameter
 * layouts (OBJECT_ATTRIBUTES / IO_STATUS_BLOCK).
 */
UINT64 NtCreateFile(UINT64 *args);
UINT64 NtReadFile(UINT64 *args);
UINT64 NtWriteFile(UINT64 *args);
UINT64 NtClose(UINT64 *args);
UINT64 NtEnumerateRootFiles(UINT64 *args); /* private SVINDOWS loader/runtime hook */

typedef struct _SVINDOWS_FILE_INFO {
    UINT64 Size;
    UINT64 Position;
    UINT32 Attributes;
    UINT32 IsConsole;
    UINT16 FatWriteDate;
    UINT16 FatWriteTime;
    UINT32 Reserved;
} SVINDOWS_FILE_INFO;

UINT64 NtQueryFileInfo(UINT64 *args); /* private SVINDOWS loader/runtime hook */
UINT64 NtSetFilePosition(UINT64 *args); /* private SVINDOWS loader/runtime hook */
UINT64 NtFileSystemControlSimple(UINT64 *args);
UINT64 NtSetFileSizeSimple(UINT64 *args);
UINT64 NtCreatePipeSimple(UINT64 *args);
UINT64 NtConsoleControlSimple(UINT64 *args);
UINT64 NtEnumerateDirectorySimple(UINT64 *args);

#endif /* _SVINDOWS_IO_H_ */
