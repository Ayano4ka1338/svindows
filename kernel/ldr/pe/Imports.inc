/* PE exports, strict import binding, dependency loading and bootstrap.
 * Included by kernel/ldr/Pe.c. */
/* ------------------------------------------------------------------ */
/* Exports and imports                                                */
/* ------------------------------------------------------------------ */

/* Forward declarations: forwarded exports may recursively load their target
 * module and resolve either a name or an ordinal there. */
static UINT64 LdrpLoadModule(const char *name);
static UINT64 LdrpProcByOrdinal(UINT64 base, UINT16 ordinal);
static UINT64 lookup_module(const char *name);

static BOOLEAN LdrpImageContains(UINT64 base, UINT64 rva, UINT64 bytes)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    if (!nt) return FALSE;
    UINT64 size = nt->OptionalHeader.SizeOfImage;
    return rva <= size && bytes <= size - rva;
}

static const char *LdrpImageString(UINT64 base, UINT64 rva)
{
    if (!LdrpImageContains(base, rva, 1)) return NULL;
    UINT64 end = nt_headers(base)->OptionalHeader.SizeOfImage;
    for (UINT64 i = rva; i < end; i++)
        if (!*(const char *)(base + i)) return (const char *)(base + rva);
    return NULL;
}

static UINT64 LdrpResolveForwarder(const char *forwarder)
{
    char module[64];
    char symbol[128];
    int dot = -1;
    for (int i = 0; forwarder[i]; i++)
        if (forwarder[i] == '.') dot = i;
    if (dot <= 0 || dot >= 58)
        return 0;

    int i = 0;
    for (; i < dot; i++)
        module[i] = forwarder[i];
    module[i++] = '.';
    module[i++] = 'd'; module[i++] = 'l'; module[i++] = 'l';
    module[i] = 0;

    int j = 0;
    for (i = dot + 1; forwarder[i] && j < (int)sizeof(symbol) - 1; i++)
        symbol[j++] = forwarder[i];
    if (!j || forwarder[i]) return 0; /* never resolve a truncated symbol */
    symbol[j] = 0;

    UINT64 target = LdrpLoadModule(module);
    if (!target)
        return 0;
    if (symbol[0] == '#') {
        UINT32 ordinal = 0;
        for (i = 1; symbol[i] >= '0' && symbol[i] <= '9'; i++) {
            ordinal = ordinal * 10 + (UINT32)(symbol[i] - '0');
            if (ordinal > 65535) return 0;
        }
        if (i == 1 || symbol[i]) return 0;
        return LdrpProcByOrdinal(target, (UINT16)ordinal);
    }
    return LdrGetProcAddress(target, symbol);
}

static UINT64 LdrpExportRvaToAddress(UINT64 base, UINT32 rva,
                                     IMAGE_DATA_DIRECTORY export_dir)
{
    /* PE export tables reserve ordinals by leaving their function RVA zero.
     * Returning `base + 0` turns such an unavailable ordinal into a call to
     * the DLL's DOS header. Treat reserved slots as unavailable exports. */
    if (rva == 0 || !LdrpImageContains(base, rva, 1))
        return 0;
    /* An RVA pointing back into the export directory is a forwarder string,
     * not executable code. Real USER32/GDI32/SHELL32 use these extensively. */
    if (rva >= export_dir.VirtualAddress &&
        (UINT64)rva < (UINT64)export_dir.VirtualAddress + export_dir.Size) {
        const char *forwarder = LdrpImageString(base, rva);
        if (!forwarder) return 0;
        /* Forwarder cycles must not exhaust the small kernel stack. */
        if (g_ldr_depth >= LDR_MAX_FORWARDER_DEPTH) return 0;
        g_ldr_depth++;
        UINT64 address = LdrpResolveForwarder(forwarder);
        g_ldr_depth--;
        return address;
    }
    return base + rva;
}

