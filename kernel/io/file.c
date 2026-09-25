/*
 * io/file.c - File objects and the handle-based file services.
 *
 * Ties the object manager (Ob) to the I/O subsystem: NtCreateFile produces a
 * File object and a handle, NtReadFile/NtWriteFile operate through that handle,
 * and NtClose releases it (the File object's delete routine frees any cached
 * contents). A single console File object lives in the namespace at
 * \Device\Console; other names are read from the FAT filesystem.
 *
 * Simplifications vs. real NT: disk contents are cached per open File object,
 * mutations use a compact private service, and sharing rules are intentionally
 * less complete than the Windows I/O manager.
 */
#include <svindows/io.h>
#include <svindows/ob.h>
#include <svindows/ex.h>
#include <svindows/ke.h>
#include <svindows/mm.h>
#include <svindows/hal.h>
#include <svindows/console.h>
#include <svindows/rtl.h>
#include <nt/ntobject.h>

typedef enum _FILE_KIND {
    FileKindDisk = 0,
    FileKindConsole,
    FileKindPipe,
} FILE_KIND;

#define IO_PIPE_CAPACITY 65536
typedef struct _IO_PIPE_BUFFER {
    UINT8 *Data;
    SIZE_T ReadPosition;
    SIZE_T WritePosition;
    SIZE_T Count;
    ULONG Readers;
    ULONG Writers;
} IO_PIPE_BUFFER;

typedef struct _FILE_OBJECT {
    FILE_KIND Kind;
    UINT8    *Data;     /* cached contents for disk files (NULL for console) */
    SIZE_T    Size;
    SIZE_T    Capacity;
    SIZE_T    Position;
    ACCESS_MASK Access;
    ULONG     ShareAccess;
    char      Name[64];
    LIST_ENTRY OpenEntry;
    IO_PIPE_BUFFER *Pipe;
    BOOLEAN   PipeWriter;
    UINT16    FatWriteDate;
    UINT16    FatWriteTime;
} FILE_OBJECT;

static POBJECT_TYPE g_file_type;
static POBJECT      g_console_file;
static LIST_ENTRY   g_open_disk_files;

#define FILE_SHARE_READ_LOCAL   0x1u
#define FILE_SHARE_WRITE_LOCAL  0x2u
#define FILE_SHARE_DELETE_LOCAL 0x4u
#define DELETE_ACCESS_LOCAL     0x00010000u

static char IopLower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static BOOLEAN IopSamePath(const char *left, const char *right)
{
    while (*left && IopLower(*left) == IopLower(*right)) {
        left++;
        right++;
    }
    return (BOOLEAN)(*left == 0 && *right == 0);
}

static BOOLEAN IopShareCompatible(const char *name, ACCESS_MASK access,
                                  ULONG share)
{
    PLIST_ENTRY head = &g_open_disk_files;
    for (PLIST_ENTRY entry = head->Flink; entry != head; entry = entry->Flink) {
        FILE_OBJECT *open = CONTAINING_RECORD(entry, FILE_OBJECT, OpenEntry);
        if (!IopSamePath(open->Name, name))
            continue;
        if (((access & GENERIC_READ) &&
             !(open->ShareAccess & FILE_SHARE_READ_LOCAL)) ||
            ((access & GENERIC_WRITE) &&
             !(open->ShareAccess & FILE_SHARE_WRITE_LOCAL)) ||
            ((access & DELETE_ACCESS_LOCAL) &&
             !(open->ShareAccess & FILE_SHARE_DELETE_LOCAL)) ||
            ((open->Access & GENERIC_READ) &&
             !(share & FILE_SHARE_READ_LOCAL)) ||
            ((open->Access & GENERIC_WRITE) &&
             !(share & FILE_SHARE_WRITE_LOCAL)) ||
            ((open->Access & DELETE_ACCESS_LOCAL) &&
             !(share & FILE_SHARE_DELETE_LOCAL)))
            return FALSE;
    }
    return TRUE;
}

void IoConsoleInputCharacter(char c)
{
    ConQueueInputCharacter(c);
}

