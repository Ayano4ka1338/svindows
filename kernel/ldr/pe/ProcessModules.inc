/* PEB loader lists, runtime DLL loading and executable entry points.
 * Included by kernel/ldr/Pe.c. */
/* ------------------------------------------------------------------ */
/* Process module list (PEB->Ldr)                                     */
/* ------------------------------------------------------------------ */

void LdrResetProcessContext(void)
{
    memset(g_loaded, 0, sizeof(g_loaded));
    g_dynamic_image_next = 0x0000000400000000ULL;
    g_stub_arena = 0;
    g_ldr_depth = 0;
    g_peb_ldr = NULL;
    g_ldr_cursor = g_ldr_end = NULL;
    memset(LdrpCurrentContext()->TlsModules, 0,
           sizeof(LdrpCurrentContext()->TlsModules));
    LdrpCurrentContext()->TlsIndexNext = 0;
    LdrpCurrentContext()->TlsRegionNext = LDR_TLS_VA_BASE;
}

/* Append one cached module to the PEB->Ldr lists (bump-allocated from the Ldr
 * region). Marks it linked so it isn't added twice. */
static void ldr_link_module(LDR_LOADED *m)
{
    if (!g_peb_ldr || m->Linked)
        return;
    if (g_ldr_cursor + sizeof(LDR_DATA_TABLE_ENTRY) + 128 > g_ldr_end)
        return; /* out of room */

    PLDR_DATA_TABLE_ENTRY e = (PLDR_DATA_TABLE_ENTRY)g_ldr_cursor;
    g_ldr_cursor += sizeof(LDR_DATA_TABLE_ENTRY);
    memset(e, 0, sizeof(*e));

    PIMAGE_NT_HEADERS64 nt = nt_headers(m->Base);
    e->DllBase = (PVOID)m->Base;
    e->EntryPoint = nt ? (PVOID)(m->Base + nt->OptionalHeader.AddressOfEntryPoint)
                       : NULL;
    e->SizeOfImage = nt ? nt->OptionalHeader.SizeOfImage : 0;

    /* Widen the ASCII module name into a UTF-16 buffer for BaseDllName. */
    UINT16 *wname = (UINT16 *)g_ldr_cursor;
    int n = 0;
    for (; m->Name[n] && n < 63; n++)
        wname[n] = (UINT16)(UCHAR)m->Name[n];
    wname[n] = 0;
    g_ldr_cursor += (n + 1) * sizeof(UINT16);
    g_ldr_cursor = (UINT8 *)(((UINT64)g_ldr_cursor + 7) & ~7ULL); /* realign */

    e->BaseDllName.Length = (USHORT)(n * 2);
    e->BaseDllName.MaximumLength = (USHORT)((n + 1) * 2);
    e->BaseDllName.Buffer = wname;
    e->FullDllName = e->BaseDllName;

    InsertTailList(&g_peb_ldr->InLoadOrderModuleList, &e->InLoadOrderLinks);
    InsertTailList(&g_peb_ldr->InMemoryOrderModuleList, &e->InMemoryOrderLinks);
    InsertTailList(&g_peb_ldr->InInitializationOrderModuleList,
                   &e->InInitializationOrderLinks);
    m->Linked = TRUE;
}

/* Link any cached-but-not-yet-listed modules (used after a runtime load). */
static void ldr_sync_module_list(void)
{
    for (int i = 0; i < MAX_LOADED_MODULES; i++)
        if (g_loaded[i].Valid && !g_loaded[i].Linked)
            ldr_link_module(&g_loaded[i]);
}

void LdrBuildProcessModuleList(struct _PEB *peb_opaque, UINT64 ldr_va,
                               UINT64 ldr_size)
{
    PPEB peb = (PPEB)peb_opaque;

    /* Carve the PEB_LDR_DATA out of the head of the user Ldr region; the rest
     * of the region bump-allocates the module entries. */
    PPEB_LDR_DATA ldr = (PPEB_LDR_DATA)ldr_va;
    memset(ldr, 0, sizeof(*ldr));
    ldr->Length = sizeof(PEB_LDR_DATA);
    ldr->Initialized = 1;
    InitializeListHead(&ldr->InLoadOrderModuleList);
    InitializeListHead(&ldr->InMemoryOrderModuleList);
    InitializeListHead(&ldr->InInitializationOrderModuleList);

    g_peb_ldr = ldr;
    g_ldr_cursor = (UINT8 *)(ldr_va + sizeof(PEB_LDR_DATA));
    g_ldr_end = (UINT8 *)(ldr_va + ldr_size);

    ldr_sync_module_list();

    peb->Ldr = ldr;
    KeLog("[ldr]  PEB->Ldr ready @ %p\n", (void *)ldr);
}