UINT64 LdrGetProcAddress(UINT64 base, const char *name)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    if (!nt)
        return 0;
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
        !LdrpImageContains(base, dir.VirtualAddress, dir.Size))
        return 0;

    IMAGE_EXPORT_DIRECTORY exports;
    memcpy(&exports, (void *)(base + dir.VirtualAddress), sizeof(exports));
    IMAGE_EXPORT_DIRECTORY *ed = &exports;
    if (!LdrpImageContains(base, ed->AddressOfNames, (UINT64)ed->NumberOfNames * 4) ||
        !LdrpImageContains(base, ed->AddressOfNameOrdinals, (UINT64)ed->NumberOfNames * 2) ||
        !LdrpImageContains(base, ed->AddressOfFunctions, (UINT64)ed->NumberOfFunctions * 4)) return 0;

    for (UINT32 i = 0; i < ed->NumberOfNames; i++) {
        UINT32 name_rva, function_rva;
        UINT16 ordinal;
        /* lld emits valid export arrays at unaligned RVAs. */
        memcpy(&name_rva, (void *)(base + ed->AddressOfNames + (UINT64)i * 4), 4);
        memcpy(&ordinal, (void *)(base + ed->AddressOfNameOrdinals + (UINT64)i * 2), 2);
        const char *export_name = LdrpImageString(base, name_rva);
        if (!export_name || ordinal >= ed->NumberOfFunctions) return 0;
        if (strcmp(export_name, name) == 0) {
            memcpy(&function_rva, (void *)(base + ed->AddressOfFunctions + (UINT64)ordinal * 4), 4);
            return LdrpExportRvaToAddress(base, function_rva, dir);
        }
    }
    return 0;
}

static UINT64 LdrpProcByOrdinal(UINT64 base, UINT16 ordinal)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    if (!nt) return 0;
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
        !LdrpImageContains(base, dir.VirtualAddress, dir.Size))
        return 0;
    IMAGE_EXPORT_DIRECTORY exports;
    memcpy(&exports, (void *)(base + dir.VirtualAddress), sizeof(exports));
    IMAGE_EXPORT_DIRECTORY *ed = &exports;
    if (!LdrpImageContains(base, ed->AddressOfFunctions,
                          (UINT64)ed->NumberOfFunctions * 4)) return 0;
    UINT32 index = ordinal - ed->Base;
    if (index >= ed->NumberOfFunctions)
        return 0;
    UINT32 function_rva;
    memcpy(&function_rva, (void *)(base + ed->AddressOfFunctions + (UINT64)index * 4), 4);
    return LdrpExportRvaToAddress(base, function_rva, dir);
}

/* CFG bootstrap trampolines. This arena must not be used
 * to manufacture missing imports. W^X/CFG enforcement remains separate work. */
#define LDR_STUB_ARENA_VA    0x0000000000069000ULL
#define LDR_STUB_ARENA_PAGES 32     /* legacy CFG bootstrap arena; not callbacks */
#define LDR_STUB_SLOT_SIZE   16
#define LDR_CFG_TRACE_VA     (LDR_STUB_ARENA_VA + \
                              LDR_STUB_ARENA_PAGES * PAGE_SIZE - 8)

static BOOLEAN LdrpEnsureStubArena(void)
{
    if (!g_stub_arena) {
        if (!image_range_free(LDR_STUB_ARENA_VA,
                              LDR_STUB_ARENA_PAGES * PAGE_SIZE) ||
            !NT_SUCCESS(LdrpAllocateImagePages(LDR_STUB_ARENA_VA,
                              LDR_STUB_ARENA_PAGES * PAGE_SIZE)))
            return FALSE;
        memset((void *)LDR_STUB_ARENA_VA, 0,
               LDR_STUB_ARENA_PAGES * PAGE_SIZE);
        g_stub_arena = LDR_STUB_ARENA_VA;
        /* The Windows loader initializes these two process-wide CFG targets.
         * Guard-check is a no-op (`ret`); guard-dispatch transfers control to
         * the compiler-supplied target in RAX (`jmp rax`). */
        UINT8 *code = (UINT8 *)LDR_STUB_ARENA_VA;
        code[0] = 0xC3;
        /* Record the most recent CFG target, then jump to it. `mov moffs64,
         * rax` preserves all call arguments and makes a later fault actionable
         * without a syscall on every indirect call. */
        code[LDR_STUB_SLOT_SIZE + 0] = 0x48;
        code[LDR_STUB_SLOT_SIZE + 1] = 0xA3;
        *(UINT64 *)(code + LDR_STUB_SLOT_SIZE + 2) = LDR_CFG_TRACE_VA;
        code[LDR_STUB_SLOT_SIZE + 10] = 0xFF;
        code[LDR_STUB_SLOT_SIZE + 11] = 0xE0;
    }
    return TRUE;
}

static BOOLEAN LdrpNameContains(const char *name, const char *needle)
{
    for (; *name; name++) {
        const char *n = needle, *m = name;
        while (*n && *m && *n == *m) {
            n++;
            m++;
        }
        if (!*n)
            return TRUE;
    }
    return FALSE;
}

