/*
 * io/File.c - File objects and the handle-based file services.
 *
 * Ties the object manager (Ob) to the I/O subsystem: NtCreateFile produces a
 * File object and a handle, NtReadFile/NtWriteFile operate through that handle,
 * and NtClose releases it (the File object's delete routine frees any cached
 * contents). A single console File object lives in the namespace at
 * \Device\Console; other names are read from the FAT filesystem.
 *
 * Disk contents and metadata live in shared vnodes; each open File object owns
 * only its access mask and current byte offset. Native file-information calls
 * perform the normal handle-based seek, truncate, rename and delete operations.
 */
#include <svindows/Io.h>
#include "Internal.h"
#include <svindows/Ob.h>
#include <svindows/Ex.h>
#include <svindows/Ke.h>
#include <svindows/Mm.h>
#include <svindows/Hal.h>
#include <svindows/Console.h>
#include <svindows/Linux.h>
#include <svindows/Rtl.h>
#include <nt/Ntobject.h>
static IO_STATISTICS g_io_statistics;
static void io_record_transfer(BOOLEAN write,UINT64 bytes)
{
    __atomic_add_fetch(write?&g_io_statistics.WriteBytes:&g_io_statistics.ReadBytes,bytes,__ATOMIC_RELAXED);
    __atomic_add_fetch(write?&g_io_statistics.WriteOperations:&g_io_statistics.ReadOperations,1,__ATOMIC_RELAXED);
}
void IoQueryStatistics(IO_STATISTICS *statistics)
{
    statistics->ReadBytes=__atomic_load_n(&g_io_statistics.ReadBytes,__ATOMIC_RELAXED);
    statistics->WriteBytes=__atomic_load_n(&g_io_statistics.WriteBytes,__ATOMIC_RELAXED);
    statistics->ReadOperations=__atomic_load_n(&g_io_statistics.ReadOperations,__ATOMIC_RELAXED);
    statistics->WriteOperations=__atomic_load_n(&g_io_statistics.WriteOperations,__ATOMIC_RELAXED);
}

typedef enum _FILE_KIND {
    FileKindDisk = 0,
    FileKindConsole,
    FileKindPipe,
    FileKindDirectory,
    FileKindCharDevice,
    FileKindProc,
} FILE_KIND;

typedef struct _IO_VNODE {
    LIST_ENTRY Entry;
    ULONG     References;
    UINT8    *Data;
    SIZE_T    Size;
    SIZE_T    Capacity;
    UINT32    Attributes;
    UINT16    FatWriteDate;
    UINT16    FatWriteTime;
    BOOLEAN   DeletePending;
    char      Name[260];
} IO_VNODE;

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
    SIZE_T    Position;
    ACCESS_MASK Access;
    ULONG     ShareAccess;
    LIST_ENTRY OpenEntry;
    IO_VNODE *Vnode;
    IO_PIPE_BUFFER *Pipe;
    BOOLEAN   PipeWriter;
    POBJECT   CompletionPort;
    ULONG_PTR CompletionKey;
    UINT64    CharTag;
    void     *CharContext;
    const IO_CHAR_DEVICE_OPS *CharOps;
    UINT8    *ProcData;
    SIZE_T    ProcSize;
} FILE_OBJECT;

static POBJECT_TYPE g_file_type;
typedef struct _IO_CONSOLE_READ_PIN {
    struct _IO_CONSOLE_READ_PIN *Next;
    PKTHREAD Thread;
    POBJECT Object;
} IO_CONSOLE_READ_PIN;
static IO_CONSOLE_READ_PIN *g_console_read_pins;
static IO_CONSOLE_READ_PIN *io_pin_console_read(POBJECT object)
{
    IO_CONSOLE_READ_PIN *pin=ExAllocatePoolWithTag(NonPagedPool,sizeof(*pin),'PirC');
    if(!pin)return NULL;
    pin->Thread=KeGetCurrentThread();pin->Object=object;
    UINT64 flags=KiIrqSave();pin->Next=g_console_read_pins;g_console_read_pins=pin;KiIrqRestore(flags);
    return pin;
}
static void io_unpin_console_read(IO_CONSOLE_READ_PIN *pin)
{
    if(!pin)return;
    UINT64 flags=KiIrqSave();
    IO_CONSOLE_READ_PIN **link=&g_console_read_pins;
    while(*link&&*link!=pin)link=&(*link)->Next;
    if(*link){*link=pin->Next;ExFreePool(pin);}KiIrqRestore(flags);
}
PVOID IoPinThreadConsoleObject(PVOID referenced_object)
{return io_pin_console_read(referenced_object);}
void IoUnpinThreadConsoleObject(PVOID pin)
{io_unpin_console_read(pin);}
void IoCleanupThreadConsoleReads(PKTHREAD thread)
{
    UINT64 flags=KiIrqSave();IO_CONSOLE_READ_PIN **link=&g_console_read_pins;
    while(*link){IO_CONSOLE_READ_PIN *pin=*link;if(pin->Thread!=thread){link=&pin->Next;continue;}
        *link=pin->Next;ObDereferenceObject(pin->Object);ExFreePool(pin);}
    KiIrqRestore(flags);
}
static NTSTATUS io_native_console_permission(HANDLE handle,ACCESS_MASK needed)
{
    POBJECT object;
    NTSTATUS status=ObReferenceObjectByHandle(handle,needed,g_file_type,&object);
    if(NT_SUCCESS(status))ObDereferenceObject(object);
    return status;
}

NTSTATUS IoReferenceFileData(HANDLE handle, PVOID *file_object,
                             const UINT8 **data, SIZE_T *size)
{
    if (!file_object || !data || !size)
        return STATUS_INVALID_PARAMETER;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type,
                                                 &object);
    if (!NT_SUCCESS(status))
        return status;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    if (file->Kind != FileKindDisk || !file->Vnode) {
        ObDereferenceObject(object);
        return STATUS_INVALID_HANDLE;
    }
    *file_object = object;
    *data = file->Vnode->Data;
    *size = file->Vnode->Size;
    return STATUS_SUCCESS;
}

/* Kernel clients such as the DOS personality use the same process handles as
 * native programs. No user-pointer probe is appropriate for their buffers. */
static NTSTATUS io_read_handle_kernel(HANDLE handle, void *buffer,
                                      SIZE_T length, SIZE_T *read_out,
                                      BOOLEAN wait)
{
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type,
                                                &object);
    if (!NT_SUCCESS(status)) return status;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    SIZE_T n = 0;
    if (file->Kind == FileKindConsole)
        n = wait ? ConRead(buffer, length) : ConTryRead(buffer, length);
    else if (file->Kind == FileKindCharDevice && file->CharOps) {
        if (file->CharOps->ReadResult) {
            IO_CONSOLE_READ_PIN *pin=wait&&file->CharTag==IO_CHAR_DEVICE_TAG_CONSOLE?io_pin_console_read(object):NULL;
            if(wait&&file->CharTag==IO_CHAR_DEVICE_TAG_CONSOLE&&!pin){ObDereferenceObject(object);return STATUS_NO_MEMORY;}
            status = file->CharOps->ReadResult(file->CharContext, buffer, length, wait, &n);
            io_unpin_console_read(pin);
        } else if (wait && file->CharOps->Read)
            n = file->CharOps->Read(file->CharContext, buffer, length);
        else if (!wait && file->CharOps->TryRead)
            n = file->CharOps->TryRead(file->CharContext, buffer, length);
    }
    else if (file->Kind == FileKindPipe) {
        IO_PIPE_BUFFER *pipe = file->Pipe;
        while (!n && length) {
            UINT64 irq = KiIrqSave();
            while (n < length && pipe->Count) {
                ((UINT8 *)buffer)[n++] = pipe->Data[pipe->ReadPosition];
                pipe->ReadPosition = (pipe->ReadPosition + 1) % IO_PIPE_CAPACITY;
                pipe->Count--;
            }
            BOOLEAN eof = !pipe->Writers;
            KiIrqRestore(irq);
            if (!n && !eof && wait) KeYield(); else break;
        }
    } else if (file->Kind == FileKindDisk && file->Vnode &&
               file->Position < file->Vnode->Size) {
        SIZE_T available = file->Vnode->Size - file->Position;
        n = length < available ? length : available;
        memcpy(buffer, file->Vnode->Data + file->Position, n);
        file->Position += n;
    }
    *read_out = n;
    io_record_transfer(FALSE,n);
    ObDereferenceObject(object);
    return status;
}

NTSTATUS IoReadHandleKernel(HANDLE handle, void *buffer, SIZE_T length,
                            SIZE_T *read_out)
{
    return io_read_handle_kernel(handle, buffer, length, read_out, TRUE);
}

NTSTATUS IoTryReadHandleKernel(HANDLE handle, void *buffer, SIZE_T length,
                               SIZE_T *read_out)
{
    return io_read_handle_kernel(handle, buffer, length, read_out, FALSE);
}

NTSTATUS IoHandleInputAvailableKernel(HANDLE handle, BOOLEAN *available)
{
    if (!available) return STATUS_INVALID_PARAMETER;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type,
                                                &object);
    if (!NT_SUCCESS(status)) return status;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    if (file->Kind == FileKindConsole)
        *available = ConInputAvailable();
    else if (file->Kind == FileKindCharDevice && file->CharOps &&
             file->CharOps->InputAvailable)
        *available = file->CharOps->InputAvailable(file->CharContext);
    else if (file->Kind == FileKindPipe) {
        UINT64 irq = KiIrqSave();
        *available = file->Pipe->Count != 0;
        KiIrqRestore(irq);
    } else
        *available = file->Kind == FileKindDisk && file->Vnode &&
                     file->Position < file->Vnode->Size;
    ObDereferenceObject(object);
    return STATUS_SUCCESS;
}