/* Load a DLL by name at runtime and link it into PEB->Ldr, returning its load
 * base (0 on failure). Repeat loads return the cached base. */
static void LdrpRollbackModules(const LDR_PROCESS_CONTEXT *before)
{
    PLDR_PROCESS_CONTEXT context = LdrpCurrentContext();
    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        if (context->Loaded[i].Valid && !before->Loaded[i].Valid) {
            UINT64 base = context->Loaded[i].Base;
            UINT64 size = PAGE_ALIGN_UP(nt_headers(base)->OptionalHeader.SizeOfImage);
            LdrpReleaseImagePages(base, size);
        }
    }
    if (context->StubArena && !before->StubArena)
        LdrpReleaseImagePages(context->StubArena, LDR_STUB_ARENA_SIZE);
    NTSTATUS failure = context->LoadStatus;
    *context = *before;
    context->LoadStatus = failure;
}

UINT64 LdrLoadLibrary(const char *name)
{
    /* No PEB list publication or user callbacks until the complete graph binds.
     * Snapshot in the pool, not on the small recursive loader kernel stack. */
    PLDR_PROCESS_CONTEXT before = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*before), 'TxnL');
    if (!before) return 0;
    *before = *LdrpCurrentContext();
    LdrpCurrentContext()->LoadStatus = STATUS_SUCCESS;
    UINT64 base = LdrpLoadModule(name);
    if (base)
        ldr_sync_module_list(); /* thread the newly-loaded module(s) into Ldr */
    else
        LdrpRollbackModules(before);
    ExFreePool(before);
    return base;
}

UINT64 LdrGetModuleBase(const char *name)
{
    return lookup_module(name);
}

void LdrSeedCrtCommandLine(UINT64 ansi_va, UINT64 wide_va)
{
    UINT64 base = lookup_module("msvcrt.dll");
    if (!base)
        return;

    UINT64 acmdln = LdrGetProcAddress(base, "_acmdln");
    UINT64 wcmdln = LdrGetProcAddress(base, "_wcmdln");
    if (acmdln && MmProbeForWrite(acmdln, sizeof(UINT64)))
        *(UINT64 *)acmdln = ansi_va;
    if (wcmdln && MmProbeForWrite(wcmdln, sizeof(UINT64)))
        *(UINT64 *)wcmdln = wide_va;
    KeLog("[ldr]  msvcrt CRT command line seeded (_acmdln %p -> %p, "
          "_wcmdln %p -> %p)\n", (void *)acmdln, (void *)ansi_va,
          (void *)wcmdln, (void *)wide_va);
}

BOOLEAN LdrDescribeUserAddress(UINT64 addr, const char **name_out,
                               UINT64 *base_out)
{
    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        if (!g_loaded[i].Valid)
            continue;
        if (!MmProbeForRead(g_loaded[i].Base, sizeof(IMAGE_DOS_HEADER)))
            continue;
        PIMAGE_NT_HEADERS64 nt = nt_headers(g_loaded[i].Base);
        UINT64 size = nt ? nt->OptionalHeader.SizeOfImage : 0;
        if (size && addr >= g_loaded[i].Base &&
            addr < g_loaded[i].Base + size) {
            *name_out = g_loaded[i].Name;
            *base_out = g_loaded[i].Base;
            return TRUE;
        }
    }
    return FALSE;
}

