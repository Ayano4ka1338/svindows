/*
 * svindows/Io.h - I/O manager (Io): block device + filesystem access.
 *
 * A memory-backed Limine initrd block device and a writable FAT32 layer used
 * by native file objects and the Win32 filesystem facade.
 */
#ifndef _SVINDOWS_IO_H_
#define _SVINDOWS_IO_H_

#include <nt/Ntdef.h>
#include <nt/Ntstatus.h>
#include <svindows/Ob.h>

/* Waitable completion-port objects and packets. A file retains a reference
 * to its associated port until the file object is deleted. */
void IoInitializeCompletionObjects(void);
NTSTATUS IoReferenceCompletionPort(HANDLE handle, PVOID *object);
NTSTATUS IoPostCompletion(PVOID port, ULONG_PTR key, PVOID overlapped,
                          NTSTATUS status, SIZE_T information);
UINT64 NtCreateIoCompletion(UINT64 *args);
UINT64 NtSetIoCompletion(UINT64 *args);
UINT64 NtRemoveIoCompletion(UINT64 *args);
UINT64 NtAssociateIoCompletionFile(UINT64 *args);

/* Small synchronous IRP/device contract. Drivers receive one current stack
 * location; completion is explicit and carries status and byte count. */
typedef struct _IO_IRP IO_IRP;
typedef struct _IO_DEVICE_OBJECT IO_DEVICE_OBJECT;
typedef NTSTATUS (*PIO_DISPATCH)(IO_DEVICE_OBJECT *device, IO_IRP *irp);
typedef void (*PIO_COMPLETION)(IO_DEVICE_OBJECT *device, IO_IRP *irp,
                               void *context);
enum { IRP_MJ_READ = 0, IRP_MJ_WRITE = 1, IRP_MJ_FLUSH = 2,
       IRP_MJ_MAXIMUM = 3 };
typedef struct _IO_DRIVER_OBJECT {
    PIO_DISPATCH MajorFunction[IRP_MJ_MAXIMUM];
} IO_DRIVER_OBJECT;
struct _IO_DEVICE_OBJECT {
    IO_DRIVER_OBJECT *DriverObject;
    void *DeviceExtension;
};
typedef struct _IO_STACK_LOCATION {
    UINT8 MajorFunction;
    UINT32 Sector;
    UINT8 SectorCount;
    void *Buffer;
} IO_STACK_LOCATION;
struct _IO_IRP {
    IO_STACK_LOCATION Stack;
    NTSTATUS Status;
    SIZE_T Information;
    PIO_COMPLETION Completion;
    void *CompletionContext;
    BOOLEAN Completed;
};
NTSTATUS IoCallDriver(IO_DEVICE_OBJECT *device, IO_IRP *irp);
void IoCompleteRequest(IO_DEVICE_OBJECT *device, IO_IRP *irp,
                       NTSTATUS status, SIZE_T information);
BOOLEAN IoDiskReadSectors(UINT32 lba, UINT8 count, void *buffer);
BOOLEAN IoDiskWriteSectors(UINT32 lba, UINT8 count, const void *buffer);
BOOLEAN IoDiskFlush(void);
BOOLEAN IoDiskSetMemoryImage(void *image, SIZE_T size);
BOOLEAN IoDiskIsMemoryBacked(void);

/* ------------------------------------------------------------------ */
/* ATA PIO fallback block device (primary bus, master)                */
/* ------------------------------------------------------------------ */

#define ATA_SECTOR_SIZE 512

BOOLEAN AtaInitialize(void);
/* Read `count` (1..255) 512-byte sectors starting at LBA into `buffer`. */
BOOLEAN AtaReadSectors(UINT32 lba, UINT8 count, void *buffer);
BOOLEAN AtaWriteSectors(UINT32 lba, UINT8 count, const void *buffer);
BOOLEAN AtaFlushCache(void);

/* Shared write-back sector cache used by filesystem backends. */
BOOLEAN IoBlockReadSectors(UINT32 lba, UINT8 count, void *buffer);
BOOLEAN IoBlockWriteSectors(UINT32 lba, UINT8 count, const void *buffer);
BOOLEAN IoBlockFlush(void);
BOOLEAN IoBlockBeginTransaction(void);
BOOLEAN IoBlockCommitTransaction(void);
BOOLEAN IoBlockRollbackTransaction(void);
/* Test hook: fail one physical sector write after `count` successful writes.
 * A negative count disables injection. */
