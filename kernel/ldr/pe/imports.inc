/* PE exports, imports, stubs, dependency loading and bootstrap.
 * Included by kernel/ldr/pe.c. */
/* ------------------------------------------------------------------ */
/* Exports and imports                                                */
/* ------------------------------------------------------------------ */

/* Forward declarations: forwarded exports may recursively load their target
 * module and resolve either a name or an ordinal there. */
static UINT64 LdrpLoadModule(const char *name);
static UINT64 LdrpProcByOrdinal(UINT64 base, UINT16 ordinal);
static UINT64 lookup_module(const char *name);

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
    symbol[j] = 0;

    UINT64 target = LdrpLoadModule(module);
    if (!target)
        return 0;
    if (symbol[0] == '#') {
        UINT32 ordinal = 0;
        for (i = 1; symbol[i] >= '0' && symbol[i] <= '9'; i++)
            ordinal = ordinal * 10 + (UINT32)(symbol[i] - '0');
        return LdrpProcByOrdinal(target, (UINT16)ordinal);
    }
    return LdrGetProcAddress(target, symbol);
}

static UINT64 LdrpExportRvaToAddress(UINT64 base, UINT32 rva,
                                     IMAGE_DATA_DIRECTORY export_dir)
{
    /* An RVA pointing back into the export directory is a forwarder string,
     * not executable code. Real USER32/GDI32/SHELL32 use these extensively. */
    if (rva >= export_dir.VirtualAddress &&
        rva < export_dir.VirtualAddress + export_dir.Size)
        return LdrpResolveForwarder((const char *)(base + rva));
    return base + rva;
}

UINT64 LdrGetProcAddress(UINT64 base, const char *name)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    if (!nt)
        return 0;
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.Size == 0)
        return 0;

    IMAGE_EXPORT_DIRECTORY *ed =
        (IMAGE_EXPORT_DIRECTORY *)(base + dir.VirtualAddress);
    UINT32 *names = (UINT32 *)(base + ed->AddressOfNames);
    UINT16 *ordinals = (UINT16 *)(base + ed->AddressOfNameOrdinals);
    UINT32 *funcs = (UINT32 *)(base + ed->AddressOfFunctions);

    for (UINT32 i = 0; i < ed->NumberOfNames; i++) {
        const char *export_name = (const char *)(base + names[i]);
        if (strcmp(export_name, name) == 0)
            return LdrpExportRvaToAddress(base, funcs[ordinals[i]], dir);
    }
    return 0;
}

static UINT64 LdrpProcByOrdinal(UINT64 base, UINT16 ordinal)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.Size == 0)
        return 0;
    IMAGE_EXPORT_DIRECTORY *ed =
        (IMAGE_EXPORT_DIRECTORY *)(base + dir.VirtualAddress);
    UINT32 index = ordinal - ed->Base;
    if (index >= ed->NumberOfFunctions)
        return 0;
    UINT32 *funcs = (UINT32 *)(base + ed->AddressOfFunctions);
    return LdrpExportRvaToAddress(base, funcs[index], dir);
}

/* A per-import stub arena in low user memory. Each unresolved import gets its
 * own 16-byte slot, so a single arena serves imports that are functions AND
 * imports that are data:
 *
 *   - A missing FUNCTION is pointed at its slot and called: the slot's first
 *     bytes are `xor eax, eax; ret`, so the call harmlessly returns 0.
 *   - A missing DATA export (e.g. msvcrt's `_fmode`, `_commode`) has an IAT slot
 *     that is a *pointer to the variable*; the image dereferences it and may
 *     read or WRITE through it. Pointing it at its own slot means such a write
 *     lands in that slot alone and cannot corrupt any other stub.
 *
 * The arena is therefore mapped writable *and* executable (a deliberate W^X
 * exception for this bootstrap shim). Giving every import a distinct slot is
 * what makes the two uses coexist: a slot used as data is never executed, and a
 * slot used as code is never written. */
#define LDR_STUB_ARENA_VA    0x0000000000069000ULL
#define LDR_STUB_ARENA_PAGES 32     /* 32 * 4096 / 16 = 8192 stub slots */
#define LDR_STUB_SLOT_SIZE   16
#define LDR_CFG_TRACE_VA     (LDR_STUB_ARENA_VA + \
                              LDR_STUB_ARENA_PAGES * PAGE_SIZE - 8)

