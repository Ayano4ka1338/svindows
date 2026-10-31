/* Static PE thread-local storage.
 *
 * A module that declares thread-local variables carries an IMAGE_TLS_DIRECTORY
 * naming its .tls template, the DWORD the loader fills with its TLS index, and
 * an optional callback array. This part of the loader allocates one index per
 * such module and, for every thread, builds the per-thread block vector the
 * compiler reaches through `gs:[0x58]` (TEB.ThreadLocalStoragePointer).
 *
 * Included by kernel/ldr/Pe.c. */

/* LDR_TLS_MODULE and MAX_TLS_MODULES are declared with LDR_PROCESS_CONTEXT in
 * kernel/ldr/Pe.c. */

/* The per-thread TLS block vector has the same width as Windows' inline
 * TlsSlots. Static indices come first; kernel32's TlsAlloc continues after
 * them (PEB->LdrpTlsNextIndex). */
#define LDR_TLS_SLOT_COUNT 64

/* Record a module's TLS directory and give it an index. Safe to call for a
 * module with no TLS directory. */
static void LdrpRegisterTls(UINT64 base)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    if (!nt || nt->OptionalHeader.NumberOfRvaAndSizes <=
                  IMAGE_DIRECTORY_ENTRY_TLS)
        return;
    IMAGE_DATA_DIRECTORY *dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dir->VirtualAddress || dir->Size < sizeof(IMAGE_TLS_DIRECTORY64))
        return;

    PLDR_PROCESS_CONTEXT context = LdrpCurrentContext();
    if (context->TlsIndexNext >= MAX_TLS_MODULES)
        return;

    IMAGE_TLS_DIRECTORY64 *tls =
        (IMAGE_TLS_DIRECTORY64 *)(base + dir->VirtualAddress);

    LDR_TLS_MODULE *module = NULL;
    for (int i = 0; i < MAX_TLS_MODULES; i++) {
        if (context->TlsModules[i].Valid &&
            context->TlsModules[i].ModuleBase == base)
            return; /* already registered */
        if (!context->TlsModules[i].Valid && !module)
            module = &context->TlsModules[i];
    }
    if (!module)
        return;

    module->ModuleBase = base;
    module->TemplateStart = tls->StartAddressOfRawData;
    module->TemplateEnd = tls->EndAddressOfRawData;
    module->AddressOfIndex = tls->AddressOfIndex;
    module->AddressOfCallBacks = tls->AddressOfCallBacks;
    module->SizeOfZeroFill = tls->SizeOfZeroFill;
    module->Index = context->TlsIndexNext++;
    module->Valid = TRUE;

    /* Publish the index where the compiler's TLS accessor reads it. */
    if (module->AddressOfIndex &&
        MmProbeForWrite(module->AddressOfIndex, sizeof(UINT32)))
        *(UINT32 *)module->AddressOfIndex = module->Index;

    KeLog("[ldr]  tls[%lu] base=%p template %p..%p zerofill 0x%x\n",
          (unsigned long)module->Index, (void *)base,
          (void *)module->TemplateStart, (void *)module->TemplateEnd,
          (unsigned)module->SizeOfZeroFill);
}

static void LdrpTlsReleasePages(UINT64 base, UINT64 end)
{
    for (UINT64 va = base; va < end; va += PAGE_SIZE) {
        UINT64 pa = MmGetPhysicalAddress(va);
        if (pa != MM_INVALID_PHYS && MmUnmapPage(va))
            MmFreePages(PAGE_ALIGN(pa), 1);
    }
}

/* Allocate a contiguous TLS span, reusing unmapped holes left by threads. */
static UINT64 LdrpTlsAllocate(PLDR_PROCESS_CONTEXT context, SIZE_T bytes)
{
    if (!bytes || bytes > ~(SIZE_T)0 - (PAGE_SIZE - 1))
        return 0;
    SIZE_T rounded = (bytes + PAGE_SIZE - 1) & ~PAGE_MASK;
    UINT64 va = LDR_TLS_VA_BASE;
    while (va < context->TlsRegionNext &&
           va < MM_USER_MAX && rounded <= MM_USER_MAX - va) {
        SIZE_T off = 0;
        while (off < rounded &&
               MmGetPhysicalAddress(va + off) == MM_INVALID_PHYS)
            off += PAGE_SIZE;
        if (off == rounded) break;
        va += off + PAGE_SIZE;
    }
    if (!va || va >= MM_USER_MAX || rounded > MM_USER_MAX - va)
        return 0;
    for (SIZE_T off = 0; off < rounded; off += PAGE_SIZE)
        if (MmGetPhysicalAddress(va + off) != MM_INVALID_PHYS)
            return 0;
    for (SIZE_T off = 0; off < rounded; off += PAGE_SIZE) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS) {
            LdrpTlsReleasePages(va, va + off);
            return 0;
        }
        if (!MmMapPage(va + off, pa, PTE_USER | PTE_WRITE)) {
            MmFreePages(pa, 1);
            LdrpTlsReleasePages(va, va + off);
            return 0;
        }
    }
    if (va + rounded > context->TlsRegionNext)
        context->TlsRegionNext = va + rounded;
    return va;
}