static void IopFileDelete(POBJECT object)
{
    FILE_OBJECT *f = (FILE_OBJECT *)object;
    if (f->Kind == FileKindDisk && f->OpenEntry.Flink &&
        f->OpenEntry.Flink != &f->OpenEntry) {
        UINT64 flags = KiIrqSave();
        RemoveEntryList(&f->OpenEntry);
        InitializeListHead(&f->OpenEntry);
        KiIrqRestore(flags);
    }
    if (f->Kind == FileKindPipe && f->Pipe) {
        UINT64 flags = KiIrqSave();
        if (f->PipeWriter) {
            if (f->Pipe->Writers) f->Pipe->Writers--;
        } else {
            if (f->Pipe->Readers) f->Pipe->Readers--;
        }
        BOOLEAN release = !f->Pipe->Readers && !f->Pipe->Writers;
        KiIrqRestore(flags);
        if (release) {
            ExFreePool(f->Pipe->Data);
            ExFreePool(f->Pipe);
        }
    }
    if (f->Data)
        ExFreePool(f->Data);
}

void IoInitializeObjects(void)
{
    ConInitialize();
    InitializeListHead(&g_open_disk_files);
    g_file_type = ObCreateObjectType("File", IopFileDelete);

    /* \Device directory + a shared console File object at \Device\Console. */
    struct _OBJECT_DIRECTORY *devdir;
    if (!NT_SUCCESS(ObCreateDirectory(ObRootDirectory, "Device", &devdir)))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "cannot create \\Device");

    if (!NT_SUCCESS(ObCreateObject(g_file_type, sizeof(FILE_OBJECT),
                                   &g_console_file)))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "cannot create console");

    FILE_OBJECT *cf = (FILE_OBJECT *)g_console_file;
    cf->Kind = FileKindConsole;
    cf->Data = NULL;
    cf->Size = 0;
    cf->Capacity = 0;
    cf->Position = 0;
    cf->Access = GENERIC_ALL;
    cf->FatWriteDate = 0;
    cf->FatWriteTime = 0;

    ObInsertObjectByName(devdir, "Console", g_console_file);
    ObDereferenceObject(g_console_file); /* the namespace owns it now */

    KeLog("[io]   \\Device\\Console ready; File object type registered\n");
}

/*
 * NtCreateFile with the real Windows signature (through the argument array):
 *   a0 PHANDLE FileHandle (out)   a1 ACCESS_MASK DesiredAccess
 *   a2 POBJECT_ATTRIBUTES         a3 PIO_STATUS_BLOCK (out)
 *   a4 PLARGE_INTEGER Alloc       a5 ULONG FileAttributes
 *   a6 ULONG ShareAccess          a7 ULONG CreateDisposition
 *   a8 ULONG CreateOptions        a9 PVOID EaBuffer   a10 ULONG EaLength
 * User pointers are validated (and the name captured) before use.
 */
