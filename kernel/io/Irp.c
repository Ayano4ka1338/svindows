/* Synchronous IRP boundary for the in-memory initrd block device. */
#include <svindows/Io.h>
#include <svindows/Rtl.h>

static UINT8 *g_memory_disk;
static SIZE_T g_memory_disk_size;

BOOLEAN IoDiskSetMemoryImage(void *image, SIZE_T size)
{
    if (!image || size < ATA_SECTOR_SIZE || (size % ATA_SECTOR_SIZE))
        return FALSE;
    g_memory_disk = (UINT8 *)image;
    g_memory_disk_size = size;
    return TRUE;
}

BOOLEAN IoDiskIsMemoryBacked(void)
{
    return g_memory_disk != NULL;
}

NTSTATUS IoCallDriver(IO_DEVICE_OBJECT *device, IO_IRP *irp)
{
    if (!device || !device->DriverObject || !irp ||
        irp->Stack.MajorFunction >= IRP_MJ_MAXIMUM)
        return STATUS_INVALID_PARAMETER;
    PIO_DISPATCH dispatch =
        device->DriverObject->MajorFunction[irp->Stack.MajorFunction];
    if (!dispatch)
        return STATUS_NOT_SUPPORTED;
    irp->Status = STATUS_PENDING;
    irp->Information = 0;
    irp->Completed = FALSE;
    NTSTATUS status = dispatch(device, irp);
    /* This first device contract is synchronous. A driver that returns
     * without completing cannot make the caller's stack IRP safe to reuse. */
    if (!irp->Completed)
        IoCompleteRequest(device, irp,
                          status == STATUS_PENDING ? STATUS_INTERNAL_ERROR : status,
                          0);
    return irp->Status;
}

void IoCompleteRequest(IO_DEVICE_OBJECT *device, IO_IRP *irp,
                       NTSTATUS status, SIZE_T information)
{
    if (!irp || irp->Completed)
        return;
    irp->Status = status;
    irp->Information = NT_SUCCESS(status) ? information : 0;
    irp->Completed = TRUE;
    if (irp->Completion)
        irp->Completion(device, irp, irp->CompletionContext);
}

static NTSTATUS IopDiskDispatch(IO_DEVICE_OBJECT *device, IO_IRP *irp)
{
    (void)device;
    IO_STACK_LOCATION *stack = &irp->Stack;
    BOOLEAN ok = FALSE;
    SIZE_T bytes = 0;
    if (stack->MajorFunction == IRP_MJ_FLUSH) {
        ok = g_memory_disk ? TRUE : AtaFlushCache();
    } else if (stack->SectorCount && stack->Buffer) {
        bytes = (SIZE_T)stack->SectorCount * ATA_SECTOR_SIZE;
        SIZE_T offset = (SIZE_T)stack->Sector * ATA_SECTOR_SIZE;
        if (g_memory_disk && offset <= g_memory_disk_size &&
            bytes <= g_memory_disk_size - offset) {
            if (stack->MajorFunction == IRP_MJ_READ) {
                memcpy(stack->Buffer, g_memory_disk + offset, bytes);
                ok = TRUE;
            } else if (stack->MajorFunction == IRP_MJ_WRITE) {
                memcpy(g_memory_disk + offset, stack->Buffer, bytes);
                ok = TRUE;
            }
        } else if (!g_memory_disk && stack->MajorFunction == IRP_MJ_READ)
            ok = AtaReadSectors(stack->Sector, stack->SectorCount,
                                stack->Buffer);
        else if (!g_memory_disk && stack->MajorFunction == IRP_MJ_WRITE)
            ok = AtaWriteSectors(stack->Sector, stack->SectorCount,
                                 stack->Buffer);
    }
    NTSTATUS status = ok ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    IoCompleteRequest(device, irp, status, ok ? bytes : 0);
    return status;
}

static IO_DRIVER_OBJECT g_disk_driver = {
    {IopDiskDispatch, IopDiskDispatch, IopDiskDispatch}
};
static IO_DEVICE_OBJECT g_disk_device = {&g_disk_driver, NULL};

static BOOLEAN IopDiskRequest(UINT8 major, UINT32 lba, UINT8 count,
                              void *buffer)
{
    IO_IRP irp;
    memset(&irp, 0, sizeof(irp));
    irp.Stack.MajorFunction = major;
    irp.Stack.Sector = lba;
    irp.Stack.SectorCount = count;
    irp.Stack.Buffer = buffer;
    NTSTATUS status = IoCallDriver(&g_disk_device, &irp);
    return (BOOLEAN)(NT_SUCCESS(status) &&
        irp.Information == (major == IRP_MJ_FLUSH ? 0 :
                           (SIZE_T)count * ATA_SECTOR_SIZE));
}

BOOLEAN IoDiskReadSectors(UINT32 lba, UINT8 count, void *buffer)
{ return IopDiskRequest(IRP_MJ_READ, lba, count, buffer); }
BOOLEAN IoDiskWriteSectors(UINT32 lba, UINT8 count, const void *buffer)
{ return IopDiskRequest(IRP_MJ_WRITE, lba, count, (void *)buffer); }
BOOLEAN IoDiskFlush(void)
{ return IopDiskRequest(IRP_MJ_FLUSH, 0, 0, NULL); }
