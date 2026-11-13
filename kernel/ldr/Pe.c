/*
 * ldr/Pe.c - the PE/COFF image loader with import/export linking.
 *
 * Loading an executable is three steps:
 *   1. map the image at its preferred base and lay out its sections,
 *   2. resolve its imports - load each dependency DLL (e.g. ntdll) on demand
 *      and patch this image's Import Address Table with the resolved routine
 *      addresses,
 *   3. tighten page permissions per section.
 *
 * Executables and their dependency DLLs (ntdll) are read from the mounted
 * filesystem by name; a small cache tracks what is already loaded.
 */
#include <svindows/Ldr.h>
#include <svindows/Mm.h>
#include <svindows/Ke.h>
#include <svindows/Rtl.h>
#include <svindows/Io.h>
#include <svindows/Ex.h>
#include <nt/Pe.h>
#include <nt/Peb.h>

/* Cache of already-loaded modules (name -> load base). Satisfies repeat imports
 * and breaks import cycles. */
typedef struct _LDR_LOADED {
    char    Name[32];
    UINT64  Base;
    UINT64  Dependencies; /* bit i: this module imports g_loaded[i] */
    BOOLEAN Valid;
    BOOLEAN Linked; /* already threaded onto the process PEB->Ldr list */
} LDR_LOADED;

#define MAX_LOADED_MODULES 64
#define LDR_MAX_FORWARDER_DEPTH 16

/* One module's static TLS directory (see ldr/pe/Tls.inc). */
typedef struct _LDR_TLS_MODULE {
    UINT64  ModuleBase;         /* load base, passed to callbacks as hModule */
    UINT64  TemplateStart;      /* VA of the initialized template           */
    UINT64  TemplateEnd;        /* VA one past the template                 */
    UINT64  AddressOfIndex;     /* VA of the module's `_tls_index` DWORD    */
    UINT64  AddressOfCallBacks; /* VA of the null-terminated callback array */
    UINT32  SizeOfZeroFill;     /* extra zero bytes past the template       */
    UINT32  Index;              /* the TLS index assigned to this module    */
    BOOLEAN Valid;
} LDR_TLS_MODULE;

#define MAX_TLS_MODULES 64

/* Fixed user region the loader bump-allocates per-thread TLS vectors and data
 * blocks from (above the LDR_INIT_BLOCK page at 0x90000). */
#define LDR_TLS_VA_BASE 0x00000000000A0000ULL

struct _LDR_PROCESS_CONTEXT {
    LDR_LOADED Loaded[MAX_LOADED_MODULES];
    char ApplicationDirectory[260];
    UINT64 DynamicImageNext;
    UINT64 StubArena;
    UINT32 Depth;
    NTSTATUS LoadStatus;
    PPEB_LDR_DATA PebLdr;
    UINT8 *LdrCursor;
    UINT8 *LdrEnd;
    LDR_TLS_MODULE TlsModules[MAX_TLS_MODULES];
    UINT32 TlsIndexNext;
    UINT32 TlsPad;
    UINT64 TlsRegionNext;
};

static LDR_PROCESS_CONTEXT g_early_context = {
    .DynamicImageNext = 0x0000000400000000ULL,
    .TlsRegionNext = 0x00000000000A0000ULL,
};

static PLDR_PROCESS_CONTEXT LdrpCurrentContext(void)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (thread) {
        if (thread->LoaderContextOverride)
            return thread->LoaderContextOverride;
        if (thread->Process && thread->Process->LoaderContext)
            return thread->Process->LoaderContext;
    }
    return &g_early_context;
}

PLDR_PROCESS_CONTEXT LdrCreateProcessContext(void)
{
    PLDR_PROCESS_CONTEXT context = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*context), 'CtxL');
    if (!context)
        return NULL;
    memset(context, 0, sizeof(*context));
    context->DynamicImageNext = 0x0000000400000000ULL;
    context->TlsRegionNext = LDR_TLS_VA_BASE;
    KeTrace("[ldr]  created process context %p\n", context);
    return context;
}

void LdrDestroyProcessContext(PLDR_PROCESS_CONTEXT context)
{
    if (context && context != &g_early_context) {
        KeTrace("[ldr]  destroying process context %p\n", context);
        ExFreePool(context);
    }
}

PLDR_PROCESS_CONTEXT LdrSetThreadProcessContext(
    PLDR_PROCESS_CONTEXT context)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (!thread)
        return NULL;
    PLDR_PROCESS_CONTEXT previous = thread->LoaderContextOverride;
    thread->LoaderContextOverride = context;
    return previous;
}

#define g_loaded            (LdrpCurrentContext()->Loaded)
#define g_dynamic_image_next (LdrpCurrentContext()->DynamicImageNext)
#define g_stub_arena        (LdrpCurrentContext()->StubArena)
#define g_ldr_depth         (LdrpCurrentContext()->Depth)
#define g_peb_ldr           (LdrpCurrentContext()->PebLdr)
#define g_ldr_cursor        (LdrpCurrentContext()->LdrCursor)
#define g_ldr_end           (LdrpCurrentContext()->LdrEnd)

static int module_index_by_base(UINT64 base);
static void remember_dependency(UINT64 importer, UINT64 dependency);

/* Case-insensitive ASCII compare (DLL names are matched loosely). */
static int ci_strcmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || ca == 0)
            return (int)(UCHAR)ca - (int)(UCHAR)cb;
    }
}

static BOOLEAN ci_starts_with(const char *text, const char *prefix)
{
    while (*prefix) {
        char a = *text++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (!a || a != b) return FALSE;
    }
    return TRUE;
}

static PIMAGE_NT_HEADERS64 nt_headers(UINT64 base)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return NULL;
    return nt;
}

static PIMAGE_SECTION_HEADER first_section(PIMAGE_NT_HEADERS64 nt)
{
    return (PIMAGE_SECTION_HEADER)((UINT8 *)&nt->OptionalHeader +
                                   nt->FileHeader.SizeOfOptionalHeader);
}


#include "pe/ImageMapping.inc"

/* Defined in pe/Tls.inc, called while caching modules in pe/Imports.inc. */
static void LdrpRegisterTls(UINT64 base);

#include "pe/Imports.inc"
#include "pe/Tls.inc"
#include "pe/ProcessModules.inc"