NTSTATUS IoFlushInputHandleKernel(HANDLE handle)
{
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type,
                                                &object);
    if (!NT_SUCCESS(status)) return status;

    FILE_OBJECT *file = (FILE_OBJECT *)object;
    if (file->Kind == FileKindConsole) {
        ConFlushInput();
    } else if (file->Kind == FileKindCharDevice && file->CharOps &&
               file->CharOps->TryRead) {
        UINT8 discard[64];
        while (file->CharOps->TryRead(file->CharContext, discard,
                                      sizeof(discard)) != 0) {
            /* Drain only data already buffered by the character device. */
        }
    } else if (file->Kind == FileKindPipe && file->Pipe) {
        UINT64 irq = KiIrqSave();
        file->Pipe->ReadPosition = file->Pipe->WritePosition;
        file->Pipe->Count = 0;
        KiIrqRestore(irq);
    }
    /* Disk-backed redirected stdin has no independent input queue to flush;
     * its file position must remain unchanged. */

    ObDereferenceObject(object);
    return STATUS_SUCCESS;
}

NTSTATUS IoWriteHandleKernel(HANDLE handle, const void *buffer, SIZE_T length,
                             SIZE_T *written_out)
{
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type,
                                                &object);
    if (!NT_SUCCESS(status)) return status;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    SIZE_T n = 0;
    if (file->Kind == FileKindConsole) n = ConWrite(buffer, length);
    else if (file->Kind == FileKindCharDevice && file->CharOps &&
             (file->CharOps->Write || file->CharOps->WriteResult)) {
        if (file->CharOps->WriteResult)
            status = file->CharOps->WriteResult(file->CharContext, buffer, length, &n);
        else n = file->CharOps->Write(file->CharContext, buffer, length);
    }
    else if (file->Kind == FileKindPipe) {
        IO_PIPE_BUFFER *pipe = file->Pipe;
        while (n < length) {
            UINT64 irq = KiIrqSave();
            if (!pipe->Readers) {status = STATUS_PIPE_BROKEN;KiIrqRestore(irq);break;}
            while (n < length && pipe->Count < IO_PIPE_CAPACITY) {
                pipe->Data[pipe->WritePosition] = ((const UINT8 *)buffer)[n++];
                pipe->WritePosition = (pipe->WritePosition + 1) % IO_PIPE_CAPACITY;
                pipe->Count++;
            }
            KiIrqRestore(irq);
            if (n < length) KeYield();
        }
    } else if (file->Kind == FileKindDisk && file->Vnode &&
               (file->Access & GENERIC_WRITE)) {
        IO_VNODE *vnode = file->Vnode;
        if (length > (SIZE_T)-1 - file->Position) {
            status = STATUS_INVALID_PARAMETER;
        } else {
            SIZE_T end = file->Position + length;
            if (end > vnode->Capacity) {
                SIZE_T capacity = (end + PAGE_SIZE - 1) & ~PAGE_MASK;
                UINT8 *expanded = ExAllocatePoolWithTag(NonPagedPool,
                                                         capacity, 'wIoF');
                if (!expanded) status = STATUS_NO_MEMORY;
                else {
                    memset(expanded, 0, capacity);
                    if (vnode->Data && vnode->Size)
                        memcpy(expanded, vnode->Data, vnode->Size);
                    if (vnode->Data) ExFreePool(vnode->Data);
                    vnode->Data = expanded;
                    vnode->Capacity = capacity;
                }
            }
            if (NT_SUCCESS(status)) {
                memcpy(vnode->Data + file->Position, buffer, length);
                file->Position = end;
                if (end > vnode->Size) vnode->Size = end;
                status = FatStoreFile(vnode->Name, vnode->Data,
                                      vnode->Size, FALSE);
                if (NT_SUCCESS(status)) n = length;
            }
        }
    } else status = STATUS_NOT_SUPPORTED;
    *written_out = n;
    if(n||NT_SUCCESS(status))io_record_transfer(TRUE,n);
    ObDereferenceObject(object);
    return status;
}

NTSTATUS IoConsoleControlHandleKernel(HANDLE handle, UINT32 operation,
                                      UINT64 argument1, UINT64 argument2,
                                      UINT64 argument3, UINT64 *result)
{
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type,
                                                &object);
    if (!NT_SUCCESS(status)) return status;
    FILE_OBJECT *file = object;
    if (file->Kind == FileKindConsole)
        status = ConControl(operation, argument1, argument2, argument3, result);
    else if (file->Kind == FileKindCharDevice && file->CharOps &&
             file->CharOps->ConsoleControl)
        status = file->CharOps->ConsoleControl(file->CharContext, operation,
            argument1, argument2, argument3, result);
    else
        status = STATUS_INVALID_HANDLE;
    ObDereferenceObject(object);
    return status;
}
static POBJECT      g_console_file;
static LIST_ENTRY   g_open_disk_files;
static LIST_ENTRY   g_vnodes;

#define FILE_SHARE_READ_LOCAL   0x1u
#define FILE_SHARE_WRITE_LOCAL  0x2u
#define FILE_SHARE_DELETE_LOCAL 0x4u
#define DELETE_ACCESS_LOCAL     0x00010000u
#define FILE_READ_DATA_LOCAL    0x00000001u
#define FILE_WRITE_DATA_LOCAL   0x00000002u
#define FILE_APPEND_DATA_LOCAL  0x00000004u
#define FILE_EXECUTE_LOCAL      0x00000020u

static BOOLEAN IopWantsRead(ACCESS_MASK access)
{
    return (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA_LOCAL |
                      FILE_EXECUTE_LOCAL)) != 0;
}

static BOOLEAN IopWantsWrite(ACCESS_MASK access)
{
    return (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA_LOCAL |
                      FILE_APPEND_DATA_LOCAL)) != 0;
}

static NTSTATUS IopCheckShareAccess(const char *name, ACCESS_MASK access,
                                    ULONG share)
{
    for (PLIST_ENTRY entry = g_vnodes.Flink;
         entry != &g_vnodes; entry = entry->Flink) {
        IO_VNODE *vnode = CONTAINING_RECORD(entry, IO_VNODE, Entry);
        if (vnode->DeletePending &&
            (IoPathSame(vnode->Name, name) ||
             ((vnode->Attributes & 0x10) &&
              IoPathIsWithin(vnode->Name, name))))
            return STATUS_DELETE_PENDING;
    }
    PLIST_ENTRY head = &g_open_disk_files;
    for (PLIST_ENTRY entry = head->Flink; entry != head; entry = entry->Flink) {
        FILE_OBJECT *open = CONTAINING_RECORD(entry, FILE_OBJECT, OpenEntry);
        if (!open->Vnode || !IoPathSame(open->Vnode->Name, name))
            continue;
        if ((IopWantsRead(access) &&
             !(open->ShareAccess & FILE_SHARE_READ_LOCAL)) ||
            (IopWantsWrite(access) &&
             !(open->ShareAccess & FILE_SHARE_WRITE_LOCAL)) ||
            ((access & DELETE_ACCESS_LOCAL) &&
             !(open->ShareAccess & FILE_SHARE_DELETE_LOCAL)) ||
            (IopWantsRead(open->Access) &&
             !(share & FILE_SHARE_READ_LOCAL)) ||
            (IopWantsWrite(open->Access) &&
             !(share & FILE_SHARE_WRITE_LOCAL)) ||
            ((open->Access & DELETE_ACCESS_LOCAL) &&
             !(share & FILE_SHARE_DELETE_LOCAL)))
            return STATUS_SHARING_VIOLATION;
    }
    return STATUS_SUCCESS;
}

static IO_VNODE *IopAcquireVnode(const char *name, UINT8 *data, SIZE_T size,
                                 const FAT_FIND_DATA *info, BOOLEAN replace)
{
    UINT64 flags = KiIrqSave();
    for (PLIST_ENTRY entry = g_vnodes.Flink;
         entry != &g_vnodes; entry = entry->Flink) {
        IO_VNODE *vnode = CONTAINING_RECORD(entry, IO_VNODE, Entry);
        if (!IoPathSame(vnode->Name, name))
            continue;
        vnode->References++;
        KiIrqRestore(flags);
        if (replace) {
            if (vnode->Data) ExFreePool(vnode->Data);
            vnode->Data = data;
            vnode->Size = size;
            vnode->Capacity = size;
            vnode->Attributes = info ? info->Attributes : 0x20;
            vnode->FatWriteDate = info ? info->WriteDate : 0;
            vnode->FatWriteTime = info ? info->WriteTime : 0;
        } else if (data) {
            ExFreePool(data);
        }
        return vnode;
    }
    KiIrqRestore(flags);

    IO_VNODE *vnode = ExAllocatePoolWithTag(NonPagedPool, sizeof(*vnode),
                                             'nVoI');
    if (!vnode) {
        if (data) ExFreePool(data);
        return NULL;
    }
    memset(vnode, 0, sizeof(*vnode));
    vnode->References = 1;
    vnode->Data = data;
    vnode->Size = size;
    vnode->Capacity = size;
    vnode->Attributes = info ? info->Attributes : 0x20;
    vnode->FatWriteDate = info ? info->WriteDate : 0;
    vnode->FatWriteTime = info ? info->WriteTime : 0;
    UINT32 i = 0;
    for (; name[i] && i + 1 < sizeof(vnode->Name); i++)
        vnode->Name[i] = name[i];
    vnode->Name[i] = 0;
    InitializeListHead(&vnode->Entry);
    flags = KiIrqSave();
    InsertTailList(&g_vnodes, &vnode->Entry);
    KiIrqRestore(flags);
    return vnode;
}