/* Patch the GuardCF function-pointer variables named by a PE32+ load-config
 * directory. MSVC emits indirect calls through these variables; leaving them
 * zero makes otherwise valid inbox DLL worker threads jump to address zero. */
static void LdrpInitializeGuardPointers(UINT64 base)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    if (!dir.VirtualAddress || dir.Size < 0x80)
        return;

    UINT8 *config = (UINT8 *)(base + dir.VirtualAddress);
    UINT32 declared_size = *(UINT32 *)config;
    if (declared_size < 0x80 || !LdrpEnsureStubArena())
        return;

    UINT64 check_slot = *(UINT64 *)(config + 0x70);
    UINT64 dispatch_slot = *(UINT64 *)(config + 0x78);
    BOOLEAN patched = FALSE;
    if (check_slot && MmProbeForWrite(check_slot, sizeof(UINT64))) {
        *(UINT64 *)check_slot = LDR_STUB_ARENA_VA;
        patched = TRUE;
    }
    if (dispatch_slot && MmProbeForWrite(dispatch_slot, sizeof(UINT64))) {
        *(UINT64 *)dispatch_slot = LDR_STUB_ARENA_VA + LDR_STUB_SLOT_SIZE;
        patched = TRUE;
    }
    if (patched)
        KeLog("[ldr]  CFG thunks initialized for image %p (check %p, dispatch %p)\n",
              (void *)base, (void *)check_slot, (void *)dispatch_slot);
}

/* Resolve a few API-set aliases against hosts that have already been loaded,
 * without adding another initialization dependency edge. These contracts are
 * consumed late by Shell/OLE and adding a synthetic edge for them creates a
 * cycle that changes the proven USER32/combase attach order. */
static UINT64 LdrpLateApiSetHost(const char *name)
{
    /* The USER/GDI surface ships under many contract names (ntuser window,
     * windowclass, message, menu, synch, sysparams, ... plus the gdi families).
     * They all live in the genuine user32/gdi32, but only bind if that host is
     * already in the module cache, so no initialization edge is added. */
    if (LdrpNameContains(name, "ntuser"))
        return lookup_module("user32.dll");
    return 0;
}

static NTSTATUS LdrResolveImports(UINT64 base)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.Size == 0)
        return STATUS_SUCCESS; /* nothing to import */
    if (!LdrpImageContains(base, dir.VirtualAddress, dir.Size) ||
        (dir.VirtualAddress & 3)) return STATUS_INVALID_IMAGE_FORMAT;

    IMAGE_IMPORT_DESCRIPTOR *desc =
        (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);

    UINT32 count = dir.Size / sizeof(*desc);
    for (UINT32 d = 0; d < count; d++, desc++) {
        if (!desc->Name) return STATUS_SUCCESS;
        const char *dll = LdrpImageString(base, desc->Name);
        if (!dll || !*dll) return STATUS_INVALID_IMAGE_FORMAT;
        UINT64 dll_base = LdrpLateApiSetHost(dll);
        BOOLEAN late_alias = dll_base != 0;
        if (!dll_base)
            dll_base = LdrpLoadModule(dll);
        if (dll_base && !late_alias)
            remember_dependency(base, dll_base);
        if (!dll_base) {
            KeLog("[ldr]  import dependency '%s' could not be loaded\n",
                  dll);
            return LdrpCurrentContext()->LoadStatus;
        }

        UINT32 int_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk;
        if (!int_rva || !desc->FirstThunk || (int_rva & 7) ||
            (desc->FirstThunk & 7)) return STATUS_INVALID_IMAGE_FORMAT;
        UINT64 *names = (UINT64 *)(base + int_rva);
        UINT64 *iat = (UINT64 *)(base + desc->FirstThunk);

        for (UINT32 i = 0; ; i++) {
            if (!LdrpImageContains(base, (UINT64)int_rva + (UINT64)i * 8, 8) ||
                !LdrpImageContains(base, (UINT64)desc->FirstThunk + (UINT64)i * 8, 8))
                return STATUS_INVALID_IMAGE_FORMAT;
            if (!names[i]) break;
            UINT64 thunk = names[i];
            UINT64 addr = 0;
            if (thunk & IMAGE_ORDINAL_FLAG64) {
                UINT16 ord = (UINT16)(thunk & 0xFFFF);
                if (dll_base)
                    addr = LdrpProcByOrdinal(dll_base, ord);
                if (!addr) {
                    KeLog("[ldr]  missing import %s!#%u\n", dll, ord);
                    return STATUS_ORDINAL_NOT_FOUND;
                }
            } else {
                if (!LdrpImageContains(base, thunk, 2)) return STATUS_INVALID_IMAGE_FORMAT;
                const char *symbol = LdrpImageString(base, thunk + 2);
                if (!symbol || !*symbol) return STATUS_INVALID_IMAGE_FORMAT;
                if (dll_base)
                    addr = LdrGetProcAddress(dll_base, symbol);
                /* The core COM contract is temporarily hosted by kernel32 for
                 * allocator/bootstrap calls, but object activation must use
                 * the genuine combase implementation once it is loaded. */
                if (strncmp(dll, "api-ms-win-core-com-",
                            sizeof("api-ms-win-core-com-") - 1) == 0 &&
                    strcmp(symbol, "CoCreateInstance") == 0) {
                    UINT64 combase = lookup_module("combase.dll");
                    if (combase)
                        addr = LdrGetProcAddress(combase, symbol);
                }
                /* Windows ntdll also exports a CRT-like leaf surface. Our
                 * compact build keeps those routines in msvcrt, so use it as
                 * an explicit compatibility host fallback. */
                if (!addr && ci_strcmp(dll, "ntdll.dll") == 0) {
                    UINT64 crt = lookup_module("msvcrt.dll");
                    if (crt)
                        addr = LdrGetProcAddress(crt, symbol);
                }
                if (!addr) {
                    KeLog("[ldr]  missing import %s!%s\n", dll, symbol);
                    return STATUS_ENTRYPOINT_NOT_FOUND;
                }
            }
            iat[i] = addr;
        }
        KeLog("[ldr]  linked imports from %s (base %p)\n", dll,
              (void *)dll_base);
    }
    return STATUS_INVALID_IMAGE_FORMAT; /* directory lacks a terminator */
}

