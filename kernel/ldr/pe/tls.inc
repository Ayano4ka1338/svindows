/* Static PE thread-local storage.
 *
 * A module that declares thread-local variables carries an IMAGE_TLS_DIRECTORY
 * naming its .tls template, the DWORD the loader fills with its TLS index, and
 * an optional callback array. This part of the loader allocates one index per
 * such module and, for every thread, builds the per-thread block vector the
 * compiler reaches through `gs:[0x58]` (TEB.ThreadLocalStoragePointer).
 *
 * Included by kernel/ldr/pe.c. */

/* LDR_TLS_MODULE and MAX_TLS_MODULES are declared with LDR_PROCESS_CONTEXT in
 * kernel/ldr/pe.c. */

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
            context->TlsModules[i].TemplateStart == tls->StartAddressOfRawData)
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

/* Bump-allocate `bytes` of user-writable memory in the TLS region. */
static UINT64 LdrpTlsAllocate(PLDR_PROCESS_CONTEXT context, SIZE_T bytes)
{
    if (!bytes)
        return 0;
    SIZE_T rounded = (bytes + PAGE_SIZE - 1) & ~PAGE_MASK;
    UINT64 va = context->TlsRegionNext;
    for (SIZE_T off = 0; off < rounded; off += PAGE_SIZE) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS)
            return 0;
        if (!MmMapPage(va + off, pa, PTE_USER | PTE_WRITE))
            return 0;
    }
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
void LdrApplyThreadTls(UINT64 teb_va)
{
    PLDR_PROCESS_CONTEXT context = LdrpCurrentContext();
    if (!context->TlsIndexNext)
        return;

    UINT64 vector_va = LdrpTlsAllocate(
        context, LDR_TLS_SLOT_COUNT * sizeof(UINT64));
    if (!vector_va)
        return;
    UINT64 *vector = (UINT64 *)vector_va;
    memset(vector, 0, LDR_TLS_SLOT_COUNT * sizeof(UINT64));

    for (int i = 0; i < MAX_TLS_MODULES; i++) {
        LDR_TLS_MODULE *module = &context->TlsModules[i];
        if (!module->Valid || module->Index >= LDR_TLS_SLOT_COUNT)
            continue;
        UINT64 initialized = module->TemplateEnd > module->TemplateStart ?
            module->TemplateEnd - module->TemplateStart : 0;
        SIZE_T size = (SIZE_T)initialized + module->SizeOfZeroFill;
        if (!size)
            continue;
        UINT64 block_va = LdrpTlsAllocate(context, size);
        if (!block_va)
            continue;
        memset((void *)block_va, 0, size);
        if (initialized && MmProbeForRead(module->TemplateStart,
                                          (SIZE_T)initialized))
            memcpy((void *)block_va, (const void *)module->TemplateStart,
                   (SIZE_T)initialized);
        vector[module->Index] = block_va;
    }

    if (MmProbeForWrite(teb_va + 0x58, sizeof(UINT64)))
        *(UINT64 *)(teb_va + 0x58) = vector_va;
}
