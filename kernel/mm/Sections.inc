/* Section objects and shared committed views. Included after the VM helpers
 * in ke/Syscall.c so the same address-reservation rules apply to views. */
#define SEC_COMMIT_LOCAL 0x08000000u
#define SEC_IMAGE_LOCAL  0x01000000u
#define SECTION_MAP_WRITE_LOCAL 0x0002u
#define SECTION_MAP_READ_LOCAL  0x0004u
#define SECTION_MAP_EXEC_LOCAL  0x0008u

typedef struct _MM_SECTION {
    UINT64 Size;
    UINT64 PageCount;
    UINT64 *Pages;
    ULONG Protection;
} MM_SECTION;

static POBJECT_TYPE g_section_type;

static void MiDeleteSection(POBJECT object)
{
    MM_SECTION *section = (MM_SECTION *)object;
    for (UINT64 i = 0; i < section->PageCount; ++i)
        if (section->Pages[i] != MM_INVALID_PHYS)
            MmFreePages(section->Pages[i], 1);
    ExFreePool(section->Pages);
}

static NTSTATUS MiUnmapSectionRegion(PKPROCESS process,
                                     PMM_PROCESS_REGION region)
{
    MM_SECTION *section = (MM_SECTION *)region->Section;
    if (!section)
        return STATUS_NOT_FOUND;
    for (UINT64 va = region->Base;
         va < region->Base + region->ReserveSize; va += PAGE_SIZE)
        MmUnmapPageInAddressSpace(process->DirectoryTableBase, va);
    memset(region, 0, sizeof(*region));
    ObDereferenceObject(section);
    return STATUS_SUCCESS;
}

void MmCleanupProcessSections(PKPROCESS process)
{
    if (!process)
        return;
    for (ULONG i = 0; i < MM_MAX_PROCESS_REGIONS; ++i)
        if (process->VmRegions[i].InUse && process->VmRegions[i].Section)
            MiUnmapSectionRegion(process, &process->VmRegions[i]);
}

/* NtCreateSection(Handle, Access, Attributes, MaximumSize, Protect,
 *                 AllocationAttributes, FileHandle). The file-backed case
 * captures the current file bytes; writable file-backed mappings and PE image
 * sections require writeback/relocation machinery and are rejected. */