static void IopReleaseVnode(IO_VNODE *vnode)
{
    if (!vnode) return;
    UINT64 flags = KiIrqSave();
    if (--vnode->References) {
        KiIrqRestore(flags);
        return;
    }
    RemoveEntryList(&vnode->Entry);
    KiIrqRestore(flags);
    if (vnode->DeletePending) {
        NTSTATUS status = (vnode->Attributes & 0x10)
                        ? FatRemoveDirectory(vnode->Name)
                        : FatDeleteFile(vnode->Name);
        if (!NT_SUCCESS(status))
            KeLog("[io]   delete-pending cleanup failed for '%s': 0x%08x\n",
                  vnode->Name, (unsigned)status);
    }
    if (vnode->Data) ExFreePool(vnode->Data);
    ExFreePool(vnode);
}

/* Kernel personalities use the same vnodes and handle table as NtCreateFile.
 * The input is a DOS-style path already translated by the caller. */
NTSTATUS IoOpenFileKernel(const char *path, ULONG access, BOOLEAN create,
                          BOOLEAN exclusive, BOOLEAN truncate,
                          HANDLE *handle_out)
{
    if (!handle_out) return STATUS_INVALID_PARAMETER;
    *handle_out = NULL;
    char name[260];
    if (!IoCanonicalizeDosPathKernel(path, name))
        return STATUS_OBJECT_NAME_INVALID;
    /* The Linux proc provider also serves its two machine information files
     * to native callers. Keep the snapshot on the ordinary file object so
     * Win32 reads and seeks use the same handle semantics as disk files. */
    UINT8 proc_buffer[8192];
    SIZE_T proc_length = 0;
    NTSTATUS proc_status = LinuxProcSnapshot(name, (char *)proc_buffer,
                                               sizeof(proc_buffer),
                                               &proc_length);
    if (NT_SUCCESS(proc_status)) {
        if (create || truncate) return STATUS_ACCESS_DENIED;
        POBJECT proc_object;
        proc_status = ObCreateObject(g_file_type, sizeof(FILE_OBJECT),
                                     &proc_object);
        if (!NT_SUCCESS(proc_status)) return proc_status;
        FILE_OBJECT *proc_file = (FILE_OBJECT *)proc_object;
        proc_file->Kind = FileKindProc;
        proc_file->Access = access;
        proc_file->ProcData = ExAllocatePoolWithTag(NonPagedPool,
                                                     proc_length ? proc_length : 1,
                                                     'cPrF');
        if (!proc_file->ProcData) {
            ObDereferenceObject(proc_object);
            return STATUS_NO_MEMORY;
        }
        memcpy(proc_file->ProcData, proc_buffer, proc_length);
        proc_file->ProcSize = proc_length;
        InitializeListHead(&proc_file->OpenEntry);
        proc_status = ObCreateHandle(proc_object, GENERIC_ALL, handle_out);
        ObDereferenceObject(proc_object);
        return proc_status;
    }
    if (proc_status != STATUS_OBJECT_NAME_NOT_FOUND) return proc_status;
    UINT64 irq = KiIrqSave();
    NTSTATUS status = IopCheckShareAccess(name, access,
        FILE_SHARE_READ_LOCAL | FILE_SHARE_WRITE_LOCAL | FILE_SHARE_DELETE_LOCAL);
    KiIrqRestore(irq);
    if (!NT_SUCCESS(status)) return status;

    FAT_FIND_DATA info;
    status = FatQueryPath(name, &info);
    BOOLEAN exists = NT_SUCCESS(status);
    if (exists && (info.Attributes & 0x10u)) return STATUS_ACCESS_DENIED;
    if (exists && create && exclusive) return STATUS_OBJECT_NAME_COLLISION;
    if (!exists && !create) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (!exists || truncate) {
        status = FatStoreFile(name, NULL, 0, !exists);
        if (!NT_SUCCESS(status)) return status;
    }
    void *data = NULL;
    SIZE_T size = 0;
    status = FatLoadFile(name, &data, &size, &info);
    if (!NT_SUCCESS(status)) return status;
    IO_VNODE *vnode = IopAcquireVnode(name, data, size, &info, truncate);
    if (!vnode) return STATUS_NO_MEMORY;

    POBJECT object;
    status = ObCreateObject(g_file_type, sizeof(FILE_OBJECT), &object);
    if (!NT_SUCCESS(status)) {
        IopReleaseVnode(vnode);
        return status;
    }
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    file->Kind = FileKindDisk;
    file->Access = access;
    file->ShareAccess = FILE_SHARE_READ_LOCAL | FILE_SHARE_WRITE_LOCAL |
                        FILE_SHARE_DELETE_LOCAL;
    file->Vnode = vnode;
    InitializeListHead(&file->OpenEntry);
    irq = KiIrqSave();
    InsertTailList(&g_open_disk_files, &file->OpenEntry);
    KiIrqRestore(irq);
    status = ObCreateHandle(object, GENERIC_ALL, handle_out);
    ObDereferenceObject(object);
    return status;
}

NTSTATUS IoSeekHandleKernel(HANDLE handle, INT64 offset, UINT32 whence,
                            UINT64 *position_out)
{
    if (!position_out || whence > 2) return STATUS_INVALID_PARAMETER;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type,
                                                 &object);
    if (!NT_SUCCESS(status)) return status;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    if (file->Kind != FileKindDisk || !file->Vnode) {
        status = STATUS_INVALID_PARAMETER;
    } else {
        UINT64 base = whence == 0 ? 0 : whence == 1 ? file->Position :
                      file->Vnode->Size;
        UINT64 next;
        if (offset < 0) {
            UINT64 distance = 0 - (UINT64)offset;
            if (distance > base) status = STATUS_INVALID_PARAMETER;
            next = base - distance;
        } else {
            next = base + (UINT64)offset;
            if (next < base) status = STATUS_INVALID_PARAMETER;
        }
        if (NT_SUCCESS(status)) {
            file->Position = (SIZE_T)next;
            *position_out = next;
        }
    }
    ObDereferenceObject(object);
    return status;
}

NTSTATUS IoQueryFileStatKernel(HANDLE handle, IO_KERNEL_FILE_STAT *out)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type,
                                                 &object);
    if (!NT_SUCCESS(status)) return status;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    IO_VNODE *vnode = file->Vnode;
    memset(out, 0, sizeof(*out));
    out->Kind = file->Kind;
    if (vnode) {
        out->Size = vnode->Size;
        out->Attributes = vnode->Attributes;
        out->FatWriteDate = vnode->FatWriteDate;
        out->FatWriteTime = vnode->FatWriteTime;
        FAT_FIND_DATA current_info;
        if (NT_SUCCESS(FatQueryPath(vnode->Name, &current_info))) {
            out->FatWriteDate = current_info.WriteDate;
            out->FatWriteTime = current_info.WriteTime;
        }
        UINT64 hash = 14695981039346656037ULL;
        for (const char *p = vnode->Name; *p; p++) {
            hash ^= (UINT8)IoPathLower(*p);
            hash *= 1099511628211ULL;
        }
        out->FileId = hash ? hash : 1;
    } else {
        out->FileId = file->Kind + 1;
    }
    ObDereferenceObject(object);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Character-device file objects                                       */
/* ------------------------------------------------------------------ */

NTSTATUS IoCreateCharDeviceFile(UINT64 tag, void *context,
                                const IO_CHAR_DEVICE_OPS *ops, HANDLE *out)
{
    PKTHREAD thread = KeGetCurrentThread();
    return IoCreateCharDeviceFileInTable(thread && thread->Process ?
        thread->Process->HandleTable : ObGetBootstrapHandleTable(), tag, context,
        ops, GENERIC_ALL, out);
}

NTSTATUS IoCreateCharDeviceFileInTable(POB_HANDLE_TABLE table, UINT64 tag,
    void *context, const IO_CHAR_DEVICE_OPS *ops, ACCESS_MASK access, HANDLE *out)
{
    if (!out || !ops || !ops->Close) return STATUS_INVALID_PARAMETER;
    *out = NULL;
    POBJECT object;
    NTSTATUS status = ObCreateObject(g_file_type, sizeof(FILE_OBJECT), &object);
    if (!NT_SUCCESS(status)) return status;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    file->Kind = FileKindCharDevice;
    file->Access = GENERIC_READ | GENERIC_WRITE;
    file->CharTag = tag;
    file->CharContext = context;
    file->CharOps = ops;
    status = ObCreateHandleInTable(table, object, access, out);
    if (!NT_SUCCESS(status)) {
        /* Handle publication transfers the context. On failure it remains
         * with the creator; deleting the unexposed File must not close it. */
        file->CharOps = NULL;
        file->CharContext = NULL;
    }
    ObDereferenceObject(object);
    return status;
}

NTSTATUS IoQueryCharDeviceFileInTable(POB_HANDLE_TABLE table, HANDLE handle,
                                      UINT64 *tag, void **context)
{
    if (!tag || !context) return STATUS_INVALID_PARAMETER;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandleInTable(table, handle, 0,
                                                        g_file_type, &object,
                                                        NULL);
    if (!NT_SUCCESS(status)) return status;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    if (file->Kind != FileKindCharDevice || !file->CharOps) {
        ObDereferenceObject(object);
        return STATUS_INVALID_HANDLE;
    }
    *tag = file->CharTag;
    *context = file->CharContext;
    ObDereferenceObject(object);
    return STATUS_SUCCESS;
}