static BOOLEAN LdrpEnsureStubArena(void)
{
    if (!g_stub_arena) {
        for (UINT64 i = 0; i < LDR_STUB_ARENA_PAGES; i++) {
            UINT64 pa = MmAllocatePage();
            if (pa == MM_INVALID_PHYS)
                return FALSE;
            MmMapPage(LDR_STUB_ARENA_VA + i * PAGE_SIZE, pa,
                      PTE_USER | PTE_WRITE);
        }
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
        g_stub_next = LDR_STUB_ARENA_VA + 2 * LDR_STUB_SLOT_SIZE;
        g_stub_end = LDR_CFG_TRACE_VA;
    }
    return TRUE;
}

/* Accessors for the kernel's user-callback trampolines (ke/syscall.c). */
BOOLEAN LdrpEnsureStubArenaPub(void)
{
    return LdrpEnsureStubArena();
}

UINT64 LdrStubArenaBase(void)
{
    return g_stub_arena;
}

UINT64 LdrStubArenaEnd(void)
{
    return g_stub_end;
}

UINT64 LdrStubArenaNext(UINT64 advance)
{
    UINT64 cur = g_stub_next;
    if (advance)
        g_stub_next += advance;
    return cur;
}

static UINT64 LdrpImportStub(void)
{
    if (!LdrpEnsureStubArena())
        return 0;

    /* Out of slots: fall back to the first one. Sharing degrades correctness for
     * data imports but keeps the image loadable rather than failing outright. */
    if (g_stub_next + LDR_STUB_SLOT_SIZE > g_stub_end)
        return g_stub_arena;
    UINT64 slot = g_stub_next;
    g_stub_next += LDR_STUB_SLOT_SIZE;
    UINT8 *code = (UINT8 *)slot;
    code[0] = 0x31; /* xor eax, eax */
    code[1] = 0xC0;
    code[2] = 0xC3; /* ret */
    return slot;
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

/*
 * Boundary-trace trampoline (see svindows/trace.h): publish the tag, notify the
 * kernel, restore RCX (the syscall entry moves it to R10), and tail-jump to
 * the real function. Layout (three arena slots, 48 bytes):
 *   +0x00  mov dword [rip+rel32], tag     ; C7 05 rel32 imm32
 *   +0x0A  mov eax, SVINDOWS_TRACE_SYSCALL    ; B8 imm32
 *   +0x0F  syscall                        ; 0F 05
 *   +0x11  mov rcx, r10                   ; 49 89 D1
 *   +0x14  jmp qword [rip+0]              ; FF 25 00000000
 *   +0x20  target function address
 *   +0x28  (the tag word itself lives here via the rel32 above)
 */
static UINT64 LdrpTraceThunk(UINT32 tag, UINT64 target,
                                const char *name)
{
    if (!LdrpEnsureStubArena() ||
        g_stub_next + 3 * LDR_STUB_SLOT_SIZE > g_stub_end)
        return target;

    UINT64 slot = g_stub_next;
    g_stub_next += 3 * LDR_STUB_SLOT_SIZE;
    UINT8 *code = (UINT8 *)slot;
    UINT64 tag_va = SVINDOWS_TRACE_TAG_VA; /* fixed address the kernel probes */

    code[0x00] = 0x49; /* mov r10, rcx: the syscall ABI takes arg1 in r10 */
    code[0x01] = 0x89;
    code[0x02] = 0xCA;
    code[0x03] = 0xC7; /* mov dword [rip+rel32], tag */
    code[0x04] = 0x05;
    *(INT32 *)(code + 0x05) = (INT32)(tag_va - (slot + 0x0D));
    *(UINT32 *)(code + 0x09) = tag;
    code[0x0D] = 0xB8; /* mov eax, SVINDOWS_TRACE_SYSCALL */
    *(UINT32 *)(code + 0x0E) = SVINDOWS_TRACE_SYSCALL;
    code[0x12] = 0x0F; /* syscall */
    code[0x13] = 0x05;
    code[0x14] = 0x49; /* mov rcx, r10: restore the caller's arg1 */
    code[0x15] = 0x89;
    code[0x16] = 0xD1;
    code[0x17] = 0xFF; /* jmp qword [rip+0xB] -> target at +0x28 */
    code[0x18] = 0x25;
    *(UINT32 *)(code + 0x19) = 0x28 - 0x1D;
    *(UINT64 *)(code + 0x24) = target; /* mirrored at +0x28 */
    *(UINT64 *)(code + 0x28) = target;
    KeLog("[ldr]  trace thunk %s slot=%p target=%p\n", name ? name : "?",
          (void *)slot, (void *)target);
    return slot;
}

/* USER-surface imports wrapped with a boundary trace while the shell bring-up
 * is being debugged. The tag numbers mirror the kernel's name table. */
static UINT64 LdrpMaybeTraceImport(const char *dll, const char *name,
                                   UINT64 addr)
{
    struct {
        const char *Name;
        UINT32 Tag;
    } traced[] = {
        { "RegisterClassW",    SVINDOWS_TRACE_RegisterClassW },
        { "RegisterClassExW",  SVINDOWS_TRACE_RegisterClassExW },
        { "CreateWindowExW",   SVINDOWS_TRACE_CreateWindowExW },
        { "DestroyWindow",     SVINDOWS_TRACE_DestroyWindow },
        { "PostThreadMessageW", SVINDOWS_TRACE_PostThreadMessageW },
        { "PostMessageW",      SVINDOWS_TRACE_PostMessageW },
        /* DefWindowProcW is deliberately NOT wrapped: user32 exports it as a
         * forwarder to ntdll!NtdllDefWindowProc_W, and substituting an arena
         * thunk for the forwarded pointer deranges user32's internal class
         * tables (observed as thunk bytes surfacing in WNDCLASS copies). */
        { "DispatchMessageW",  SVINDOWS_TRACE_DispatchMessageW },
    };
    (void)traced;
    return addr; /* tracing disabled: arena thunks derange user32 internals */
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

    IMAGE_IMPORT_DESCRIPTOR *desc =
        (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);

    for (; desc->Name != 0; desc++) {
        const char *dll = (const char *)(base + desc->Name);
        UINT64 dll_base = LdrpLateApiSetHost(dll);
        BOOLEAN late_alias = dll_base != 0;
        if (!dll_base)
            dll_base = LdrpLoadModule(dll);
        if (dll_base && !late_alias)
            remember_dependency(base, dll_base);
        /* A dependency DLL we don't provide no longer fails the load: every
         * import from it is stubbed (and logged), just like an individual
         * missing function. */
        if (!dll_base)
            KeLog("[ldr]  dependency '%s' not found -> stubbing its imports\n",
                  dll);

        UINT32 int_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk;
        UINT64 *names = (UINT64 *)(base + int_rva);
        UINT64 *iat = (UINT64 *)(base + desc->FirstThunk);

        UINT32 stubbed = 0, linked = 0;
        for (UINT32 i = 0; names[i]; i++) {
            UINT64 thunk = names[i];
            UINT64 addr = 0;
            if (thunk & IMAGE_ORDINAL_FLAG64) {
                UINT16 ord = (UINT16)(thunk & 0xFFFF);
                if (dll_base)
                    addr = LdrpProcByOrdinal(dll_base, ord);
                if (!addr)
                    KeLog("[ldr]  STUB %s!#%u\n", dll, ord);
            } else {
                IMAGE_IMPORT_BY_NAME *ibn =
                    (IMAGE_IMPORT_BY_NAME *)(base + (thunk & 0x7FFFFFFF));
                if (dll_base)
                    addr = LdrGetProcAddress(dll_base, ibn->Name);
                if (addr)
                    addr = LdrpMaybeTraceImport(dll, ibn->Name, addr);
                /* The core COM contract is temporarily hosted by kernel32 for
                 * allocator/bootstrap calls, but object activation must use
                 * the genuine combase implementation once it is loaded. */
                if (strncmp(dll, "api-ms-win-core-com-",
                            sizeof("api-ms-win-core-com-") - 1) == 0 &&
                    strcmp(ibn->Name, "CoCreateInstance") == 0) {
                    UINT64 combase = lookup_module("combase.dll");
                    if (combase)
                        addr = LdrGetProcAddress(combase, ibn->Name);
                }
                /* Windows ntdll also exports a CRT-like leaf surface. Our
                 * compact build keeps those routines in msvcrt, so use it as
                 * a compatibility fallback before manufacturing a stub. */
                if (!addr && ci_strcmp(dll, "ntdll.dll") == 0) {
                    UINT64 crt = lookup_module("msvcrt.dll");
                    if (crt)
                        addr = LdrGetProcAddress(crt, ibn->Name);
                }
                if (!addr)
                    KeLog("[ldr]  STUB %s!%s\n", dll, ibn->Name);
            }
            /* Unresolved imports get a return-0 stub so the image still loads;
             * the log above is the to-do list of what a binary actually needs. */
            if (!addr) {
                addr = LdrpImportStub();
                stubbed++;
            } else {
                linked++;
            }
            iat[i] = addr;
        }
        if (stubbed)
            KeLog("[ldr]  %s: %u linked, %u stubbed\n", dll, linked, stubbed);
        else
            KeLog("[ldr]  linked imports from %s (base %p)\n", dll,
                  (void *)dll_base);
    }
    return STATUS_SUCCESS;
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

    IMAGE_DELAYLOAD_DESCRIPTOR *desc =
        (IMAGE_DELAYLOAD_DESCRIPTOR *)(base + dir.VirtualAddress);
    UINT32 count = dir.Size / sizeof(*desc);

    for (UINT32 d = 0; d < count && desc[d].DllNameRva; d++) {
        /* PE32+ images produced by modern MSVC use RVA-based descriptors. */
        if (!(desc[d].Attributes & 1)) {
            KeLog("[ldr]  unsupported VA-based delay import descriptor\n");
            return STATUS_INVALID_IMAGE_FORMAT;
        }

        const char *dll = (const char *)(base + desc[d].DllNameRva);
        UINT64 dll_base = LdrpLateApiSetHost(dll);
        BOOLEAN late_alias = dll_base != 0;
        if (!dll_base)
            dll_base = LdrpLoadModule(dll);
        if (dll_base && !late_alias)
            remember_dependency(base, dll_base);
        UINT64 *names = (UINT64 *)(base + desc[d].ImportNameTableRva);
        UINT64 *iat = (UINT64 *)(base + desc[d].ImportAddressTableRva);
        UINT32 linked = 0, stubbed = 0;

        if (desc[d].ModuleHandleRva)
            *(UINT64 *)(base + desc[d].ModuleHandleRva) = dll_base;

        for (UINT32 i = 0; names[i]; i++) {
            UINT64 thunk = names[i];
            UINT64 addr = 0;
            if (thunk & IMAGE_ORDINAL_FLAG64) {
                UINT16 ordinal = (UINT16)(thunk & 0xFFFF);
                if (dll_base)
                    addr = LdrpProcByOrdinal(dll_base, ordinal);
                if (!addr)
                    KeLog("[ldr]  DELAY STUB %s!#%u\n", dll, ordinal);
            } else {
                IMAGE_IMPORT_BY_NAME *ibn =
                    (IMAGE_IMPORT_BY_NAME *)(base + (UINT32)thunk);
                if (dll_base)
                    addr = LdrGetProcAddress(dll_base, ibn->Name);
                if (addr)
                    addr = LdrpMaybeTraceImport(dll, ibn->Name, addr);
                if (strncmp(dll, "api-ms-win-core-com-",
                            sizeof("api-ms-win-core-com-") - 1) == 0 &&
                    strcmp(ibn->Name, "CoCreateInstance") == 0) {
                    UINT64 combase = lookup_module("combase.dll");
                    if (combase)
                        addr = LdrGetProcAddress(combase, ibn->Name);
                }
                if (!addr)
                    KeLog("[ldr]  DELAY STUB %s!%s\n", dll, ibn->Name);
            }
            if (!addr) {
                addr = LdrpImportStub();
                stubbed++;
            } else {
                linked++;
            }
            iat[i] = addr;
        }

        KeLog("[ldr]  delay imports from %s: %u linked, %u stubbed\n",
              dll, linked, stubbed);
    }
    return STATUS_SUCCESS;
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

static void remember_module(const char *name, UINT64 base)
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
        return;
    }
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

    /* Shell32 consumes the Windows.Storage implementation through API-set
     * contracts.  Keep the contract name out of the module cache and resolve
     * both its internal and public export surfaces to the matching inbox host
     * DLL supplied with Explorer. */
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

    /* GDI32's private kernel-facing contract: the NtGdi* stubs themselves.
     * They live in win32u.dll, which is supplied, and stubbing them to 0 is
     * indistinguishable from "the call failed" -- GetDeviceCaps answering 0 for
     * LOGPIXELSY is enough to make a well-behaved program give up at startup. */
    if (LdrpNameContains(name, "gdi-internal-uap") ||
        LdrpNameContains(name, "gdi-internal-desktop"))
        return LdrpLoadModule("win32u.dll");

    /* The rest of the GDI surface ships under a dozen contract names
     * (gdi-dc, gdi-draw, gdi-font, gdi-clipping, gdi-path, rtcore-gdi-devcaps,
     * rtcore-gdi-object, rtcore-gdi-rgn, ...). They all live in the genuine
     * gdi32. Leaving them unresolved is not neutral: a stubbed GetDeviceCaps
     * answers 0 for LOGPIXELSY, and a program that sizes a font from that gives
     * up before it ever creates a window. */
    if (LdrpNameContains(name, "gdi-"))
        return LdrpLoadModule("gdi32.dll");

    /* Most of the core contracts implemented so far live in kernel32. */
    if (strncmp(name, "api-ms-win-core-", 16) == 0 ||
        strncmp(name, "ext-ms-win-core-", 16) == 0)
        return LdrpLoadModule("kernel32.dll");
    /* VERSION's implemented subset currently lives in kernel32. OLE32 is no
     * longer redirected: Explorer now ships with the matching real DLL and
     * needs its COM apartment/windowing initialization. */
    if (ci_strcmp(name, "version.dll") == 0)
        return LdrpLoadModule("kernel32.dll");

    UINT64 existing = lookup_module(name);
    if (existing)
        return existing;

    void *buf;
    SIZE_T size;
    if (!NT_SUCCESS(FatLoadFile(name, &buf, &size, NULL))) {
        KeLog("[ldr]  dependency '%s' not found on disk\n", name);
        return 0;
    }

    UINT64 base;
    NTSTATUS status = LdrpMapImage(buf, size, &base);
    ExFreePool(buf); /* mapped into user pages; the file buffer is done */
    if (!NT_SUCCESS(status))
        return 0;

    remember_module(name, base); /* cache before resolving to break cycles */

    /* Genuine-DLL compatibility patches, applied while the image is still
     * writable (before LdrpProtectImage). Each is a measured stand-in for a
     * win32k service the kernel side does not host yet. */
    if (ci_strcmp(name, "user32.dll") == 0 ||
        ci_strcmp(name, "USER32.dll") == 0) {
        /* RegisterClassExWOWW gates its class-name pipeline on
         * [g_ClassFlagsA(0xBC920)] == [g_ClassFlagsB(0xBD130)]. User32's init
         * sets the first to 0x7FFF while the second keeps its .data initial
         * value; only equal flags skip the 15-slot client class-list walk,
         * whose entries are all zero in this system (no kernel-side system
         * class pre-registration feeding them). Publishing 0x7FFF in both
         * makes every string-class registration take the direct capture ->
         * win32u path, as it does after a real win32k boot. */
        UINT32 *flags = (UINT32 *)(base + 0xBD130);
        *flags = 0x7FFF;
        KeLog("[ldr]  user32 class-flags synced to 0x7fff\n");
    }

    if (ci_strcmp(name, "gdi32.dll") == 0 ||
        ci_strcmp(name, "GDI32.dll") == 0) {
        /* user32!RegisterClassExWOWW validates a brush-handle background by
         * calling GdiValidateHandle and fails the whole registration when it
         * reports an invalid handle. The real GDI handle table lives in the
         * kernel part we do not have; every caller here passes a brush it
         * just created, so answer TRUE. */
        UINT64 proc = LdrGetProcAddress(base, "GdiValidateHandle");
        if (proc) {
            UINT8 stub[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
            memcpy((void *)proc, stub, sizeof(stub));
            KeLog("[ldr]  gdi32!GdiValidateHandle -> TRUE stub\n");
        }
        /* GDI batches drawing calls in the TEB and only flushes them at
         * GdiBatchLimit or an explicit GdiFlush. A batched call never reaches
         * win32k, so nothing would appear on screen until something happened to
         * flush; a limit of 1 means "never batch", which is also what a
         * debugger-friendly Windows setup uses. */
        UINT64 limit = LdrGetProcAddress(base, "GdiBatchLimit");
        if (limit && MmProbeForWrite(limit, sizeof(UINT32))) {
            *(UINT32 *)limit = 1;
            KeLog("[ldr]  gdi32!GdiBatchLimit = 1 (batching off)\n");
        }
    }

    if (!NT_SUCCESS(LdrResolveImports(base)))
        return 0;
    if (!NT_SUCCESS(LdrResolveDelayImports(base)))
        return 0;
    LdrpInitializeGuardPointers(base);
    LdrpProtectImage(base);

    KeLog("[ldr]  loaded module %s at %p\n", name, (void *)base);
    return base;
}
