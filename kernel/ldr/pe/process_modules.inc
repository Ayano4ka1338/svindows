/* PEB loader lists, runtime DLL loading and executable entry points.
 * Included by kernel/ldr/pe.c. */
/* ------------------------------------------------------------------ */
/* Process module list (PEB->Ldr)                                     */
/* ------------------------------------------------------------------ */

void LdrResetProcessContext(void)
{
    memset(g_loaded, 0, sizeof(g_loaded));
    g_dynamic_image_next = 0x0000000400000000ULL;
    g_stub_arena = g_stub_next = g_stub_end = 0;
    g_ldr_depth = 0;
    g_peb_ldr = NULL;
    g_ldr_cursor = g_ldr_end = NULL;
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
UINT64 LdrLoadLibrary(const char *name)
{
    UINT64 base = LdrpLoadModule(name);
    if (base)
        ldr_sync_module_list(); /* thread the newly-loaded module(s) into Ldr */
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

/* ------------------------------------------------------------------ */
/* Public entry point                                                 */
/* ------------------------------------------------------------------ */

typedef struct _LDR_INIT_ENTRY {
    UINT64 ModuleBase;
    UINT64 EntryPoint;
} LDR_INIT_ENTRY;

typedef struct _LDR_INIT_BLOCK {
    UINT64 FinalEntry;
    UINT64 Count;
    LDR_INIT_ENTRY Entries[MAX_LOADED_MODULES];
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
    remember_module(filename, base);

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