/* Resolve MSVC delay imports eagerly.  Windows normally patches each delay-IAT
 * slot on its first call through __delayLoadHelper2; eager binding is simpler
 * for the kernel loader and produces the same observable function pointers
 * before user code begins. */
static NTSTATUS LdrResolveDelayImports(UINT64 base)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (dir.Size == 0)
        return STATUS_SUCCESS;
    if (!LdrpImageContains(base, dir.VirtualAddress, dir.Size) ||
        (dir.VirtualAddress & 3)) return STATUS_INVALID_IMAGE_FORMAT;

    IMAGE_DELAYLOAD_DESCRIPTOR *desc =
        (IMAGE_DELAYLOAD_DESCRIPTOR *)(base + dir.VirtualAddress);
    UINT32 count = dir.Size / sizeof(*desc);

    for (UINT32 d = 0; d < count; d++) {
        if (!desc[d].DllNameRva) return STATUS_SUCCESS;
        /* PE32+ images produced by modern MSVC use RVA-based descriptors. */
        if (!(desc[d].Attributes & 1)) {
            KeLog("[ldr]  unsupported VA-based delay import descriptor\n");
            return STATUS_INVALID_IMAGE_FORMAT;
        }

        const char *dll = LdrpImageString(base, desc[d].DllNameRva);
        if (!dll || !*dll) return STATUS_INVALID_IMAGE_FORMAT;
        UINT64 dll_base = LdrpLateApiSetHost(dll);
        BOOLEAN late_alias = dll_base != 0;
        if (!dll_base)
            dll_base = LdrpLoadModule(dll);
        if (dll_base && !late_alias)
            remember_dependency(base, dll_base);
        if (!dll_base) {
            KeLog("[ldr]  delay import dependency '%s' could not be loaded\n", dll);
            return LdrpCurrentContext()->LoadStatus;
        }
        if (!desc[d].ImportNameTableRva || !desc[d].ImportAddressTableRva ||
            (desc[d].ImportNameTableRva & 7) || (desc[d].ImportAddressTableRva & 7))
            return STATUS_INVALID_IMAGE_FORMAT;
        UINT64 *names = (UINT64 *)(base + desc[d].ImportNameTableRva);
        UINT64 *iat = (UINT64 *)(base + desc[d].ImportAddressTableRva);
        UINT32 linked = 0;

        if (desc[d].ModuleHandleRva) {
            if ((desc[d].ModuleHandleRva & 7) ||
                !LdrpImageContains(base, desc[d].ModuleHandleRva, 8))
                return STATUS_INVALID_IMAGE_FORMAT;
            *(UINT64 *)(base + desc[d].ModuleHandleRva) = dll_base;
        }

        for (UINT32 i = 0; ; i++) {
            if (!LdrpImageContains(base, (UINT64)desc[d].ImportNameTableRva + (UINT64)i * 8, 8) ||
                !LdrpImageContains(base, (UINT64)desc[d].ImportAddressTableRva + (UINT64)i * 8, 8))
                return STATUS_INVALID_IMAGE_FORMAT;
            if (!names[i]) break;
            UINT64 thunk = names[i];
            UINT64 addr = 0;
            if (thunk & IMAGE_ORDINAL_FLAG64) {
                UINT16 ordinal = (UINT16)(thunk & 0xFFFF);
                if (dll_base)
                    addr = LdrpProcByOrdinal(dll_base, ordinal);
                if (!addr) {
                    KeLog("[ldr]  missing delay import %s!#%u\n", dll, ordinal);
                    return STATUS_ORDINAL_NOT_FOUND;
                }
            } else {
                if (!LdrpImageContains(base, thunk, 2)) return STATUS_INVALID_IMAGE_FORMAT;
                const char *symbol = LdrpImageString(base, thunk + 2);
                if (!symbol || !*symbol) return STATUS_INVALID_IMAGE_FORMAT;
                if (dll_base)
                    addr = LdrGetProcAddress(dll_base, symbol);
                if (strncmp(dll, "api-ms-win-core-com-",
                            sizeof("api-ms-win-core-com-") - 1) == 0 &&
                    strcmp(symbol, "CoCreateInstance") == 0) {
                    UINT64 combase = lookup_module("combase.dll");
                    if (combase)
                        addr = LdrGetProcAddress(combase, symbol);
                }
                if (!addr) {
                    KeLog("[ldr]  missing delay import %s!%s\n", dll, symbol);
                    return STATUS_ENTRYPOINT_NOT_FOUND;
                }
            }
            linked++;
            iat[i] = addr;
        }

        KeLog("[ldr]  delay imports from %s: %u linked\n", dll, linked);
    }
    return STATUS_INVALID_IMAGE_FORMAT; /* directory lacks a terminator */
}