UINT32 LdrTlsIndexCount(void)
{
    return LdrpCurrentContext()->TlsIndexNext;
}

/* Build the TLS block vector for a thread whose TEB lives at `teb_va`: one
 * zero-initialized, template-copied block per TLS module, hung off
 * TEB.ThreadLocalStoragePointer. Must run while the process address space is
 * active (process creation or NtCreateThreadEx). */
NTSTATUS LdrApplyThreadTlsWithAllocation(UINT64 teb_va,
                                         UINT64 *base_out, SIZE_T *pages_out)
{
    PLDR_PROCESS_CONTEXT context = LdrpCurrentContext();
    if (base_out) *base_out = 0;
    if (pages_out) *pages_out = 0;
    if (!context->TlsIndexNext)
        return STATUS_SUCCESS;

    if (!MmProbeForWrite(teb_va + 0x58, sizeof(UINT64)))
        return STATUS_ACCESS_VIOLATION;
    SIZE_T total = PAGE_SIZE; /* vector; each block begins on a page */
    for (int i = 0; i < MAX_TLS_MODULES; i++) {
        LDR_TLS_MODULE *module = &context->TlsModules[i];
        if (!module->Valid || module->Index >= LDR_TLS_SLOT_COUNT)
            continue;
        if (module->TemplateEnd < module->TemplateStart)
            return STATUS_INVALID_IMAGE_FORMAT;
        UINT64 initialized = module->TemplateEnd - module->TemplateStart;
        PIMAGE_NT_HEADERS64 image = nt_headers(module->ModuleBase);
        if (!image || (initialized &&
            (module->TemplateStart < module->ModuleBase ||
             module->TemplateEnd < module->ModuleBase ||
             module->TemplateEnd - module->ModuleBase >
                 image->OptionalHeader.SizeOfImage)))
            return STATUS_INVALID_IMAGE_FORMAT;
        if (initialized > ~(SIZE_T)0 - module->SizeOfZeroFill ||
            (initialized && !MmProbeForRead(module->TemplateStart, initialized)))
            return STATUS_INVALID_IMAGE_FORMAT;
        SIZE_T size = (SIZE_T)initialized + module->SizeOfZeroFill;
        if (size > 0x1000000ULL)
            return STATUS_INVALID_IMAGE_FORMAT;
        if (!size)
            continue;
        SIZE_T rounded = (size + PAGE_SIZE - 1) & ~PAGE_MASK;
        if (rounded > ~(SIZE_T)0 - total)
            return STATUS_NO_MEMORY;
        total += rounded;
    }

    UINT64 allocation_start = LdrpTlsAllocate(context, total);
    if (!allocation_start)
        return STATUS_NO_MEMORY;
    UINT64 *vector = (UINT64 *)allocation_start;
    memset(vector, 0, PAGE_SIZE);
    UINT64 block_va = allocation_start + PAGE_SIZE;
    for (int i = 0; i < MAX_TLS_MODULES; i++) {
        LDR_TLS_MODULE *module = &context->TlsModules[i];
        if (!module->Valid || module->Index >= LDR_TLS_SLOT_COUNT)
            continue;
        SIZE_T initialized = (SIZE_T)(module->TemplateEnd - module->TemplateStart);
        SIZE_T size = initialized + module->SizeOfZeroFill;
        if (!size)
            continue;
        memset((void *)block_va, 0, size);
        if (initialized)
            memcpy((void *)block_va, (const void *)module->TemplateStart,
                   (SIZE_T)initialized);
        vector[module->Index] = block_va;
        block_va += (size + PAGE_SIZE - 1) & ~PAGE_MASK;
    }

    *(UINT64 *)(teb_va + 0x58) = allocation_start;
    if (base_out) *base_out = allocation_start;
    if (pages_out) *pages_out = total / PAGE_SIZE;
    return STATUS_SUCCESS;
}

NTSTATUS LdrApplyThreadTls(UINT64 teb_va)
{
    return LdrApplyThreadTlsWithAllocation(teb_va, NULL, NULL);
}

void LdrRollbackThreadTls(UINT64 base, SIZE_T pages)
{
    PLDR_PROCESS_CONTEXT context = LdrpCurrentContext();
    if (!base || base >= MM_USER_MAX || !pages ||
        pages > (MM_USER_MAX - base) / PAGE_SIZE)
        return;
    UINT64 end = base + pages * PAGE_SIZE;
    LdrpTlsReleasePages(base, end);
    if (context->TlsRegionNext == end)
        context->TlsRegionNext = base;
}