NTSTATUS IoQueryCharDeviceFile(HANDLE handle, UINT64 *tag, void **context)
{
    if (!tag || !context) return STATUS_INVALID_PARAMETER;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type,
                                                 &object);
    if (!NT_SUCCESS(status)) return status;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    if (file->Kind != FileKindCharDevice || !file->CharOps) {
        ObDereferenceObject(object);
        return STATUS_INVALID_HANDLE;
    }
    *tag = file->CharTag;
    *context = file->CharContext;
    ObDereferenceObject(object);
    return STATUS_SUCCESS;
}

void IoPrepareFileForWindows(HANDLE handle)
{
    POBJECT object;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(handle, 0, g_file_type,
                                              &object)))
        return;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    if (file->Kind == FileKindCharDevice && file->CharOps &&
        file->CharOps->PrepareWindows)
        file->CharOps->PrepareWindows(file->CharContext);
    ObDereferenceObject(object);
}

NTSTATUS IoOpenConsoleKernel(HANDLE *out)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    *out = NULL;
    PKTHREAD thread = KeGetCurrentThread();
    if (thread && thread->Process && thread->Process->NativeConsole)
        return ConOpenProcessConsole(FALSE, out);
    if (!g_console_file) return STATUS_DEVICE_NOT_READY;
    return ObCreateHandle(g_console_file, GENERIC_ALL, out);
}

NTSTATUS IoIsConsoleFileInTable(POB_HANDLE_TABLE table, HANDLE handle)
{
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandleInTable(table, handle, 0,
                                                        g_file_type, &object,
                                                        NULL);
    if (!NT_SUCCESS(status)) return STATUS_INVALID_HANDLE;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    status = (file->Kind == FileKindConsole ||
              (file->Kind == FileKindCharDevice && file->CharTag == IO_CHAR_DEVICE_TAG_CONSOLE)) ? STATUS_SUCCESS :
                                          STATUS_INVALID_HANDLE;
    ObDereferenceObject(object);
    return status;
}

NTSTATUS IoIsConsoleFile(HANDLE handle)
{
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type,
                                                 &object);
    if (!NT_SUCCESS(status)) return STATUS_INVALID_HANDLE;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    status = (file->Kind == FileKindConsole ||
              (file->Kind == FileKindCharDevice && file->CharTag == IO_CHAR_DEVICE_TAG_CONSOLE)) ? STATUS_SUCCESS :
                                          STATUS_INVALID_HANDLE;
    ObDereferenceObject(object);
    return status;
}

void IoConsoleInputCharacter(char c)
{
    ConQueueInputCharacter(c);
}