static UINT64 lookup_module(const char *name)
{
    for (int i = 0; i < MAX_LOADED_MODULES; i++)
        if (g_loaded[i].Valid && ci_strcmp(name, g_loaded[i].Name) == 0)
            return g_loaded[i].Base;
    return 0;
}

static int module_index_by_base(UINT64 base)
{
    for (int i = 0; i < MAX_LOADED_MODULES; i++)
        if (g_loaded[i].Valid && g_loaded[i].Base == base)
            return i;
    return -1;
}

static void remember_dependency(UINT64 importer, UINT64 dependency)
{
    if (!importer || !dependency || importer == dependency)
        return;
    int from = module_index_by_base(importer);
    int to = module_index_by_base(dependency);
    if (from >= 0 && to >= 0)
        g_loaded[from].Dependencies |= 1ULL << to;
}

static BOOLEAN remember_module(const char *name, UINT64 base)
{
    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        if (g_loaded[i].Valid)
            continue;
        int j = 0;
        for (; j < 31 && name[j]; j++)
            g_loaded[i].Name[j] = name[j];
        g_loaded[i].Name[j] = 0;
        g_loaded[i].Base = base;
        g_loaded[i].Valid = TRUE;
        LdrpRegisterTls(base);
        return TRUE;
    }
    return FALSE;
}

/* Load a dependency module by name from disk (or return the cached base). */
/*
 * The loader recurses once per dependency edge, on the single kernel boot
 * stack, and that stack sits directly above the executive's own globals in
 * .bss -- so an overflow silently rewrites the pool's free list. A guard word
 * below the stack turns that into a named bugcheck, and a depth cap stops the
 * descent before a pathological graph gets there at all.
 */
extern UINT64 kernel_stack_guard[2];
#define LDR_STACK_GUARD_MAGIC 0x4B535447445241ULL
#define LDR_MAX_DEPENDENCY_DEPTH 96

static UINT64 LdrpLoadModuleWorker(const char *name);

void LdrInitializeStackGuard(void)
{
    kernel_stack_guard[0] = LDR_STACK_GUARD_MAGIC;
    kernel_stack_guard[1] = LDR_STACK_GUARD_MAGIC;
}

static void LdrpCheckStackGuard(const char *name)
{
    if (kernel_stack_guard[0] == LDR_STACK_GUARD_MAGIC &&
        kernel_stack_guard[1] == LDR_STACK_GUARD_MAGIC)
        return;
    KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
               "kernel stack overflowed while loading a module");
    (void)name;
}