void IoBlockSetWriteFault(INT64 count);
INT64 IoBlockGetWriteCount(void);

/* ------------------------------------------------------------------ */
/* FAT32 filesystem                                                   */
/* ------------------------------------------------------------------ */

NTSTATUS FatMount(void);
NTSTATUS FatFlush(void);

typedef struct _FAT_FIND_DATA {
    char   Name[260];
    UINT32 Size;
    UINT32 Attributes;
    UINT16 WriteDate;
    UINT16 WriteTime;
    char   ShortName[13];
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
NTSTATUS FatSetFileDateTime(const char *name, UINT16 date, UINT16 time);
NTSTATUS FatQueryDiskSpace(UINT32 *sectors_per_cluster,
                           UINT32 *free_clusters, UINT32 *bytes_per_sector,
                           UINT32 *total_clusters);

/* Bring up the I/O subsystem (ATA + mount FAT + I/O objects). */
NTSTATUS IoInitialize(void);

/* ------------------------------------------------------------------ */
/* File objects and the handle-based file services                    */
/* ------------------------------------------------------------------ */

/* Register the File object type and create \Device\Console. Requires Ob. */
void IoInitializeObjects(void);
/* Borrow a disk file's bytes while retaining its file-object reference. */
NTSTATUS IoReferenceFileData(HANDLE handle, PVOID *file_object,
                             const UINT8 **data, SIZE_T *size);

/* ------------------------------------------------------------------ */
/* Character-device file objects                                       */
/* ------------------------------------------------------------------ */

/* A personality (Linux) can expose its byte-stream devices (PTYs, VTs) as
 * native File objects, so PE and Linux programs can share the same console
 * streams through one handle. The I/O manager never interprets the device;
 * it only dispatches to the callbacks below. Buffers are user-mode pointers
 * already probed by the calling service. */
typedef struct _IO_CHAR_DEVICE_OPS {
    SIZE_T (*Read)(void *context, void *buffer, SIZE_T length);
    SIZE_T (*TryRead)(void *context, void *buffer, SIZE_T length);
    BOOLEAN (*InputAvailable)(void *context);
    SIZE_T (*Write)(void *context, const void *buffer, SIZE_T length);
    void (*PrepareWindows)(void *context); /* Windows-console semantics */
    NTSTATUS (*ConsoleControl)(void *context, UINT32 operation,
                               UINT64 argument1, UINT64 argument2,
                               UINT64 argument3, UINT64 *result);
    void (*Close)(void *context);
} IO_CHAR_DEVICE_OPS;

#define IO_CHAR_DEVICE_TAG_PTY 0x53595450ULL /* "PTYS": pseudo terminal */
#define IO_CHAR_DEVICE_TAG_TTY 0x59545454ULL /* "TTTY": VT console      */

/* Create a File object backed by a personality device and open a handle to
 * it in the current process's table. The context stays owned by the caller
 * until Close reports the final reference is gone. */
NTSTATUS IoCreateCharDeviceFile(UINT64 tag, void *context,
                                const IO_CHAR_DEVICE_OPS *ops, HANDLE *out);
/* Returns STATUS_SUCCESS for a char-device handle and fills the device tag
 * and context; any other file kind gives STATUS_INVALID_HANDLE. The InTable
 * variant inspects a specific process's handle table, for callers that probe
 * a child's inherited handles while the creator's table is current. */
NTSTATUS IoQueryCharDeviceFile(HANDLE handle, UINT64 *tag, void **context);
NTSTATUS IoQueryCharDeviceFileInTable(POB_HANDLE_TABLE table, HANDLE handle,
                                      UINT64 *tag, void **context);
/* Ask a char-device handle to switch to Windows-console semantics (raw
 * input, program-owned echo). Safe to call on any handle. */
void IoPrepareFileForWindows(HANDLE handle);
/* Open a handle to the shared console device in the current process's
 * handle table; used when a Linux VT stream is handed to a PE child. */
NTSTATUS IoOpenConsoleKernel(HANDLE *out);
NTSTATUS IoIsConsoleFile(HANDLE handle);
NTSTATUS IoIsConsoleFileInTable(POB_HANDLE_TABLE table, HANDLE handle);
NTSTATUS IoReadHandleKernel(HANDLE handle, void *buffer, SIZE_T length,
                            SIZE_T *read_out);
NTSTATUS IoTryReadHandleKernel(HANDLE handle, void *buffer, SIZE_T length,
                               SIZE_T *read_out);
NTSTATUS IoHandleInputAvailableKernel(HANDLE handle, BOOLEAN *available);
NTSTATUS IoFlushInputHandleKernel(HANDLE handle);
NTSTATUS IoWriteHandleKernel(HANDLE handle, const void *buffer, SIZE_T length,
                             SIZE_T *written_out);
NTSTATUS IoOpenFileKernel(const char *path, ULONG access, BOOLEAN create,
                          BOOLEAN exclusive, BOOLEAN truncate,
                          HANDLE *handle_out);
NTSTATUS IoSeekHandleKernel(HANDLE handle, INT64 offset, UINT32 whence,
                            UINT64 *position_out);
BOOLEAN IoCanonicalizeDosPathKernel(const char *input, char output[260]);
BOOLEAN IoResolveDosPathKernel(const char *input, const char *current_directory,
                               char output[260]);

/* Feed translated keyboard input to the console device.  The hardware input
 * worker is the single producer and console ReadFile is the consumer. */
void IoConsoleInputCharacter(char c);

/*
 * Handle-based Nt* file services with the real Windows signatures (via the
 * argument array the syscall entry passes). See io/File.c for the parameter
 * layouts (OBJECT_ATTRIBUTES / IO_STATUS_BLOCK).
 */
UINT64 NtCreateFile(UINT64 *args);
UINT64 NtOpenFile(UINT64 *args);
UINT64 NtReadFile(UINT64 *args);
UINT64 NtQueryInformationFile(UINT64 *args);
UINT64 NtWriteFile(UINT64 *args);
UINT64 NtQueryDirectoryFile(UINT64 *args);
UINT64 NtSetInformationFile(UINT64 *args);
UINT64 NtFlushBuffersFile(UINT64 *args);
UINT64 NtClose(UINT64 *args);

typedef struct _SVINDOWS_FILE_INFO {
    UINT64 Size;
    UINT64 Position;
    UINT32 Attributes;
    UINT32 IsConsole;
    UINT16 FatWriteDate;
    UINT16 FatWriteTime;
    UINT32 Reserved;
} SVINDOWS_FILE_INFO;

typedef struct _IO_KERNEL_FILE_STAT {
    UINT64 Size;
    UINT64 FileId;
    UINT32 Attributes;
    UINT32 Kind; /* 0 disk, 1 console, 2 pipe, 3 directory */
    UINT16 FatWriteDate;
    UINT16 FatWriteTime;
} IO_KERNEL_FILE_STAT;
NTSTATUS IoQueryFileStatKernel(HANDLE handle, IO_KERNEL_FILE_STAT *out);
NTSTATUS IoCreatePipeKernel(PHANDLE read_out, PHANDLE write_out);
/* A referenced pipe endpoint for poll users; release with ObDereferenceObject. */
#define IO_POLL_READABLE 0x01u
#define IO_POLL_WRITABLE 0x02u
#define IO_POLL_HANGUP   0x04u
#define IO_POLL_ERROR    0x08u
NTSTATUS IoReferencePipeForPoll(HANDLE handle, PVOID *object_out);
UINT32 IoPollPipeObject(PVOID object);

UINT64 NtQueryFileInfo(UINT64 *args); /* private SVINDOWS loader/runtime hook */
UINT64 NtFileSystemControlSimple(UINT64 *args);
UINT64 NtSetFileSizeSimple(UINT64 *args);
UINT64 NtCreatePipeSimple(UINT64 *args);
UINT64 NtConsoleControlSimple(UINT64 *args);

#endif /* _SVINDOWS_IO_H_ */