static void IopFileDelete(POBJECT object)
{
    FILE_OBJECT *f = (FILE_OBJECT *)object;
    if (f->CompletionPort) {
        ObDereferenceObject(f->CompletionPort);
        f->CompletionPort = NULL;
    }
    BOOLEAN disk_backed = f->Kind == FileKindDisk ||
                          f->Kind == FileKindDirectory;
    if ((f->Kind == FileKindDisk || f->Kind == FileKindDirectory) &&
        f->OpenEntry.Flink &&
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
    if (f->Kind == FileKindCharDevice && f->CharOps) {
        f->CharOps->Close(f->CharContext);
        return;
    }
    if (f->Kind == FileKindProc) {
        if (f->ProcData) ExFreePool(f->ProcData);
        return;
    }
    IopReleaseVnode(f->Vnode);
    if (disk_backed) {
        NTSTATUS status = FatFlush();
        if (!NT_SUCCESS(status))
            KeLog("[io]   close-time filesystem flush failed: 0x%08x\n",
                  (unsigned)status);
    }
}

void IoInitializeObjects(void)
{
    ConInitialize();
    IoInitializeCompletionObjects();
    InitializeListHead(&g_open_disk_files);
    InitializeListHead(&g_vnodes);
    g_file_type = ObCreateObjectType("File", IopFileDelete);
    SeConfigureType(g_file_type, 0x120089u, 0x120116u, 0x1200A0u, 0x1F01FFu);

    /* \Device directory + a shared console File object at \Device\Console. */
    struct _OBJECT_DIRECTORY *devdir;
    if (!NT_SUCCESS(ObCreateDirectory(ObRootDirectory, "Device", &devdir)))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "cannot create \\Device");

    if (!NT_SUCCESS(ObCreateObject(g_file_type, sizeof(FILE_OBJECT),
                                   &g_console_file)))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "cannot create console");

    FILE_OBJECT *cf = (FILE_OBJECT *)g_console_file;
    cf->Kind = FileKindConsole;
    cf->Position = 0;
    cf->Access = GENERIC_ALL;

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

    char name[260];
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
        char canonical_name[260];
        PKPROCESS current = KeGetCurrentThread()->Process;
        if (!IoResolveDosPathKernel(name,
                                    current ? current->CurrentDirectory : NULL,
                                    canonical_name)) {
            status = STATUS_OBJECT_NAME_INVALID;
            goto create_done;
        }
        const char *fname = canonical_name;
        ACCESS_MASK desired_access = (ACCESS_MASK)a[1];
        ULONG share_access = (ULONG)a[6];

        UINT8 proc_snapshot[8192];
        SIZE_T proc_size = 0;
        NTSTATUS proc_open = LinuxProcSnapshot(fname, (char *)proc_snapshot,
                                                sizeof(proc_snapshot),
                                                &proc_size);
        if (NT_SUCCESS(proc_open)) {
            if (disposition != FILE_OPEN || (desired_access & 0x40000000u)) {
                status = STATUS_ACCESS_DENIED;
                goto create_done;
            }
            POBJECT proc_object;
            status = ObCreateObject(g_file_type, sizeof(FILE_OBJECT),
                                    &proc_object);
            if (!NT_SUCCESS(status)) goto create_done;
            FILE_OBJECT *proc_file = (FILE_OBJECT *)proc_object;
            proc_file->Kind = FileKindProc;
            proc_file->Access = desired_access;
            proc_file->ProcData = ExAllocatePoolWithTag(NonPagedPool,
                proc_size ? proc_size : 1, 'cPrF');
            if (!proc_file->ProcData) {
                ObDereferenceObject(proc_object);
                status = STATUS_NO_MEMORY;
                goto create_done;
            }
            memcpy(proc_file->ProcData, proc_snapshot, proc_size);
            proc_file->ProcSize = proc_size;
            InitializeListHead(&proc_file->OpenEntry);
            status = ObCreateHandle(proc_object, GENERIC_ALL, &h);
            ObDereferenceObject(proc_object);
            if (NT_SUCCESS(status)) info = FILE_OPENED;
            goto create_done;
        }
        if (proc_open != STATUS_OBJECT_NAME_NOT_FOUND) {
            status = proc_open;
            goto create_done;
        }

        UINT64 share_flags = KiIrqSave();
        NTSTATUS share_status = IopCheckShareAccess(fname, desired_access,
                                                    share_access);
        KiIrqRestore(share_flags);
        if (!NT_SUCCESS(share_status)) {
            status = share_status;
            goto create_done;
        }

        void *data = NULL;
        SIZE_T size = 0;
        FAT_FIND_DATA fat_info;
        status = FatQueryPath(fname, &fat_info);
        BOOLEAN exists = NT_SUCCESS(status);
        BOOLEAN is_directory = exists && (fat_info.Attributes & 0x10);
        BOOLEAN create = FALSE;
        BOOLEAN truncate = FALSE;
        if (exists && !is_directory)
            status = FatLoadFile(fname, &data, &size, &fat_info);
        if (exists && disposition == FILE_CREATE) {
            if (data) ExFreePool(data);
            status = STATUS_OBJECT_NAME_COLLISION;
        } else if (is_directory) {
            status = (disposition == FILE_OPEN || disposition == FILE_OPEN_IF)
                   ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
        } else {
            create = !exists &&
                (disposition == FILE_SUPERSEDE ||
                 disposition == FILE_CREATE ||
                 disposition == FILE_OPEN_IF ||
                 disposition == FILE_OVERWRITE_IF);
            truncate = exists &&
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
            IO_VNODE *vnode = IopAcquireVnode(fname, (UINT8 *)data, size,
                                               &fat_info, truncate);
            if (!vnode) {
                status = STATUS_NO_MEMORY;
                goto create_done;
            }
            POBJECT obj;
            if (!NT_SUCCESS(ObCreateObject(g_file_type, sizeof(FILE_OBJECT),
                                           &obj))) {
                IopReleaseVnode(vnode);
                status = STATUS_NO_MEMORY;
            } else {
                FILE_OBJECT *f = (FILE_OBJECT *)obj;
                f->Kind = is_directory ? FileKindDirectory : FileKindDisk;
                f->Position = 0;
                f->Access = (ACCESS_MASK)a[1];
                f->ShareAccess = share_access;
                f->Vnode = vnode;
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
                SIZE_T opened_size = vnode->Size;
                ObDereferenceObject(obj);
                if (NT_SUCCESS(status))
                    KeLog("[io]   NtCreateFile('%s') -> handle %p (%lu bytes)\n",
                          fname, h, (unsigned long)opened_size);
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

UINT64 NtOpenFile(UINT64 *a)
{
    UINT64 create_args[11] = {0};
    create_args[0] = a[0]; /* PHANDLE */
    create_args[1] = a[1]; /* DesiredAccess */
    create_args[2] = a[2]; /* ObjectAttributes */
    create_args[3] = a[3]; /* IoStatusBlock */
    create_args[6] = a[4]; /* ShareAccess */
    create_args[7] = FILE_OPEN;
    create_args[8] = a[5]; /* OpenOptions */
    return NtCreateFile(create_args);
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
    UINT64           offset = 0;
    BOOLEAN          explicit_offset = a[7] != 0;

    if (!MmProbeForWrite((UINT64)iosb, sizeof(IO_STATUS_BLOCK)) ||
        !MmProbeForWrite((UINT64)buffer, length) ||
        (explicit_offset && !MmProbeForRead(a[7], sizeof(offset))))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (explicit_offset) memcpy(&offset, (PVOID)a[7], sizeof(offset));

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(handle, 0, g_file_type, &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;

    FILE_OBJECT *f = (FILE_OBJECT *)obj;
    UINT64 n = 0;
    if (f->Kind == FileKindConsole) {
        n = ConRead(buffer, length);
    } else if (f->Kind == FileKindProc && f->ProcData) {
        UINT64 position = explicit_offset ? offset : f->Position;
        if (position < f->ProcSize) {
            UINT64 available = f->ProcSize - position;
            n = length < available ? length : available;
            memcpy(buffer, f->ProcData + position, (SIZE_T)n);
            if (!explicit_offset) f->Position += (SIZE_T)n;
        }
    } else if (f->Kind == FileKindCharDevice && f->CharOps) {
        if (f->CharOps->ReadResult) {
            if(f->CharTag==IO_CHAR_DEVICE_TAG_CONSOLE) {
                NTSTATUS allowed=io_native_console_permission(handle,GENERIC_READ);
                if(!NT_SUCCESS(allowed)){ObDereferenceObject(obj);return (UINT64)allowed;}
            }
            IO_CONSOLE_READ_PIN *pin=f->CharTag==IO_CHAR_DEVICE_TAG_CONSOLE?io_pin_console_read(obj):NULL;
            if(f->CharTag==IO_CHAR_DEVICE_TAG_CONSOLE&&!pin){ObDereferenceObject(obj);return STATUS_NO_MEMORY;}
            SIZE_T transferred = 0;
            NTSTATUS status = f->CharOps->ReadResult(f->CharContext, buffer, length, TRUE, &transferred);
            io_unpin_console_read(pin);
            n = transferred;
            if (!NT_SUCCESS(status)) {
                if (MmProbeForWrite((UINT64)iosb, sizeof(*iosb))) {
                    iosb->Status = status; iosb->Information = n;
                }
                ObDereferenceObject(obj); return (UINT64)status;
            }
        } else if (f->CharOps->Read)
            n = f->CharOps->Read(f->CharContext, buffer, length);
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
    } else if (f->Kind == FileKindDisk && f->Vnode &&
               (explicit_offset ? offset : f->Position) < f->Vnode->Size) {
        SIZE_T position = explicit_offset ? (SIZE_T)offset : f->Position;
        SIZE_T avail = f->Vnode->Size - position;
        n = length < avail ? length : avail;
        memcpy(buffer, f->Vnode->Data + position, n);
        if (!explicit_offset) f->Position += n;
    }

    if (iosb && !MmProbeForWrite((UINT64)iosb,sizeof(*iosb))) {
        ObDereferenceObject(obj);return STATUS_ACCESS_VIOLATION;
    }
    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = n;
    }
    io_record_transfer(FALSE,n);
    if (f->CompletionPort && explicit_offset)
        IoPostCompletion(f->CompletionPort, f->CompletionKey, iosb,
                         STATUS_SUCCESS, n);
    ObDereferenceObject(obj);
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtWriteFile(UINT64 *a)
{
    HANDLE           handle = (HANDLE)a[0];
    PIO_STATUS_BLOCK iosb   = (PIO_STATUS_BLOCK)a[4];
    const void      *buffer = (const void *)a[5];
    UINT64           length = (UINT32)a[6]; /* ULONG Length; upper stack half is undefined */
    UINT64           offset = 0;
    BOOLEAN          explicit_offset = a[7] != 0;

    if (!MmProbeForWrite((UINT64)iosb, sizeof(IO_STATUS_BLOCK)) ||
        !MmProbeForRead((UINT64)buffer, length) ||
        (explicit_offset && !MmProbeForRead(a[7], sizeof(offset))))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (explicit_offset) memcpy(&offset, (PVOID)a[7], sizeof(offset));

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(handle, 0, g_file_type, &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;

    FILE_OBJECT *f = (FILE_OBJECT *)obj;
    UINT64 n = 0;
    if (f->Kind == FileKindProc) {
        ObDereferenceObject(obj);
        iosb->Status = STATUS_ACCESS_DENIED;
        iosb->Information = 0;
        return (UINT64)STATUS_ACCESS_DENIED;
    } else if (f->Kind == FileKindConsole) {
        n = ConWrite(buffer, length);
    } else if (f->Kind == FileKindCharDevice && f->CharOps) {
        if (f->CharOps->WriteResult) {
            if(f->CharTag==IO_CHAR_DEVICE_TAG_CONSOLE) {
                NTSTATUS allowed=io_native_console_permission(handle,GENERIC_WRITE);
                if(!NT_SUCCESS(allowed)){ObDereferenceObject(obj);return (UINT64)allowed;}
            }
            SIZE_T transferred = 0;
            NTSTATUS status = f->CharOps->WriteResult(f->CharContext, buffer, length, &transferred);
            n = transferred;
            if (!NT_SUCCESS(status)) {
                iosb->Status = status; iosb->Information = n;
                ObDereferenceObject(obj); return (UINT64)status;
            }
        } else if (f->CharOps->Write)
            n = f->CharOps->Write(f->CharContext, buffer, length);
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
                if(n)io_record_transfer(TRUE,n);
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
        UINT64 position = explicit_offset ? offset : f->Position;
        UINT64 end = position + length;
        if (end < position) {
            ObDereferenceObject(obj);
            return (UINT64)STATUS_INVALID_PARAMETER;
        }
        IO_VNODE *vnode = f->Vnode;
        if (!vnode) {
            ObDereferenceObject(obj);
            return (UINT64)STATUS_INVALID_HANDLE;
        }
        if (end > vnode->Capacity) {
            SIZE_T capacity = (SIZE_T)((end + PAGE_SIZE - 1) & ~PAGE_MASK);
            UINT8 *expanded = ExAllocatePoolWithTag(NonPagedPool,
                                                     capacity, 'wIoF');
            if (!expanded) {
                ObDereferenceObject(obj);
                return (UINT64)STATUS_NO_MEMORY;
            }
            memset(expanded, 0, capacity);
            if (vnode->Data && vnode->Size)
                memcpy(expanded, vnode->Data, vnode->Size);
            if (vnode->Data)
                ExFreePool(vnode->Data);
            vnode->Data = expanded;
            vnode->Capacity = capacity;
        }
        memcpy(vnode->Data + position, buffer, length);
        if (!explicit_offset) f->Position += length;
        if (end > vnode->Size)
            vnode->Size = end;
        NTSTATUS write_status = FatStoreFile(vnode->Name, vnode->Data,
                                             vnode->Size, FALSE);
        if (!NT_SUCCESS(write_status)) {
            ObDereferenceObject(obj);
            iosb->Status = write_status;
            iosb->Information = 0;
            return (UINT64)write_status;
        }
        n = length;
    }

    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = n;
    }
    io_record_transfer(TRUE,n);
    if (f->CompletionPort && explicit_offset)
        IoPostCompletion(f->CompletionPort, f->CompletionKey, iosb,
                         STATUS_SUCCESS, n);
    ObDereferenceObject(obj);
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtAssociateIoCompletionFile(UINT64 *a)
{
    POBJECT file_object;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)a[0], 0,
                                                g_file_type, &file_object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    POBJECT port_object;
    status = IoReferenceCompletionPort((HANDLE)(ULONG_PTR)a[1], &port_object);
    if (NT_SUCCESS(status)) {
        FILE_OBJECT *file = file_object;
        UINT64 flags = KiIrqSave();
        if (file->CompletionPort ||
            (file->Kind != FileKindDisk && file->Kind != FileKindPipe)) {
            status = STATUS_INVALID_PARAMETER;
        } else {
            file->CompletionPort = port_object;
            file->CompletionKey = (ULONG_PTR)a[2];
        }
        KiIrqRestore(flags);
        if (!NT_SUCCESS(status)) ObDereferenceObject(port_object);
    }
    ObDereferenceObject(file_object);
    return (UINT64)status;
}

static BOOLEAN IopUtf8ToWide(const char *text, UINT16 *wide,
                             UINT32 capacity, UINT32 *bytes_out)
{
    UINT32 units = 0;
    while (*text) {
        const UINT8 *s = (const UINT8 *)text;
        UINT32 cp, count;
        if (s[0] < 0x80) { cp = s[0]; count = 1; }
        else if ((s[0] & 0xe0) == 0xc0) { cp = s[0] & 0x1f; count = 2; }
        else if ((s[0] & 0xf0) == 0xe0) { cp = s[0] & 0x0f; count = 3; }
        else if ((s[0] & 0xf8) == 0xf0) { cp = s[0] & 7; count = 4; }
        else return FALSE;
        for (UINT32 i = 1; i < count; i++) {
            if (!s[i] || (s[i] & 0xc0) != 0x80) return FALSE;
            cp = (cp << 6) | (s[i] & 0x3f);
        }
        text += count;
        if (cp <= 0xffff) {
            if (units >= capacity) return FALSE;
            wide[units++] = (UINT16)cp;
        } else {
            if (units + 1 >= capacity) return FALSE;
            cp -= 0x10000;
            wide[units++] = (UINT16)(0xd800 + (cp >> 10));
            wide[units++] = (UINT16)(0xdc00 + (cp & 0x3ff));
        }
    }
    *bytes_out = units * sizeof(UINT16);
    return TRUE;
}

static BOOLEAN IopLeapYear(UINT32 year)
{
    return (BOOLEAN)((year % 4 == 0 && year % 100 != 0) || year % 400 == 0);
}

static UINT64 IopFatWriteTime(const IO_VNODE *vnode)
{
    if (!vnode || !vnode->FatWriteDate) return 0;
    UINT32 year = 1980 + (vnode->FatWriteDate >> 9);
    UINT32 month = (vnode->FatWriteDate >> 5) & 15;
    UINT32 day = vnode->FatWriteDate & 31;
    static const UINT8 days_per_month[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    UINT32 month_days = month && month <= 12
        ? (UINT32)days_per_month[month - 1] : 0;
    if (month == 2 && IopLeapYear(year)) month_days++;
    if (!month_days || !day || day > month_days) return 0;
    UINT64 days = 0;
    for (UINT32 y = 1601; y < year; ++y)
        days += IopLeapYear(y) ? 366 : 365;
    for (UINT32 m = 1; m < month; ++m)
        days += days_per_month[m - 1] + (m == 2 && IopLeapYear(year));
    days += day - 1;
    UINT64 seconds = days * 86400 +
        ((vnode->FatWriteTime >> 11) & 31) * 3600 +
        ((vnode->FatWriteTime >> 5) & 63) * 60 +
        (vnode->FatWriteTime & 31) * 2;
    return seconds * 10000000ULL;
}

/* Native file information uses the open file object, including its current
 * byte offset, and writes the exact per-class NT layout to the caller. */
UINT64 NtQueryInformationFile(UINT64 *a)
{
    PIO_STATUS_BLOCK iosb = (PIO_STATUS_BLOCK)a[1];
    UINT8 *buffer = (UINT8 *)a[2];
    UINT32 length = (UINT32)a[3];
    UINT32 info_class = (UINT32)a[4];
    if (!MmProbeForWrite((UINT64)iosb, sizeof(*iosb)) ||
        !MmProbeForWrite((UINT64)buffer, length))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)a[0], 0,
                                                 g_file_type, &object);
    if (!NT_SUCCESS(status)) {
        iosb->Status = status;
        iosb->Information = 0;
        return (UINT64)status;
    }
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    IO_VNODE *vnode = file->Vnode;
    UINT32 needed = 0;
    UINT32 attributes = vnode ? vnode->Attributes : 0x80;
    UINT64 size = vnode ? vnode->Size : 0;
    UINT64 write_time = IopFatWriteTime(vnode);
    switch (info_class) {
    case 4: /* FILE_BASIC_INFORMATION */
        needed = 40;
        if (length >= needed) {
            memset(buffer, 0, needed);
            *(UINT64 *)(buffer + 16) = write_time;
            *(UINT64 *)(buffer + 24) = write_time;
            *(UINT32 *)(buffer + 32) = attributes;
        }
        break;
    case 5: /* FILE_STANDARD_INFORMATION */
        needed = 24;
        if (length >= needed) {
            memset(buffer, 0, needed);
            *(UINT64 *)(buffer + 0) = (size + 511) & ~511ULL;
            *(UINT64 *)(buffer + 8) = size;
            *(UINT32 *)(buffer + 16) = 1;
            buffer[21] = file->Kind == FileKindDirectory;
        }
        break;
    case 9: /* FILE_NAME_INFORMATION */
    case 48: { /* FILE_NORMALIZED_NAME_INFORMATION */
        UINT16 wide[262];
        UINT32 bytes = 0;
        const char *name = vnode ? vnode->Name : NULL;
        if (name && name[1] == ':') name += 2;
        BOOLEAN leading = name && (name[0] == '\\' || name[0] == '/');
        if (!name || !IopUtf8ToWide(name, wide + !leading, 260, &bytes)) {
            status = STATUS_NOT_SUPPORTED;
            break;
        }
        if (!leading) {
            wide[0] = '\\';
            bytes += 2;
        }
        needed = 4 + bytes;
        if (length >= 4) {
            *(UINT32 *)buffer = bytes;
            UINT32 copy = length - 4 < bytes ? length - 4 : bytes;
            copy &= ~1u;
            memcpy(buffer + 4, wide, copy);
            if (length < needed) status = STATUS_BUFFER_OVERFLOW;
        }
        break;
    }
    case 14: /* FILE_POSITION_INFORMATION */
        needed = 8;
        if (length >= needed) *(UINT64 *)buffer = file->Position;
        break;
    case 16: /* FILE_MODE_INFORMATION */
        needed = 4;
        if (length >= needed) *(UINT32 *)buffer = 0;
        break;
    case 35: /* FILE_ATTRIBUTE_TAG_INFORMATION */
        needed = 8;
        if (length >= needed) {
            *(UINT32 *)buffer = attributes;
            *(UINT32 *)(buffer + 4) = 0;
        }
        break;
    default:
        status = STATUS_INVALID_INFO_CLASS;
        break;
    }
    if (NT_SUCCESS(status) && length < needed)
        status = STATUS_BUFFER_TOO_SMALL;
    iosb->Status = status;
    iosb->Information = NT_SUCCESS(status) ? needed :
        (status == STATUS_BUFFER_OVERFLOW && length >= 4 ? length : 0);
    ObDereferenceObject(object);
    return (UINT64)status;
}

static BOOLEAN IopWideToUtf8(const UINT16 *wide, UINT32 units,
                             char *text, UINT32 capacity)
{
    UINT32 used = 0;
    for (UINT32 i = 0; i < units; i++) {
        UINT32 cp = wide[i];
        if (!cp)
            return FALSE;
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (++i >= units || wide[i] < 0xdc00 || wide[i] > 0xdfff)
                return FALSE;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (wide[i] - 0xdc00);
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return FALSE;
        }
        UINT32 bytes = cp < 0x80 ? 1 : cp < 0x800 ? 2 :
                       cp < 0x10000 ? 3 : 4;
        if (used + bytes >= capacity)
            return FALSE;
        if (bytes == 1) {
            text[used++] = (char)cp;
        } else if (bytes == 2) {
            text[used++] = (char)(0xc0 | (cp >> 6));
            text[used++] = (char)(0x80 | (cp & 0x3f));
        } else if (bytes == 3) {
            text[used++] = (char)(0xe0 | (cp >> 12));
            text[used++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            text[used++] = (char)(0x80 | (cp & 0x3f));
        } else {
            text[used++] = (char)(0xf0 | (cp >> 18));
            text[used++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            text[used++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            text[used++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    text[used] = 0;
    return TRUE;
}

/* Native directory query. FileDirectoryInformation (1) and
 * FileNamesInformation (12) are sufficient for normal Win32 enumeration;
 * entries are packed at 8-byte boundaries as required by the NT ABI. */
UINT64 NtQueryDirectoryFile(UINT64 *a)
{
    HANDLE handle = (HANDLE)a[0];
    PIO_STATUS_BLOCK iosb = (PIO_STATUS_BLOCK)a[4];
    UINT8 *buffer = (UINT8 *)a[5];
    UINT32 length = (UINT32)a[6];
    UINT32 info_class = (UINT32)a[7];
    BOOLEAN single = (BOOLEAN)a[8];
    BOOLEAN restart = (BOOLEAN)a[10];
    if (!MmProbeForWrite((UINT64)iosb, sizeof(*iosb)) ||
        !MmProbeForWrite((UINT64)buffer, length))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (info_class != 1 && info_class != 12) {
        iosb->Status = STATUS_INVALID_INFO_CLASS;
        iosb->Information = 0;
        return (UINT64)STATUS_INVALID_INFO_CLASS;
    }

    POBJECT obj;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type, &obj);
    if (!NT_SUCCESS(status))
        return (UINT64)status;
    FILE_OBJECT *file = (FILE_OBJECT *)obj;
    if (file->Kind != FileKindDirectory || !file->Vnode) {
        ObDereferenceObject(obj);
        iosb->Status = STATUS_INVALID_PARAMETER;
        iosb->Information = 0;
        return (UINT64)STATUS_INVALID_PARAMETER;
    }
    if (restart)
        file->Position = 0;

    UINT32 used = 0;
    UINT32 previous = 0xffffffffu;
    for (;;) {
        FAT_FIND_DATA found;
        status = FatEnumerateDirectory(file->Vnode->Name,
                                       (UINT32)file->Position, &found);
        if (!NT_SUCCESS(status))
            break;
        UINT16 wide_name[260];
        UINT32 name_bytes = 0;
        if (!IopUtf8ToWide(found.Name, wide_name, 260, &name_bytes)) {
            status = STATUS_OBJECT_NAME_INVALID;
            break;
        }
        UINT32 header = info_class == 1 ? 64 : 12;
        UINT32 raw_size = header + name_bytes;
        UINT32 entry_size = (raw_size + 7u) & ~7u;
        if (entry_size > length - used) {
            status = used ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
            break;
        }
        UINT8 *entry = buffer + used;
        memset(entry, 0, entry_size);
        *(UINT32 *)(entry + 4) = (UINT32)file->Position;
        if (info_class == 1) {
            *(INT64 *)(entry + 40) = found.Size;
            *(INT64 *)(entry + 48) = (found.Size + 511u) & ~511u;
            *(UINT32 *)(entry + 56) = found.Attributes;
            *(UINT32 *)(entry + 60) = name_bytes;
        } else {
            *(UINT32 *)(entry + 8) = name_bytes;
        }
        memcpy(entry + header, wide_name, name_bytes);
        if (previous != 0xffffffffu)
            *(UINT32 *)(buffer + previous) = used - previous;
        previous = used;
        used += entry_size;
        file->Position++;
        status = STATUS_SUCCESS;
        if (single || used == length)
            break;
    }
    ObDereferenceObject(obj);
    if (!used && status == STATUS_OBJECT_NAME_NOT_FOUND)
        status = STATUS_NO_MORE_FILES;
    iosb->Status = status;
    iosb->Information = used;
    return (UINT64)status;
}

#define FileRenameInformationLocal       10u
#define FileDispositionInformationLocal  13u
#define FilePositionInformationLocal     14u
#define FileEndOfFileInformationLocal    20u

static NTSTATUS IopResizeFile(FILE_OBJECT *file, SIZE_T size)
{
    IO_VNODE *vnode = file->Vnode;
    if (file->Kind != FileKindDisk || !vnode || !IopWantsWrite(file->Access))
        return STATUS_ACCESS_DENIED;
    if (size > 0xffffffffu)
        return STATUS_INVALID_PARAMETER;
    if (size > vnode->Capacity) {
        SIZE_T capacity = (size + PAGE_SIZE - 1) & ~PAGE_MASK;
        UINT8 *expanded = ExAllocatePoolWithTag(NonPagedPool, capacity, 'sIoF');
        if (!expanded)
            return STATUS_NO_MEMORY;
        memset(expanded, 0, capacity);
        if (vnode->Data && vnode->Size)
            memcpy(expanded, vnode->Data, vnode->Size);
        if (vnode->Data) ExFreePool(vnode->Data);
        vnode->Data = expanded;
        vnode->Capacity = capacity;
    }
    vnode->Size = size;
    if (file->Position > size) file->Position = size;
    return FatStoreFile(vnode->Name, vnode->Data, vnode->Size, FALSE);
}

static NTSTATUS IopCheckDirectoryEmpty(const char *path)
{
    for (UINT32 index = 0;; index++) {
        FAT_FIND_DATA found;
        NTSTATUS status = FatEnumerateDirectory(path, index, &found);
        if (status == STATUS_OBJECT_NAME_NOT_FOUND)
            return STATUS_SUCCESS;
        if (!NT_SUCCESS(status))
            return status;
        if (!IoPathSame(found.Name, ".") && !IoPathSame(found.Name, ".."))
            return STATUS_DIRECTORY_NOT_EMPTY;
    }
}

static BOOLEAN IopCanRenameVnodes(const char *old_name, const char *new_name,
                                  BOOLEAN directory)
{
    SIZE_T old_length = strlen(old_name);
    SIZE_T new_length = strlen(new_name);
    for (PLIST_ENTRY entry = g_vnodes.Flink;
         entry != &g_vnodes; entry = entry->Flink) {
        IO_VNODE *vnode = CONTAINING_RECORD(entry, IO_VNODE, Entry);
        BOOLEAN affected = IoPathSame(vnode->Name, old_name) ||
            (directory && IoPathIsWithin(old_name, vnode->Name));
        if (affected && new_length + strlen(vnode->Name + old_length) >=
                        sizeof(vnode->Name))
            return FALSE;
    }
    return TRUE;
}

static void IopRenameVnodes(const char *old_name, const char *new_name,
                            BOOLEAN directory)
{
    SIZE_T old_length = strlen(old_name);
    SIZE_T new_length = strlen(new_name);
    for (PLIST_ENTRY entry = g_vnodes.Flink;
         entry != &g_vnodes; entry = entry->Flink) {
        IO_VNODE *vnode = CONTAINING_RECORD(entry, IO_VNODE, Entry);
        if (!IoPathSame(vnode->Name, old_name) &&
            !(directory && IoPathIsWithin(old_name, vnode->Name)))
            continue;
        char renamed[260];
        memcpy(renamed, new_name, new_length);
        strcpy(renamed + new_length, vnode->Name + old_length);
        strcpy(vnode->Name, renamed);
    }
}

/* NtSetInformationFile (native x64 service 0x24). */
UINT64 NtSetInformationFile(UINT64 *a)
{
    HANDLE handle = (HANDLE)a[0];
    PIO_STATUS_BLOCK iosb = (PIO_STATUS_BLOCK)a[1];
    UINT8 *information = (UINT8 *)a[2];
    UINT32 length = (UINT32)a[3];
    UINT32 info_class = (UINT32)a[4];
    if (!MmProbeForWrite((UINT64)iosb, sizeof(*iosb)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    UINT32 minimum;
    switch (info_class) {
    case FileRenameInformationLocal: minimum = 20; break;
    case FileDispositionInformationLocal: minimum = 1; break;
    case FilePositionInformationLocal:
    case FileEndOfFileInformationLocal: minimum = 8; break;
    default:
        iosb->Status = STATUS_INVALID_INFO_CLASS;
        iosb->Information = 0;
        return (UINT64)STATUS_INVALID_INFO_CLASS;
    }
    if (length < minimum) {
        iosb->Status = STATUS_INFO_LENGTH_MISMATCH;
        iosb->Information = 0;
        return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!MmProbeForRead((UINT64)information, length)) {
        iosb->Status = STATUS_ACCESS_VIOLATION;
        iosb->Information = 0;
        return (UINT64)STATUS_ACCESS_VIOLATION;
    }

    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type,
                                                 &object);
    if (!NT_SUCCESS(status))
        goto complete;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    IO_VNODE *vnode = file->Vnode;

    if (info_class == FilePositionInformationLocal) {
        INT64 position = *(volatile INT64 *)information;
        if (file->Kind != FileKindDisk || position < 0)
            status = STATUS_INVALID_PARAMETER;
        else {
            file->Position = (SIZE_T)position;
            status = STATUS_SUCCESS;
        }
    } else if (info_class == FileEndOfFileInformationLocal) {
        INT64 end = *(volatile INT64 *)information;
        status = end < 0 ? STATUS_INVALID_PARAMETER
                         : IopResizeFile(file, (SIZE_T)end);
    } else if (!vnode || !(file->Access & DELETE_ACCESS_LOCAL)) {
        status = STATUS_ACCESS_DENIED;
    } else if (info_class == FileDispositionInformationLocal) {
        BOOLEAN delete_file = *(volatile UINT8 *)information != 0;
        if (delete_file && (vnode->Attributes & 0x10))
            status = IopCheckDirectoryEmpty(vnode->Name);
        else
            status = STATUS_SUCCESS;
        if (NT_SUCCESS(status))
            vnode->DeletePending = delete_file;
    } else {
        BOOLEAN replace = information[0] != 0;
        HANDLE root = *(volatile HANDLE *)(information + 8);
        UINT32 name_bytes = *(volatile UINT32 *)(information + 16);
        if (replace || root || (name_bytes & 1) ||
            name_bytes > length - 20) {
            status = replace ? STATUS_NOT_SUPPORTED : STATUS_INVALID_PARAMETER;
        } else {
            char raw_name[260], new_name[260];
            if (!IopWideToUtf8((const UINT16 *)(information + 20),
                               name_bytes / 2, raw_name, sizeof(raw_name)) ||
                !IoResolveDosPathKernel(raw_name,
                    KeGetCurrentThread()->Process
                        ? KeGetCurrentThread()->Process->CurrentDirectory : NULL,
                    new_name)) {
                status = STATUS_OBJECT_NAME_INVALID;
            } else {
                BOOLEAN directory = (vnode->Attributes & 0x10) != 0;
                if (!IopCanRenameVnodes(vnode->Name, new_name, directory)) {
                    status = STATUS_OBJECT_NAME_INVALID;
                } else {
                    char old_name[260];
                    strcpy(old_name, vnode->Name);
                    status = FatRenamePath(old_name, new_name);
                    if (NT_SUCCESS(status))
                        IopRenameVnodes(old_name, new_name, directory);
                }
            }
        }
    }
    ObDereferenceObject(object);

complete:
    iosb->Status = status;
    iosb->Information = 0;
    return (UINT64)status;
}

UINT64 NtFlushBuffersFile(UINT64 *a)
{
    PIO_STATUS_BLOCK iosb = (PIO_STATUS_BLOCK)a[1];
    if (!MmProbeForWrite((UINT64)iosb, sizeof(*iosb)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)a[0], 0, g_file_type,
                                                 &object);
    if (NT_SUCCESS(status)) {
        FILE_OBJECT *file = (FILE_OBJECT *)object;
        status = (file->Kind == FileKindDisk ||
                  file->Kind == FileKindDirectory)
               ? FatFlush() : STATUS_INVALID_HANDLE;
        ObDereferenceObject(object);
    }
    iosb->Status = status;
    iosb->Information = 0;
    return (UINT64)status;
}

UINT64 NtClose(UINT64 *a)
{
    return (UINT64)ObCloseHandle((HANDLE)a[0]);
}

NTSTATUS IoCreatePipeKernel(PHANDLE read_out, PHANDLE write_out)
{
    IO_PIPE_BUFFER *pipe = ExAllocatePoolWithTag(NonPagedPool,
                                                  sizeof(*pipe), 'pIoB');
    if (!pipe)
        return STATUS_NO_MEMORY;
    memset(pipe, 0, sizeof(*pipe));
    pipe->Data = ExAllocatePoolWithTag(NonPagedPool, IO_PIPE_CAPACITY, 'dIoP');
    if (!pipe->Data) {
        ExFreePool(pipe);
        return STATUS_NO_MEMORY;
    }
    pipe->Readers = pipe->Writers = 1;

    POBJECT read_object, write_object;
    NTSTATUS status = ObCreateObject(g_file_type, sizeof(FILE_OBJECT),
                                     &read_object);
    if (!NT_SUCCESS(status)) {
        ExFreePool(pipe->Data); ExFreePool(pipe);
        return status;
    }
    status = ObCreateObject(g_file_type, sizeof(FILE_OBJECT), &write_object);
    if (!NT_SUCCESS(status)) {
        pipe->Writers = 0;
        ObDereferenceObject(read_object);
        return status;
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
        return status;
    }
    *read_out = read_handle;
    *write_out = write_handle;
    return STATUS_SUCCESS;
}

NTSTATUS IoReferencePipeForPoll(HANDLE handle, PVOID *object_out)
{
    if (!object_out) return STATUS_INVALID_PARAMETER;
    *object_out = NULL;
    POBJECT object = NULL;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type,
                                                &object);
    if (!NT_SUCCESS(status)) return status;
    if (((FILE_OBJECT *)object)->Kind != FileKindPipe) {
        ObDereferenceObject(object);
        return STATUS_INVALID_HANDLE;
    }
    *object_out = object;
    return STATUS_SUCCESS;
}

UINT32 IoPollPipeObject(PVOID object)
{
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    if (!file || file->Kind != FileKindPipe) return 0;
    UINT64 irq = KiIrqSave();
    IO_PIPE_BUFFER *pipe = file->Pipe;
    UINT32 events = 0;
    if (file->PipeWriter) {
        if (pipe->Count < IO_PIPE_CAPACITY && pipe->Readers)
            events |= IO_POLL_WRITABLE;
        if (!pipe->Readers) events |= IO_POLL_ERROR;
    } else {
        if (pipe->Count || !pipe->Writers) events |= IO_POLL_READABLE;
        if (!pipe->Writers) events |= IO_POLL_HANGUP;
    }
    KiIrqRestore(irq);
    return events;
}

UINT64 NtCreatePipeSimple(UINT64 *a)
{
    PHANDLE read_out = (PHANDLE)a[0];
    PHANDLE write_out = (PHANDLE)a[1];
    if (!MmProbeForWrite((UINT64)read_out, sizeof(HANDLE)) ||
        !MmProbeForWrite((UINT64)write_out, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    return (UINT64)IoCreatePipeKernel(read_out, write_out);
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
    if (operation == SV_CONSOLE_CONTROL_WRITE_WIDE &&
        (a[3]>((SIZE_T)-1)/sizeof(UINT16)||!MmProbeForRead(argument1,a[3]*sizeof(UINT16))))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, g_file_type, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    UINT64 captured_result=0;
    if(file->Kind==FileKindCharDevice&&file->CharTag==IO_CHAR_DEVICE_TAG_CONSOLE) {
        ACCESS_MASK needed=GENERIC_READ;
        switch(operation) {
        case ConsoleControlSetAttribute:case ConsoleControlSetCursorPosition:case ConsoleControlSetCursorInfo:
        case ConsoleControlFillCharacter:case ConsoleControlFillAttribute:case SV_CONSOLE_CONTROL_FILL_WIDE:
        case SV_CONSOLE_CONTROL_WRITE_WIDE:case SV_CONSOLE_CONTROL_RESIZE_BUFFER:case SV_CONSOLE_CONTROL_SET_WINDOW:
        case SV_CONSOLE_CONTROL_FLUSH_INPUT:case SV_CONSOLE_CONTROL_WRITE_INPUT:case SV_CONSOLE_CONTROL_WRITE_OUTPUT:
        case SV_CONSOLE_CONTROL_SET_TITLE:case SV_CONSOLE_CONTROL_SET_INFO_EX:needed=GENERIC_WRITE;break;
        default:break;
        }
        NTSTATUS allowed=io_native_console_permission(handle,needed);
        if(!NT_SUCCESS(allowed)){ObDereferenceObject(object);return (UINT64)allowed;}
    }
    IO_CONSOLE_READ_PIN *pin=NULL;
    if((operation==SV_CONSOLE_CONTROL_READ_WIDE||operation==SV_CONSOLE_CONTROL_READ_INPUT)&&
       (file->Kind==FileKindConsole || (file->Kind==FileKindCharDevice&&file->CharTag==IO_CHAR_DEVICE_TAG_CONSOLE))) {
        pin=io_pin_console_read(object);
        if(!pin){ObDereferenceObject(object);return STATUS_NO_MEMORY;}
    }
    if (file->Kind == FileKindConsole)
        status = ConControl(operation, argument1, a[3], a[4], &captured_result);
    else if (file->Kind == FileKindCharDevice && file->CharOps &&
             file->CharOps->ConsoleControl)
        status = file->CharOps->ConsoleControl(file->CharContext, operation,
                                               argument1, a[3], a[4], &captured_result);
    else
        status = STATUS_INVALID_HANDLE;
    io_unpin_console_read(pin);
    if(result) {
        UINT64 flags=KiIrqSave();
        if(MmProbeForWrite((UINT64)result,sizeof(*result)))*result=captured_result;
        else status=STATUS_ACCESS_VIOLATION;
        KiIrqRestore(flags);
    }
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
    IO_VNODE *vnode = file->Vnode;
    out->Size = vnode ? vnode->Size : 0;
    out->Position = file->Position;
    out->Attributes = vnode ? vnode->Attributes : 0;
    out->IsConsole = file->Kind == FileKindConsole ||
        (file->Kind == FileKindCharDevice && file->CharTag == IO_CHAR_DEVICE_TAG_CONSOLE);
    out->FatWriteDate = vnode ? vnode->FatWriteDate : 0;
    out->FatWriteTime = vnode ? vnode->FatWriteTime : 0;
    out->Reserved = 0;
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
    status = IopResizeFile(file, (SIZE_T)a[1]);
    ObDereferenceObject(obj);
    return (UINT64)status;
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
    char raw_first[260], first[260], raw_second[260], second[260];
    if (!IopCaptureAsciiPath(a[1], raw_first, sizeof(raw_first)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    PKPROCESS process = KeGetCurrentThread()->Process;
    const char *cwd = process ? process->CurrentDirectory : NULL;
    if (!IoResolveDosPathKernel(raw_first, cwd, first))
        return (UINT64)STATUS_OBJECT_NAME_INVALID;
    switch ((ULONG)a[0]) {
    case 1:
        for (PLIST_ENTRY entry = g_open_disk_files.Flink;
             entry != &g_open_disk_files; entry = entry->Flink) {
            FILE_OBJECT *open = CONTAINING_RECORD(entry, FILE_OBJECT,
                                                   OpenEntry);
            if (open->Vnode && IoPathSame(open->Vnode->Name, first) &&
                !(open->ShareAccess & FILE_SHARE_DELETE_LOCAL))
                return (UINT64)STATUS_SHARING_VIOLATION;
        }
        for (PLIST_ENTRY entry = g_vnodes.Flink;
             entry != &g_vnodes; entry = entry->Flink) {
            IO_VNODE *vnode = CONTAINING_RECORD(entry, IO_VNODE, Entry);
            if (IoPathSame(vnode->Name, first)) {
                vnode->DeletePending = TRUE;
                return (UINT64)STATUS_SUCCESS;
            }
        }
        {
            NTSTATUS delete_status = FatDeleteFile(first);
            return (UINT64)(NT_SUCCESS(delete_status) ? FatFlush()
                                                       : delete_status);
        }
    case 2:
        if (!IopCaptureAsciiPath(a[2], raw_second, sizeof(raw_second)))
            return (UINT64)STATUS_ACCESS_VIOLATION;
        if (!IoResolveDosPathKernel(raw_second, cwd, second))
            return (UINT64)STATUS_OBJECT_NAME_INVALID;
        for (PLIST_ENTRY entry = g_open_disk_files.Flink;
             entry != &g_open_disk_files; entry = entry->Flink) {
            FILE_OBJECT *open = CONTAINING_RECORD(entry, FILE_OBJECT,
                                                   OpenEntry);
            if (open->Vnode && IoPathSame(open->Vnode->Name, first) &&
                !(open->ShareAccess & FILE_SHARE_DELETE_LOCAL))
                return (UINT64)STATUS_SHARING_VIOLATION;
        }
        {
            NTSTATUS rename_status = FatRenamePath(first, second);
            if (NT_SUCCESS(rename_status)) {
                for (PLIST_ENTRY entry = g_vnodes.Flink;
                     entry != &g_vnodes; entry = entry->Flink) {
                    IO_VNODE *vnode = CONTAINING_RECORD(entry, IO_VNODE, Entry);
                    if (!IoPathSame(vnode->Name, first))
                        continue;
                    UINT32 i = 0;
                    for (; second[i] && i + 1 < sizeof(vnode->Name); i++)
                        vnode->Name[i] = second[i];
                    vnode->Name[i] = 0;
                    break;
                }
            }
            return (UINT64)(NT_SUCCESS(rename_status) ? FatFlush()
                                                       : rename_status);
        }
    case 3: {
        NTSTATUS create_status = FatCreateDirectory(first);
        return (UINT64)(NT_SUCCESS(create_status) ? FatFlush()
                                                   : create_status);
    }
    case 4: {
        NTSTATUS remove_status = FatRemoveDirectory(first);
        return (UINT64)(NT_SUCCESS(remove_status) ? FatFlush()
                                                   : remove_status);
    }
    case 5: {
        FAT_FIND_DATA info;
        NTSTATUS status = FatQueryPath(first, &info);
        return NT_SUCCESS(status) ? (UINT64)info.Attributes : (UINT64)status;
    }
    default:
        return (UINT64)STATUS_INVALID_PARAMETER;
    }
}