static UINT64 NtCreateSection(UINT64 *a)
{
    HANDLE *result = (HANDLE *)a[0];
    INT64 requested = 0;
    if (!MmProbeForWrite((UINT64)result, sizeof(*result)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (a[2]) {
        if (!MmProbeForRead(a[2], sizeof(OBJECT_ATTRIBUTES)))
            return (UINT64)STATUS_ACCESS_VIOLATION;
        const OBJECT_ATTRIBUTES *oa = (const OBJECT_ATTRIBUTES *)a[2];
        if (oa->Length < sizeof(*oa) || oa->ObjectName || oa->RootDirectory)
            return (UINT64)STATUS_NOT_SUPPORTED;
    }
    if (a[3] && !MmProbeForRead(a[3], sizeof(requested)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (a[3]) memcpy(&requested, (void *)a[3], sizeof(requested));
    ULONG protect = (ULONG)a[4];
    ULONG attributes = (ULONG)a[5];
    if (!MiValidProtection(protect) || protect == PAGE_NOACCESS ||
        (protect & PAGE_GUARD) || (attributes & SEC_IMAGE_LOCAL) ||
        (attributes & ~SEC_COMMIT_LOCAL) ||
        (attributes && !(attributes & SEC_COMMIT_LOCAL)))
        return (UINT64)STATUS_NOT_SUPPORTED;

    POBJECT file = NULL;
    const UINT8 *file_data = NULL;
    SIZE_T file_size = 0;
    if (a[6]) {
        NTSTATUS status = IoReferenceFileData((HANDLE)a[6], &file,
                                               &file_data, &file_size);
        if (!NT_SUCCESS(status)) return (UINT64)status;
        if (protect != PAGE_READONLY && protect != 0x20) {
            ObDereferenceObject(file);
            return (UINT64)STATUS_NOT_SUPPORTED;
        }
        if (!requested) requested = (INT64)file_size;
    }
    if (requested <= 0 || (UINT64)requested > 64ULL * 1024 * 1024) {
        if (file) ObDereferenceObject(file);
        return (UINT64)STATUS_INVALID_PARAMETER;
    }
    UINT64 page_count = BYTES_TO_PAGES((UINT64)requested);
    if (!g_section_type)
        g_section_type = ObCreateObjectType("Section", MiDeleteSection);
    SeConfigureType(g_section_type, 0x20005u, 0x20002u, 0x20008u, 0xF001Fu);
    if (!g_section_type) {
        if (file) ObDereferenceObject(file);
        return (UINT64)STATUS_NO_MEMORY;
    }
    POBJECT object;
    NTSTATUS status = ObCreateObject(g_section_type, sizeof(MM_SECTION),
                                     &object);
    if (!NT_SUCCESS(status)) {
        if (file) ObDereferenceObject(file);
        return (UINT64)status;
    }
    MM_SECTION *section = (MM_SECTION *)object;
    section->Size = PAGE_ALIGN_UP((UINT64)requested);
    section->PageCount = page_count;
    section->Protection = protect;
    section->Pages = ExAllocatePoolWithTag(NonPagedPool,
                             page_count * sizeof(UINT64), 'cSeM');
    if (!section->Pages) {
        if (file) ObDereferenceObject(file);
        ObDereferenceObject(object);
        return (UINT64)STATUS_NO_MEMORY;
    }
    for (UINT64 i = 0; i < page_count; ++i)
        section->Pages[i] = MM_INVALID_PHYS;
    for (UINT64 i = 0; i < page_count; ++i) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS) {
            if (file) ObDereferenceObject(file);
            ObDereferenceObject(object);
            return (UINT64)STATUS_NO_MEMORY;
        }
        section->Pages[i] = pa;
        UINT8 *dst = MmPhysToVirt(pa);
        memset(dst, 0, PAGE_SIZE);
        UINT64 offset = i * PAGE_SIZE;
        if (file && offset < file_size && offset < (UINT64)requested) {
            SIZE_T length = file_size - offset;
            if (length > PAGE_SIZE) length = PAGE_SIZE;
            if (length > (UINT64)requested - offset)
                length = (UINT64)requested - offset;
            memcpy(dst, file_data + offset, length);
        }
    }
    if (file) ObDereferenceObject(file);
    HANDLE handle;
    status = ObCreateHandle(object, (ACCESS_MASK)a[1], &handle);
    ObDereferenceObject(object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    *result = handle;
    return (UINT64)STATUS_SUCCESS;
}

static UINT64 NtMapViewOfSection(UINT64 *a)
{
    /* The current address space is required until remote-process mapping is
     * implemented. NtCurrentProcess is the -1 pseudo handle. */
    if (a[1] != ~(UINT64)0 || a[3] || a[4] || a[7] > 2 || a[8])
        return (UINT64)STATUS_NOT_SUPPORTED;
    PVOID *base_ptr = (PVOID *)a[2];
    SIZE_T *size_ptr = (SIZE_T *)a[6];
    if (!MmProbeForWrite((UINT64)base_ptr, sizeof(*base_ptr)) ||
        !MmProbeForWrite((UINT64)size_ptr, sizeof(*size_ptr)) ||
        (a[5] && !MmProbeForRead(a[5], sizeof(INT64))))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    UINT64 offset = 0;
    if (a[5]) memcpy(&offset, (void *)a[5], sizeof(offset));
    if (offset & 0xffff)
        return (UINT64)STATUS_INVALID_PARAMETER;
    ULONG protect = (ULONG)a[9];
    if (!MiValidProtection(protect) || protect == PAGE_NOACCESS ||
        protect == 0x08 || protect == 0x80 || (protect & PAGE_GUARD))
        return (UINT64)STATUS_INVALID_PAGE_PROTECTION;
    if (!g_section_type)
        return (UINT64)STATUS_INVALID_HANDLE;
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)a[0], 0,
                                                 g_section_type, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    MM_SECTION *section = (MM_SECTION *)object;
    BOOLEAN wants_write = (BOOLEAN)(MiProtectionFlags(protect) & PTE_WRITE);
    BOOLEAN allows_write = (BOOLEAN)(MiProtectionFlags(section->Protection) & PTE_WRITE);
    BOOLEAN wants_execute = (BOOLEAN)(!(MiProtectionFlags(protect) & PTE_NX));
    BOOLEAN allows_execute = (BOOLEAN)(!(MiProtectionFlags(section->Protection) & PTE_NX));
    if ((wants_write && !allows_write) ||
        (wants_execute && !allows_execute)) {
        ObDereferenceObject(object);
        return (UINT64)STATUS_ACCESS_DENIED;
    }
    if (*size_ptr > section->Size) {
        ObDereferenceObject(object);
        return (UINT64)STATUS_INVALID_PARAMETER;
    }
    UINT64 size = *size_ptr ? PAGE_ALIGN_UP(*size_ptr) : section->Size - offset;
    if (offset >= section->Size || !size || size > section->Size - offset) {
        ObDereferenceObject(object);
        return (UINT64)STATUS_INVALID_PARAMETER;
    }
    PKPROCESS process = KeGetCurrentThread()->Process;
    UINT64 requested = (UINT64)*base_ptr;
    if (requested & 0xffff) {
        ObDereferenceObject(object);
        return (UINT64)STATUS_INVALID_PARAMETER;
    }
    UINT64 base = requested ? requested :
                        (process->UserAllocationNext + 0xffff) & ~0xffffULL;
    if (base < 0x10000 || base >= MM_USER_MAX || size > MM_USER_MAX - base) {
        ObDereferenceObject(object);
        return (UINT64)STATUS_INVALID_PARAMETER;
    }
    for (ULONG i = 0; i < MM_MAX_PROCESS_REGIONS; ++i) {
        PMM_PROCESS_REGION other = &process->VmRegions[i];
        if (other->InUse && base < other->Base + other->ReserveSize &&
            other->Base < base + size) {
            ObDereferenceObject(object);
            return (UINT64)STATUS_CONFLICTING_ADDRESSES;
        }
    }
    for (UINT64 va = base; va < base + size; va += PAGE_SIZE)
        if (MmGetPhysicalAddress(va) != MM_INVALID_PHYS) {
            ObDereferenceObject(object);
            return (UINT64)STATUS_CONFLICTING_ADDRESSES;
        }
    PMM_PROCESS_REGION region = MiAllocateRegionSlot(process);
    if (!region) {
        ObDereferenceObject(object);
        return (UINT64)STATUS_NO_MEMORY;
    }
    UINT64 mapped = 0;
    for (; mapped < size; mapped += PAGE_SIZE)
        if (!MmMapPage(base + mapped,
                       section->Pages[(offset + mapped) / PAGE_SIZE],
                       MiProtectionFlags(protect) | PTE_SHARED)) {
            for (UINT64 undo = 0; undo < mapped; undo += PAGE_SIZE)
                MmUnmapPage(base + undo);
            ObDereferenceObject(object);
            return (UINT64)STATUS_NO_MEMORY;
        }
    memset(region, 0, sizeof(*region));
    region->Base = base;
    region->ReserveSize = size;
    region->CommittedPages = size / PAGE_SIZE;
    region->AllocationProtect = section->Protection;
    region->Protect = protect;
    region->Type = MEM_MAPPED;
    region->Section = section; /* retained reference */
    region->SectionOffset = offset;
    region->InUse = TRUE;
    if (base + size > process->UserAllocationNext)
        process->UserAllocationNext = base + size;
    *base_ptr = (PVOID)base;
    *size_ptr = size;
    return (UINT64)STATUS_SUCCESS;
}

static UINT64 NtUnmapViewOfSection(UINT64 *a)
{
    if (a[0] != ~(UINT64)0)
        return (UINT64)STATUS_NOT_SUPPORTED;
    PKPROCESS process = KeGetCurrentThread()->Process;
    PMM_PROCESS_REGION region = MiFindRegion(process, a[1], 1);
    if (!region || !region->Section)
        return (UINT64)STATUS_NOT_FOUND;
    return (UINT64)MiUnmapSectionRegion(process, region);
}