BOOLEAN LdrQueryImageSection(UINT64 addr, UINT64 *allocation_base,
                             UINT64 *section_base, UINT64 *section_size,
                             UINT32 *protection)
{
    for (int i = 0; i < MAX_LOADED_MODULES; ++i) {
        if (!g_loaded[i].Valid || !g_loaded[i].Base ||
            !MmProbeForRead(g_loaded[i].Base, sizeof(IMAGE_DOS_HEADER)))
            continue;
        UINT64 image = g_loaded[i].Base;
        PIMAGE_NT_HEADERS64 nt = nt_headers(image);
        if (!nt || addr < image ||
            addr >= image + nt->OptionalHeader.SizeOfImage)
            continue;
        PIMAGE_SECTION_HEADER sec = (PIMAGE_SECTION_HEADER)(
            (UINT8 *)&nt->OptionalHeader + nt->FileHeader.SizeOfOptionalHeader);
        for (UINT16 n = 0; n < nt->FileHeader.NumberOfSections; ++n) {
            UINT64 start = sec[n].VirtualAddress;
            UINT64 span = sec[n].VirtualSize;
            if (sec[n].SizeOfRawData > span) span = sec[n].SizeOfRawData;
            if (!span || addr < image + start || addr >= image + start + span)
                continue;
            UINT64 first = PAGE_ALIGN(start);
            UINT64 last = PAGE_ALIGN_UP(start + span);
            UINT32 flags = sec[n].Characteristics;
            UINT32 p = (flags & IMAGE_SCN_MEM_EXECUTE)
                ? ((flags & IMAGE_SCN_MEM_WRITE) ? 0x40 : 0x20)
                : ((flags & IMAGE_SCN_MEM_WRITE) ? 0x04 : 0x02);
            if (allocation_base) *allocation_base = image;
            if (section_base) *section_base = image + first;
            if (section_size) *section_size = last - first;
            if (protection) *protection = p;
            return TRUE;
        }
        /* Headers have their own read-only mapped range. */
        UINT64 headers = PAGE_ALIGN_UP(nt->OptionalHeader.SizeOfHeaders);
        if (addr < image + headers) {
            if (allocation_base) *allocation_base = image;
            if (section_base) *section_base = image;
            if (section_size) *section_size = headers;
            if (protection) *protection = 0x02;
            return TRUE;
        }
    }
    return FALSE;
}