static UINT64 LdrpLoadModule(const char *name)
{
    LdrpCheckStackGuard(name);
    if (g_ldr_depth >= LDR_MAX_DEPENDENCY_DEPTH) {
        KeLog("[ldr]  dependency chain deeper than %u while loading '%s'; "
              "refusing to recurse further\n",
              (unsigned)LDR_MAX_DEPENDENCY_DEPTH, name);
        LdrpCurrentContext()->LoadStatus = STATUS_INSUFFICIENT_RESOURCES;
        return 0;
    }
    g_ldr_depth++;
    UINT64 result = LdrpLoadModuleWorker(name);
    g_ldr_depth--;
    return result;
}

static UINT64 LdrpLoadModuleWorker(const char *name)
{
    /* Modern Windows binaries import API-set contract names rather than the
     * host DLL directly.  Route each implemented contract family to its real
     * SVINDOWS host instead of treating every API set as kernel32. */
    if (strncmp(name, "api-ms-win-core-registry-",
                sizeof("api-ms-win-core-registry-") - 1) == 0)
        return LdrpLoadModule("advapi32.dll");

    /* Resolve this API-set family only if its actual host DLL is available. */
    if (strncmp(name, "api-ms-win-storage-exports-",
                sizeof("api-ms-win-storage-exports-") - 1) == 0)
        return LdrpLoadModule("windows.storage.dll");

    /* Universal CRT contracts are API-set names whose inbox host is
     * ucrtbase.dll. Windows.Storage and the shell use the private/runtime and
     * string families during their worker-thread bootstrap. */
    if (strncmp(name, "api-ms-win-crt-",
                sizeof("api-ms-win-crt-") - 1) == 0)
        return LdrpLoadModule("ucrtbase.dll");

    /* Windows system DLLs forward a large part of their surface to
     * KERNELBASE. Until we run the real KERNELBASE, our kernel32 facade is the
     * host for those core contracts. */
    if (ci_strcmp(name, "kernelbase.dll") == 0)
        return LdrpLoadModule("kernel32.dll");

    /* GDI32's private kernel-facing contract is hosted by win32u.dll. */
    if (LdrpNameContains(name, "gdi-internal-uap") ||
        LdrpNameContains(name, "gdi-internal-desktop"))
        return LdrpLoadModule("win32u.dll");

    /* Public GDI contract families are hosted by gdi32.dll. */
    if (LdrpNameContains(name, "gdi-"))
        return LdrpLoadModule("gdi32.dll");

    /* Most of the core contracts implemented so far live in kernel32. */
    if (strncmp(name, "api-ms-win-core-", 16) == 0 ||
        strncmp(name, "ext-ms-win-core-", 16) == 0)
        return LdrpLoadModule("kernel32.dll");
    /* VERSION's implemented subset currently lives in kernel32; OLE32 must
     * resolve to its own DLL rather than an unrelated compatibility host. */
    if (ci_strcmp(name, "version.dll") == 0)
        return LdrpLoadModule("kernel32.dll");

    UINT64 existing = lookup_module(name);
    if (existing)
        return existing;

    void *buf;
    SIZE_T size;
    if (!NT_SUCCESS(FatLoadFile(name, &buf, &size, NULL))) {
        KeLog("[ldr]  dependency '%s' not found on disk\n", name);
        LdrpCurrentContext()->LoadStatus = STATUS_DLL_NOT_FOUND;
        return 0;
    }

    UINT64 base;
    NTSTATUS status = LdrpMapImage(buf, size, &base);
    ExFreePool(buf); /* mapped into user pages; the file buffer is done */
    if (!NT_SUCCESS(status)) {
        LdrpCurrentContext()->LoadStatus = status;
        return 0;
    }

    if (!remember_module(name, base)) { /* cache before resolving to break cycles */
        LdrpReleaseImagePages(base, PAGE_ALIGN_UP(
            nt_headers(base)->OptionalHeader.SizeOfImage));
        LdrpCurrentContext()->LoadStatus = STATUS_INSUFFICIENT_RESOURCES;
        return 0;
    }


    status = LdrResolveImports(base);
    if (NT_SUCCESS(status)) status = LdrResolveDelayImports(base);
    if (!NT_SUCCESS(status)) {
        LdrpCurrentContext()->LoadStatus = status;
        return 0;
    }
    LdrpInitializeGuardPointers(base);
    LdrpProtectImage(base);

    KeLog("[ldr]  loaded module %s at %p\n", name, (void *)base);
    return base;
}