UINT64 NtCreateFile(UINT64 *a)
{
    PHANDLE            out_handle = (PHANDLE)a[0];
    POBJECT_ATTRIBUTES oa         = (POBJECT_ATTRIBUTES)a[2];
    PIO_STATUS_BLOCK   iosb       = (PIO_STATUS_BLOCK)a[3];

    if (!MmProbeForWrite((UINT64)out_handle, sizeof(HANDLE)) ||
        !MmProbeForWrite((UINT64)iosb, sizeof(IO_STATUS_BLOCK)) ||
        !MmProbeForRead((UINT64)oa, sizeof(OBJECT_ATTRIBUTES)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    char name[128];
    if (!MmCaptureUnicodeName((UINT64)oa->ObjectName, name, sizeof(name)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    HANDLE  h = NULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG_PTR info = FILE_OPENED;
    ULONG disposition = (ULONG)a[7];

    /* A named object in the namespace (e.g. the console) opens directly. */
    POBJECT named = NULL;
    if (NT_SUCCESS(ObLookupObjectByName(name, &named))) {
        status = ObCreateHandle(named, GENERIC_ALL, &h);
        ObDereferenceObject(named);
        KeLog("[io]   NtCreateFile('%s') -> handle %p (namespace)\n", name, h);
    } else {
        /* Otherwise pass the complete DOS-style path to FAT. */
        const char *fname = name;
        ACCESS_MASK desired_access = (ACCESS_MASK)a[1];
        ULONG share_access = (ULONG)a[6];

        UINT64 share_flags = KiIrqSave();
        BOOLEAN share_ok = IopShareCompatible(fname, desired_access,
                                               share_access);
        KiIrqRestore(share_flags);
        if (!share_ok) {
            status = STATUS_SHARING_VIOLATION;
            goto create_done;
        }

        void *data;
        SIZE_T size;
        FAT_FIND_DATA fat_info;
        status = FatLoadFile(fname, &data, &size, &fat_info);
        BOOLEAN exists = NT_SUCCESS(status);
        if (exists && disposition == FILE_CREATE) {
            ExFreePool(data);
            status = STATUS_OBJECT_NAME_COLLISION;
        } else {
            BOOLEAN create = !exists &&
                (disposition == FILE_SUPERSEDE ||
                 disposition == FILE_CREATE ||
                 disposition == FILE_OPEN_IF ||
                 disposition == FILE_OVERWRITE_IF);
            BOOLEAN truncate = exists &&
                (disposition == FILE_SUPERSEDE ||
                 disposition == FILE_OVERWRITE ||
                 disposition == FILE_OVERWRITE_IF);
            if (!exists && !create) {
                KeLog("[io]   NtCreateFile: '%s' not found\n", fname);
                status = STATUS_OBJECT_NAME_NOT_FOUND;
            } else if (create || truncate) {
                if (exists)
                    ExFreePool(data);
                status = FatStoreFile(fname, NULL, 0, create);
                if (NT_SUCCESS(status)) {
                    status = FatLoadFile(fname, &data, &size, &fat_info);
                    info = create ? FILE_CREATED : FILE_SUPERSEDED;
                }
            }
        }
        if (NT_SUCCESS(status)) {
            POBJECT obj;
            if (!NT_SUCCESS(ObCreateObject(g_file_type, sizeof(FILE_OBJECT),
                                           &obj))) {
                ExFreePool(data);
                status = STATUS_NO_MEMORY;
            } else {
                FILE_OBJECT *f = (FILE_OBJECT *)obj;
                f->Kind = FileKindDisk;
                f->Data = data;
                f->Size = size;
                f->Capacity = size;
                f->Position = 0;
                f->Access = (ACCESS_MASK)a[1];
                f->ShareAccess = share_access;
                UINT32 name_i = 0;
                for (; fname[name_i] && name_i + 1 < sizeof(f->Name); name_i++)
                    f->Name[name_i] = fname[name_i];
                f->Name[name_i] = 0;
                f->FatWriteDate = fat_info.WriteDate;
                f->FatWriteTime = fat_info.WriteTime;
                InitializeListHead(&f->OpenEntry);
                UINT64 open_flags = KiIrqSave();
                InsertTailList(&g_open_disk_files, &f->OpenEntry);
                KiIrqRestore(open_flags);
                status = ObCreateHandle(obj, GENERIC_ALL, &h);
                if (!NT_SUCCESS(status)) {
                    open_flags = KiIrqSave();
                    RemoveEntryList(&f->OpenEntry);
                    InitializeListHead(&f->OpenEntry);
                    KiIrqRestore(open_flags);
                }
                ObDereferenceObject(obj);
                KeLog("[io]   NtCreateFile('%s') -> handle %p (%lu bytes)\n",
                      fname, h, (unsigned long)size);
            }
        }
    }

create_done:

    if (NT_SUCCESS(status) && out_handle)
        *out_handle = h;
    if (NT_SUCCESS(status) && named == g_console_file) {
        PKTHREAD thread = KeGetCurrentThread();
        PKPROCESS process = thread ? thread->Process : NULL;
        if (process && !process->StandardOutput) {
            process->StandardInput = h;
            process->StandardOutput = h;
            process->StandardError = h;
        }
    }
    if (iosb) {
        iosb->Status = status;
        iosb->Information = NT_SUCCESS(status) ? info : 0;
    }
    return (UINT64)status;
}

/*
 * NtReadFile / NtWriteFile real signature (through the argument array):
 *   a0 HANDLE File   a1 HANDLE Event   a2 PIO_APC_ROUTINE   a3 PVOID ApcContext
 *   a4 PIO_STATUS_BLOCK (out; Information = bytes)   a5 PVOID Buffer
 *   a6 ULONG Length  a7 PLARGE_INTEGER ByteOffset   a8 PULONG Key
 */
UINT64 NtReadFile(UINT64 *a)
{
    HANDLE           handle = (HANDLE)a[0];
    PIO_STATUS_BLOCK iosb   = (PIO_STATUS_BLOCK)a[4];
    void            *buffer = (void *)a[5];
    UINT64           length = (UINT32)a[6]; /* ULONG Length; upper stack half is undefined */

    if (!MmProbeForWrite((UINT64)iosb, sizeof(IO_STATUS_BLOCK)) ||
        !MmProbeForWrite((UINT64)buffer, length))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(handle, 0, g_file_type, &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;

    FILE_OBJECT *f = (FILE_OBJECT *)obj;
    UINT64 n = 0;
    if (f->Kind == FileKindConsole) {
        n = ConRead(buffer, length);
    } else if (f->Kind == FileKindPipe) {
        IO_PIPE_BUFFER *pipe = f->Pipe;
        UINT8 *out = (UINT8 *)buffer;
        while (!n) {
            UINT64 flags = KiIrqSave();
            while (n < length && pipe->Count) {
                out[n++] = pipe->Data[pipe->ReadPosition];
                pipe->ReadPosition = (pipe->ReadPosition + 1) % IO_PIPE_CAPACITY;
                pipe->Count--;
            }
            BOOLEAN eof = pipe->Writers == 0;
            KiIrqRestore(flags);
            if (n || eof || !length)
                break;
            KeYield();
        }
    } else if (f->Position < f->Size) {
        SIZE_T avail = f->Size - f->Position;
        n = length < avail ? length : avail;
        memcpy(buffer, f->Data + f->Position, n);
        f->Position += n;
    }

    ObDereferenceObject(obj);
    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = n;
    }
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtWriteFile(UINT64 *a)
{
    HANDLE           handle = (HANDLE)a[0];
    PIO_STATUS_BLOCK iosb   = (PIO_STATUS_BLOCK)a[4];
    const void      *buffer = (const void *)a[5];
    UINT64           length = (UINT32)a[6]; /* ULONG Length; upper stack half is undefined */

    if (!MmProbeForWrite((UINT64)iosb, sizeof(IO_STATUS_BLOCK)) ||
        !MmProbeForRead((UINT64)buffer, length))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(handle, 0, g_file_type, &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;

    FILE_OBJECT *f = (FILE_OBJECT *)obj;
    UINT64 n = 0;
    if (f->Kind == FileKindConsole) {
        n = ConWrite(buffer, length);
    } else if (f->Kind == FileKindPipe) {
        IO_PIPE_BUFFER *pipe = f->Pipe;
        const UINT8 *input = (const UINT8 *)buffer;
        while (n < length) {
            UINT64 flags = KiIrqSave();
            if (!pipe->Readers) {
                KiIrqRestore(flags);
                ObDereferenceObject(obj);
                iosb->Status = STATUS_PIPE_BROKEN;
                iosb->Information = n;
                return (UINT64)STATUS_PIPE_BROKEN;
            }
            while (n < length && pipe->Count < IO_PIPE_CAPACITY) {
                pipe->Data[pipe->WritePosition] = input[n++];
                pipe->WritePosition = (pipe->WritePosition + 1) % IO_PIPE_CAPACITY;
                pipe->Count++;
            }
            KiIrqRestore(flags);
            if (n < length)
                KeYield();
        }
    } else {
        if (!(f->Access & GENERIC_WRITE)) {
            ObDereferenceObject(obj);
            iosb->Status = STATUS_ACCESS_DENIED;
            iosb->Information = 0;
            return (UINT64)STATUS_ACCESS_DENIED;
        }
        UINT64 end = f->Position + length;
        if (end < f->Position) {
            ObDereferenceObject(obj);
            return (UINT64)STATUS_INVALID_PARAMETER;
        }
        if (end > f->Capacity) {
            SIZE_T capacity = (SIZE_T)((end + PAGE_SIZE - 1) & ~PAGE_MASK);
            UINT8 *expanded = ExAllocatePoolWithTag(NonPagedPool,
                                                     capacity, 'wIoF');
            if (!expanded) {
                ObDereferenceObject(obj);
                return (UINT64)STATUS_NO_MEMORY;
            }
            memset(expanded, 0, capacity);
            if (f->Data && f->Size)
                memcpy(expanded, f->Data, f->Size);
            if (f->Data)
                ExFreePool(f->Data);
            f->Data = expanded;
            f->Capacity = capacity;
        }
        memcpy(f->Data + f->Position, buffer, length);
        f->Position += length;
        if (f->Position > f->Size)
            f->Size = f->Position;
        NTSTATUS write_status = FatStoreFile(f->Name, f->Data, f->Size, FALSE);
        if (!NT_SUCCESS(write_status)) {
            ObDereferenceObject(obj);
            iosb->Status = write_status;
            iosb->Information = 0;
            return (UINT64)write_status;
        }
        n = length;
    }

    ObDereferenceObject(obj);
    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = n;
    }
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtClose(UINT64 *a)
{
    return (UINT64)ObCloseHandle((HANDLE)a[0]);
}

UINT64 NtCreatePipeSimple(UINT64 *a)
{
    PHANDLE read_out = (PHANDLE)a[0];
    PHANDLE write_out = (PHANDLE)a[1];
    if (!MmProbeForWrite((UINT64)read_out, sizeof(HANDLE)) ||
        !MmProbeForWrite((UINT64)write_out, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    IO_PIPE_BUFFER *pipe = ExAllocatePoolWithTag(NonPagedPool,
                                                  sizeof(*pipe), 'pIoB');
    if (!pipe)
        return (UINT64)STATUS_NO_MEMORY;
    memset(pipe, 0, sizeof(*pipe));
    pipe->Data = ExAllocatePoolWithTag(NonPagedPool, IO_PIPE_CAPACITY, 'dIoP');
    if (!pipe->Data) {
        ExFreePool(pipe);
        return (UINT64)STATUS_NO_MEMORY;
    }
    pipe->Readers = pipe->Writers = 1;

    POBJECT read_object, write_object;
    NTSTATUS status = ObCreateObject(g_file_type, sizeof(FILE_OBJECT),
                                     &read_object);
    if (!NT_SUCCESS(status)) {
        ExFreePool(pipe->Data); ExFreePool(pipe);
        return (UINT64)status;
    }
    status = ObCreateObject(g_file_type, sizeof(FILE_OBJECT), &write_object);
    if (!NT_SUCCESS(status)) {
        pipe->Writers = 0;
        ObDereferenceObject(read_object);
        return (UINT64)status;
    }
    FILE_OBJECT *reader = (FILE_OBJECT *)read_object;
    FILE_OBJECT *writer = (FILE_OBJECT *)write_object;
    reader->Kind = writer->Kind = FileKindPipe;
    reader->Pipe = writer->Pipe = pipe;
    writer->PipeWriter = TRUE;
    reader->Access = GENERIC_READ;
    writer->Access = GENERIC_WRITE;

    HANDLE read_handle = NULL, write_handle = NULL;
    status = ObCreateHandle(read_object, GENERIC_READ, &read_handle);
    if (NT_SUCCESS(status))
        status = ObCreateHandle(write_object, GENERIC_WRITE, &write_handle);
    ObDereferenceObject(read_object);
    ObDereferenceObject(write_object);
    if (!NT_SUCCESS(status)) {
        if (read_handle) ObCloseHandle(read_handle);
        return (UINT64)status;
    }
    *read_out = read_handle;
    *write_out = write_handle;
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtConsoleControlSimple(UINT64 *a)
{
    HANDLE handle = (HANDLE)a[0];
    UINT32 operation = (UINT32)a[1];
    UINT64 argument1 = a[2];
    UINT64 *result = (UINT64 *)a[5];

    if (result && !MmProbeForWrite((UINT64)result, sizeof(*result)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (operation == ConsoleControlGetScreenInfo &&
        !MmProbeForWrite(argument1, sizeof(CONSOLE_SCREEN_INFO)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (operation == ConsoleControlGetCursorInfo &&
        !MmProbeForWrite(argument1, sizeof(CONSOLE_CURSOR_INFO)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (operation == ConsoleControlSetCursorInfo &&
        !MmProbeForRead(argument1, sizeof(CONSOLE_CURSOR_INFO)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    if (file->Kind != FileKindConsole) status = STATUS_INVALID_HANDLE;
    else status = ConControl(operation, argument1, a[3], a[4], result);
    ObDereferenceObject(object);
    return (UINT64)status;
}

/* Private minimal handle query used to build Win32 file-information APIs. */
UINT64 NtQueryFileInfo(UINT64 *a)
{
    SVINDOWS_FILE_INFO *out = (SVINDOWS_FILE_INFO *)a[1];
    if (!MmProbeForWrite((UINT64)out, sizeof(*out)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    POBJECT obj;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)a[0], 0,
                                                 g_file_type, &obj);
    if (!NT_SUCCESS(status))
        return (UINT64)status;

    FILE_OBJECT *file = (FILE_OBJECT *)obj;
    out->Size = file->Size;
    out->Position = file->Position;
    out->Attributes = file->Kind == FileKindDisk ? 0x20 : 0;
    out->IsConsole = file->Kind == FileKindConsole;
    out->FatWriteDate = file->FatWriteDate;
    out->FatWriteTime = file->FatWriteTime;
    out->Reserved = 0;
    ObDereferenceObject(obj);
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtSetFilePosition(UINT64 *a)
{
    UINT64 *new_position = (UINT64 *)a[2];
    if (new_position &&
        !MmProbeForWrite((UINT64)new_position, sizeof(*new_position)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    POBJECT obj;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)a[0], 0,
                                                 g_file_type, &obj);
    if (!NT_SUCCESS(status))
        return (UINT64)status;

    FILE_OBJECT *file = (FILE_OBJECT *)obj;
    if (file->Kind != FileKindDisk) {
        ObDereferenceObject(obj);
        return (UINT64)STATUS_INVALID_PARAMETER;
    }
    file->Position = (SIZE_T)a[1];
    if (new_position)
        *new_position = file->Position;
    ObDereferenceObject(obj);
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtSetFileSizeSimple(UINT64 *a)
{
    POBJECT obj;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)a[0], 0,
                                                 g_file_type, &obj);
    if (!NT_SUCCESS(status))
        return (UINT64)status;
    FILE_OBJECT *file = (FILE_OBJECT *)obj;
    SIZE_T size = (SIZE_T)a[1];
    if (file->Kind != FileKindDisk || !(file->Access & GENERIC_WRITE)) {
        ObDereferenceObject(obj);
        return (UINT64)STATUS_ACCESS_DENIED;
    }
    if (size > file->Capacity) {
        SIZE_T capacity = (size + PAGE_SIZE - 1) & ~PAGE_MASK;
        UINT8 *expanded = ExAllocatePoolWithTag(NonPagedPool, capacity, 'sIoF');
        if (!expanded) {
            ObDereferenceObject(obj);
            return (UINT64)STATUS_NO_MEMORY;
        }
        memset(expanded, 0, capacity);
        if (file->Data && file->Size)
            memcpy(expanded, file->Data, file->Size);
        if (file->Data) ExFreePool(file->Data);
        file->Data = expanded;
        file->Capacity = capacity;
    }
    file->Size = size;
    if (file->Position > size) file->Position = size;
    status = FatStoreFile(file->Name, file->Data, file->Size, FALSE);
    ObDereferenceObject(obj);
    return (UINT64)status;
}

static BOOLEAN IopCaptureAsciiPath(UINT64 address, char *path, SIZE_T capacity);

/* SVINDOWS-private primitive used by kernel32's FindFirst/FindNext facade until
 * directory File objects and the full NtQueryDirectoryFile contract exist. */
UINT64 NtEnumerateRootFiles(UINT64 *a)
{
    FAT_FIND_DATA *out = (FAT_FIND_DATA *)a[1];
    if (!MmProbeForWrite((UINT64)out, sizeof(*out)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    NTSTATUS status = FatEnumerateRoot((UINT32)a[0], out);
    if (NT_SUCCESS(status))
        KeLog("[io]   enumerate root[%u] -> %s (%u bytes)\n",
              (unsigned)a[0], out->Name, (unsigned)out->Size);
    return (UINT64)status;
}

UINT64 NtEnumerateDirectorySimple(UINT64 *a)
{
    char path[128];
    FAT_FIND_DATA *out = (FAT_FIND_DATA *)a[2];
    if (!IopCaptureAsciiPath(a[0], path, sizeof(path)) ||
        !MmProbeForWrite((UINT64)out, sizeof(*out)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    return (UINT64)FatEnumerateDirectory(path, (UINT32)a[1], out);
}

static BOOLEAN IopCaptureAsciiPath(UINT64 address, char *path, SIZE_T capacity)
{
    if (!address || !capacity)
        return FALSE;
    for (SIZE_T i = 0; i + 1 < capacity; i++) {
        if (!MmProbeForRead(address + i, 1))
            return FALSE;
        path[i] = *(const char *)(address + i);
        if (!path[i])
            return TRUE;
    }
    path[capacity - 1] = 0;
    return FALSE;
}

/* SVINDOWS-private path service: 1 delete, 2 rename, 3 mkdir, 4 rmdir,
 * 5 query attributes (returned as a non-negative service result). */
UINT64 NtFileSystemControlSimple(UINT64 *a)
{
    char first[128], second[128];
    if (!IopCaptureAsciiPath(a[1], first, sizeof(first)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    switch ((ULONG)a[0]) {
    case 1:
        for (PLIST_ENTRY entry = g_open_disk_files.Flink;
             entry != &g_open_disk_files; entry = entry->Flink) {
            FILE_OBJECT *open = CONTAINING_RECORD(entry, FILE_OBJECT,
                                                   OpenEntry);
            if (IopSamePath(open->Name, first) &&
                !(open->ShareAccess & FILE_SHARE_DELETE_LOCAL))
                return (UINT64)STATUS_SHARING_VIOLATION;
        }
        return (UINT64)FatDeleteFile(first);
    case 2:
        if (!IopCaptureAsciiPath(a[2], second, sizeof(second)))
            return (UINT64)STATUS_ACCESS_VIOLATION;
        for (PLIST_ENTRY entry = g_open_disk_files.Flink;
             entry != &g_open_disk_files; entry = entry->Flink) {
            FILE_OBJECT *open = CONTAINING_RECORD(entry, FILE_OBJECT,
                                                   OpenEntry);
            if (IopSamePath(open->Name, first) &&
                !(open->ShareAccess & FILE_SHARE_DELETE_LOCAL))
                return (UINT64)STATUS_SHARING_VIOLATION;
        }
        return (UINT64)FatRenamePath(first, second);
    case 3:
        return (UINT64)FatCreateDirectory(first);
    case 4:
        return (UINT64)FatRemoveDirectory(first);
    case 5: {
        FAT_FIND_DATA info;
        NTSTATUS status = FatQueryPath(first, &info);
        return NT_SUCCESS(status) ? (UINT64)info.Attributes : (UINT64)status;
    }
    default:
        return (UINT64)STATUS_INVALID_PARAMETER;
    }
}