BOOLEAN LdrDescribeUserSymbol(UINT64 addr, const char **module_out,
                              UINT64 *module_base_out,
                              const char **symbol_out,
                              UINT64 *symbol_address_out)
{
    const char *module = NULL;
    UINT64 base = 0;
    if (symbol_out)
        *symbol_out = NULL;
    if (symbol_address_out)
        *symbol_address_out = 0;
    if (!LdrDescribeUserAddress(addr, &module, &base))
        return FALSE;

    if (module_out)
        *module_out = module;
    if (module_base_out)
        *module_base_out = base;

    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    if (!nt)
        return TRUE;
    UINT64 image_size = nt->OptionalHeader.SizeOfImage;
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.Size || dir.VirtualAddress >= image_size ||
        dir.Size > image_size - dir.VirtualAddress ||
        dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
        return TRUE;

    PIMAGE_EXPORT_DIRECTORY ed =
        (PIMAGE_EXPORT_DIRECTORY)(base + dir.VirtualAddress);
    UINT64 names_bytes = (UINT64)ed->NumberOfNames * sizeof(UINT32);
    UINT64 ordinals_bytes = (UINT64)ed->NumberOfNames * sizeof(UINT16);
    UINT64 functions_bytes = (UINT64)ed->NumberOfFunctions * sizeof(UINT32);
    if (!ed->NumberOfNames || !ed->NumberOfFunctions ||
        ed->AddressOfNames >= image_size ||
        names_bytes > image_size - ed->AddressOfNames ||
        ed->AddressOfNameOrdinals >= image_size ||
        ordinals_bytes > image_size - ed->AddressOfNameOrdinals ||
        ed->AddressOfFunctions >= image_size ||
        functions_bytes > image_size - ed->AddressOfFunctions)
        return TRUE;

    const UINT32 *names = (const UINT32 *)(base + ed->AddressOfNames);
    const UINT16 *ordinals =
        (const UINT16 *)(base + ed->AddressOfNameOrdinals);
    const UINT32 *functions =
        (const UINT32 *)(base + ed->AddressOfFunctions);
    UINT64 address_rva = addr - base;
    UINT64 section_start = 0, section_end = 0;
    PIMAGE_SECTION_HEADER sections = first_section(nt);
    for (UINT16 i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        UINT64 span = sections[i].VirtualSize > sections[i].SizeOfRawData
                          ? sections[i].VirtualSize
                          : sections[i].SizeOfRawData;
        UINT64 start = sections[i].VirtualAddress;
        if ((sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) &&
            address_rva >= start && address_rva - start < span) {
            section_start = start;
            section_end = start + span;
            break;
        }
    }
    /* Do not attach a function name to pointers in .rdata/.data merely
     * because that function happened to be the preceding export RVA. */
    if (!section_end)
        return TRUE;

    UINT64 best_address = 0;
    const char *best_name = NULL;
    for (UINT32 i = 0; i < ed->NumberOfNames; i++) {
        UINT16 ordinal = ordinals[i];
        if (ordinal >= ed->NumberOfFunctions || names[i] >= image_size)
            continue;
        UINT32 rva = functions[ordinal];
        if (!rva || rva >= image_size || rva < section_start ||
            rva >= section_end)
            continue;
        /* An export-directory RVA names a forwarder, not code in this image. */
        if (rva >= dir.VirtualAddress &&
            rva < dir.VirtualAddress + dir.Size)
            continue;
        UINT64 candidate = base + rva;
        if (candidate <= addr && candidate >= best_address) {
            best_address = candidate;
            best_name = (const char *)(base + names[i]);
        }
    }

    if (best_name) {
        if (symbol_out)
            *symbol_out = best_name;
        if (symbol_address_out)
            *symbol_address_out = best_address;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                 */
/* ------------------------------------------------------------------ */

typedef struct _LDR_INIT_ENTRY {
    UINT64 ModuleBase;
    UINT64 EntryPoint;
} LDR_INIT_ENTRY;

typedef struct _LDR_INIT_TLS_ENTRY {
    UINT64 ModuleBase;
    UINT64 CallbackArray;
} LDR_INIT_TLS_ENTRY;

/* The ring-3 bootstrap (ntdll!LdrInitializeProcess) reads this page: the EXE
 * entry, one entry per DLL to attach, then the TLS callback arrays to invoke
 * with DLL_PROCESS_ATTACH before any DllMain. Ntdll.asm hardcodes these
 * offsets, so keep the layout fixed. */
typedef struct _LDR_INIT_BLOCK {
    UINT64 FinalEntry;                                 /* +0    */
    UINT64 Count;                                      /* +8    */
    LDR_INIT_ENTRY Entries[MAX_LOADED_MODULES];        /* +16   */
    UINT64 TlsCallbackCount;                           /* +1040 */
    LDR_INIT_TLS_ENTRY TlsCallbacks[MAX_TLS_MODULES];  /* +1048 */
} LDR_INIT_BLOCK;

#define LDR_INIT_BLOCK_VA 0x0000000000090000ULL

static void LdrpAppendInitDfs(int index, UINT8 *state, LDR_INIT_BLOCK *block)
{
    if (index < 0 || index >= MAX_LOADED_MODULES ||
        !g_loaded[index].Valid || state[index] == 2)
        return;
    if (state[index] == 1)
        return; /* import cycle: the outer visit will append both members */

    state[index] = 1;
    UINT64 dependencies = g_loaded[index].Dependencies;
    for (int i = 0; i < MAX_LOADED_MODULES; i++)
        if (dependencies & (1ULL << i))
            LdrpAppendInitDfs(i, state, block);
    state[index] = 2;

    if (index == 0)
        return; /* executable is entered after all DLL process attaches */
    PIMAGE_NT_HEADERS64 nt = nt_headers(g_loaded[index].Base);
    if (!nt || !nt->OptionalHeader.AddressOfEntryPoint ||
        block->Count >= MAX_LOADED_MODULES)
        return;
    UINT64 n = block->Count++;
    block->Entries[n].ModuleBase = g_loaded[index].Base;
    block->Entries[n].EntryPoint =
        g_loaded[index].Base + nt->OptionalHeader.AddressOfEntryPoint;
    KeLog("[ldr]  init[%lu] %s entry %p\n", (unsigned long)n,
          g_loaded[index].Name, (void *)block->Entries[n].EntryPoint);
}

static NTSTATUS LdrpBuildInitBlock(UINT64 executable_entry,
                                   UINT64 *bootstrap_entry,
                                   UINT64 *bootstrap_arg)
{
    UINT64 pa = MmAllocatePage();
    if (pa == MM_INVALID_PHYS)
        return STATUS_NO_MEMORY;
    if (!MmMapPage(LDR_INIT_BLOCK_VA, pa, PTE_USER | PTE_WRITE))
        return STATUS_CONFLICTING_ADDRESSES;

    LDR_INIT_BLOCK *block = (LDR_INIT_BLOCK *)LDR_INIT_BLOCK_VA;
    memset(block, 0, PAGE_SIZE);
    block->FinalEntry = executable_entry;

    /* Windows initializes DLLs in dependency-first topological order. A plain
     * reverse load list fails when sibling DLLs import one another (USERENV
     * called UCRT before UCRT's cookie/callback initialization, for example). */
    UINT8 state[MAX_LOADED_MODULES];
    memset(state, 0, sizeof(state));
    LdrpAppendInitDfs(0, state, block);
    /* Forwarded exports and delay-load helpers can pull in an otherwise
     * disconnected module. Initialize those too, still honoring known edges. */
    for (int i = 1; i < MAX_LOADED_MODULES; i++)
        if (g_loaded[i].Valid && state[i] == 0)
            LdrpAppendInitDfs(i, state, block);

    /* TLS callbacks run before DLL initialization (DLL_PROCESS_ATTACH). The
     * ring-3 bootstrap walks this list and calls each callback with its module
     * base. */
    PLDR_PROCESS_CONTEXT context = LdrpCurrentContext();
    for (int i = 0; i < MAX_TLS_MODULES; i++) {
        LDR_TLS_MODULE *module = &context->TlsModules[i];
        if (!module->Valid || !module->AddressOfCallBacks)
            continue;
        if (block->TlsCallbackCount >= MAX_TLS_MODULES)
            break;
        UINT64 t = block->TlsCallbackCount++;
        block->TlsCallbacks[t].ModuleBase = module->ModuleBase;
        block->TlsCallbacks[t].CallbackArray = module->AddressOfCallBacks;
        KeLog("[ldr]  tls callback module %p array %p\n",
              (void *)module->ModuleBase, (void *)module->AddressOfCallBacks);
    }

    UINT64 ntdll = lookup_module("ntdll.dll");
    UINT64 thunk = ntdll ? LdrGetProcAddress(ntdll, "LdrInitializeProcess") : 0;
    if (!thunk)
        return STATUS_ENTRYPOINT_NOT_FOUND;
    *bootstrap_entry = thunk;
    *bootstrap_arg = LDR_INIT_BLOCK_VA;
    KeLog("[ldr]  process bootstrap %p: %lu DLL entry points, final %p\n",
          (void *)thunk, (unsigned long)block->Count,
          (void *)executable_entry);
    return STATUS_SUCCESS;
}

NTSTATUS LdrLoadExecutable(const char *filename, UINT64 *entry_out,
                           UINT64 *base_out, UINT64 *arg_out)
{
    void *buf;
    SIZE_T size;
    NTSTATUS status = FatLoadFile(filename, &buf, &size, NULL);
    if (!NT_SUCCESS(status)) {
        KeLog("[ldr]  cannot read '%s': status 0x%08x\n", filename,
              (unsigned)status);
        return status;
    }

    UINT64 base;
    status = LdrpMapImage(buf, size, &base);
    ExFreePool(buf);
    if (!NT_SUCCESS(status)) {
        KeLog("[ldr]  '%s' is not a supported PE32+ x64 image "
              "(status 0x%08x)\n", filename, (unsigned)status);
        return status;
    }

    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    KeLog("[ldr]  exe base=%p entry_rva=0x%x size=0x%x sections=%u\n",
          (void *)base, nt->OptionalHeader.AddressOfEntryPoint,
          nt->OptionalHeader.SizeOfImage, nt->FileHeader.NumberOfSections);

    /* Register the exe itself as the first module (load-order head), before its
     * dependencies get pulled in by import resolution. */
    if (!remember_module(filename, base)) {
        LdrpReleaseImagePages(base, PAGE_ALIGN_UP(nt->OptionalHeader.SizeOfImage));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = LdrResolveImports(base);
    if (!NT_SUCCESS(status))
        return status;
    status = LdrResolveDelayImports(base);
    if (!NT_SUCCESS(status))
        return status;

    LdrpInitializeGuardPointers(base);
    LdrpProtectImage(base);

    UINT64 executable_entry = base + nt->OptionalHeader.AddressOfEntryPoint;
    status = LdrpBuildInitBlock(executable_entry, entry_out, arg_out);
    if (!NT_SUCCESS(status))
        return status;
    if (base_out)
        *base_out = base;
    KeLog("[ldr]  executable ready; bootstrap at %p\n", (void *)*entry_out);
    return STATUS_SUCCESS;
}
