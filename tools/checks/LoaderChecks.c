/* Actual loader, synthetic PE images and fake page/FAT services. No guest EXE. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/ldr/Pe.c"

UINT64 kernel_stack_guard[2];
static UINT64 mapped[64];
static unsigned frees, allocations, file_reads;
static int fail_allocate = -1, fail_map;
static UINT8 file_image[4096];
static UINT8 child_image[4096];
static BOOLEAN provide_file;

PKTHREAD KeGetCurrentThread(void) { return NULL; }
int KeLog(const char *fmt, ...) { (void)fmt; return 0; }
NORETURN void KeBugCheck(ULONG code, const char *message)
{ (void)code; (void)message; abort(); }
PVOID ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T size, ULONG tag)
{ (void)type; (void)tag; return malloc(size); }
void ExFreePool(PVOID p) { free(p); }
UINT64 MmAllocatePage(void)
{
    if (fail_allocate == 0) return MM_INVALID_PHYS;
    if (fail_allocate > 0) fail_allocate--;
    return (++allocations) * PAGE_SIZE;
}
void MmFreePages(UINT64 pa, SIZE_T count)
{ assert(pa && count == 1); frees++; }
BOOLEAN MmMapPage(UINT64 va, UINT64 pa, UINT64 flags)
{
    (void)pa; (void)flags;
    if (fail_map) return FALSE;
    for (unsigned i = 0; i < 64; i++) {
        if (mapped[i] == va) return TRUE;
        if (!mapped[i]) { mapped[i] = va; return TRUE; }
    }
    return FALSE;
}
UINT64 MmGetPhysicalAddress(UINT64 va)
{
    for (unsigned i = 0; i < 64; i++)
        if (mapped[i] == PAGE_ALIGN(va)) return PAGE_SIZE;
    return MM_INVALID_PHYS;
}
BOOLEAN MmUnmapPage(UINT64 va)
{
    for (unsigned i = 0; i < 64; i++)
        if (mapped[i] == va) { mapped[i] = 0; return TRUE; }
    return FALSE;
}
BOOLEAN MmProbeForWrite(UINT64 va, UINT64 len)
{ (void)va; (void)len; return FALSE; }
NTSTATUS FatLoadFile(const char *name, void **buffer, SIZE_T *size,
                     FAT_FIND_DATA *info)
{
    (void)info;
    file_reads++;
    if (!provide_file || (strcmp(name, "broken.dll") && strcmp(name, "child.dll")))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    *buffer = malloc(sizeof(file_image));
    assert(*buffer);
    memcpy(*buffer, strcmp(name, "child.dll") ? file_image : child_image,
           sizeof(file_image));
    *size = sizeof(file_image);
    return STATUS_SUCCESS;
}

static void init_image(UINT8 *image)
{
    memset(image, 0, 4096);
    IMAGE_DOS_HEADER *dos = (void *)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 64;
    PIMAGE_NT_HEADERS64 nt = (void *)(image + 64);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.NumberOfRvaAndSizes = 16;
    nt->OptionalHeader.SizeOfImage = 4096;
    nt->OptionalHeader.SizeOfHeaders = 4096;
}

static void import_image(UINT8 *image, BOOLEAN delay, const char *dll)
{
    init_image(image);
    PIMAGE_NT_HEADERS64 nt = nt_headers((UINT64)image);
    if (delay) {
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT] =
            (IMAGE_DATA_DIRECTORY){512, 2 * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)};
        IMAGE_DELAYLOAD_DESCRIPTOR *d = (void *)(image + 512);
        d->Attributes = 1;
        d->DllNameRva = 640;
        d->ImportNameTableRva = 768;
        d->ImportAddressTableRva = 800;
    } else {
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] =
            (IMAGE_DATA_DIRECTORY){512, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR)};
        IMAGE_IMPORT_DESCRIPTOR *d = (void *)(image + 512);
        d->Name = 640;
        d->OriginalFirstThunk = 768;
        d->FirstThunk = 800;
    }
    strcpy((char *)image + 640, dll);
    *(UINT64 *)(image + 768) = 900;
    strcpy((char *)image + 902, "Present");
    *(UINT64 *)(image + 800) = delay ? (UINT64)image + 1000 : 0x12345678;
}

static NTSTATUS resolve(UINT8 *image, BOOLEAN delay)
{
    return delay ? LdrValidateDelayImports((UINT64)image)
                 : LdrResolveImports((UINT64)image);
}

int main(void)
{
    LdrInitializeStackGuard();
    UINT8 *dep = aligned_alloc(4096, 4096);
    UINT8 *image = aligned_alloc(4096, 4096);
    UINT8 *target = aligned_alloc(4096, 4096);
    UINT8 *child = aligned_alloc(4096, 4096);
    assert(dep && image && target && child);
    init_image(dep);
    nt_headers((UINT64)dep)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] =
        (IMAGE_DATA_DIRECTORY){512, 128};
    IMAGE_EXPORT_DIRECTORY *ed = (void *)(dep + 512);
    ed->Base = 7;
    ed->NumberOfFunctions = ed->NumberOfNames = 1;
    ed->AddressOfFunctions = 700;
    ed->AddressOfNames = 704;
    ed->AddressOfNameOrdinals = 708;
    *(UINT32 *)(dep + 700) = 1024;
    *(UINT32 *)(dep + 704) = 720;
    strcpy((char *)dep + 720, "Present");
    assert(remember_module("fixture.dll", (UINT64)dep));

    import_image(image, FALSE, "fixture.dll");
    assert(resolve(image, FALSE) == STATUS_SUCCESS);
    assert(*(UINT64 *)(image + 800) == (UINT64)dep + 1024);
    import_image(image, FALSE, "fixture.dll");
    strcpy((char *)image + 902, "Missing");
    assert(resolve(image, FALSE) == STATUS_ENTRYPOINT_NOT_FOUND);
    assert(*(UINT64 *)(image + 800) == 0x12345678);
    *(UINT64 *)(image + 768) = IMAGE_ORDINAL_FLAG64 | 8;
    assert(resolve(image, FALSE) == STATUS_ORDINAL_NOT_FOUND);
    *(UINT64 *)(image + 768) = IMAGE_ORDINAL_FLAG64 | 7;
    assert(resolve(image, FALSE) == STATUS_SUCCESS);
    import_image(image, FALSE, "absent.dll");
    assert(resolve(image, FALSE) == STATUS_DLL_NOT_FOUND);
    assert(*(UINT64 *)(image + 800) == 0x12345678);
    puts("PASS strict static named, ordinal and DLL imports");

    unsigned reads = file_reads;
    import_image(image, TRUE, "absent.dll");
    IMAGE_DELAYLOAD_DESCRIPTOR *delay_descriptor = (void *)(image + 512);
    delay_descriptor->ModuleHandleRva = 816;
    assert(resolve(image, TRUE) == STATUS_SUCCESS);
    assert(*(UINT64 *)(image + 800) == (UINT64)image + 1000);
    assert(!*(UINT64 *)(image + 816) && file_reads == reads);
    *(UINT64 *)(image + 768) = IMAGE_ORDINAL_FLAG64 | 999;
    assert(resolve(image, TRUE) == STATUS_SUCCESS && file_reads == reads);
    delay_descriptor->BoundImportAddressTableRva = 4095;
    assert(resolve(image, TRUE) == STATUS_INVALID_IMAGE_FORMAT);
    delay_descriptor->BoundImportAddressTableRva = 0;
    *(UINT64 *)(image + 800) = 0x12345678;
    assert(resolve(image, TRUE) == STATUS_INVALID_IMAGE_FORMAT);
    puts("PASS lazy delay imports preserve helpers and module slots without loading optional DLLs");

    for (unsigned delay = 0; delay < 2; delay++) {
        import_image(image, delay, "fixture.dll");
        *(UINT64 *)(image + 768) = 4095;
        assert(resolve(image, delay) == STATUS_INVALID_IMAGE_FORMAT);
        import_image(image, delay, "fixture.dll");
        memset(image + 902, 'x', 4096 - 902);
        assert(resolve(image, delay) == STATUS_INVALID_IMAGE_FORMAT);
        import_image(image, delay, "fixture.dll");
        if (delay) ((IMAGE_DELAYLOAD_DESCRIPTOR *)(image + 512))->ImportAddressTableRva = 4096;
        else ((IMAGE_IMPORT_DESCRIPTOR *)(image + 512))->FirstThunk = 4096;
        assert(resolve(image, delay) == STATUS_INVALID_IMAGE_FORMAT);
        import_image(image, delay, "fixture.dll");
        unsigned entry = delay ? IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT : IMAGE_DIRECTORY_ENTRY_IMPORT;
        nt_headers((UINT64)image)->OptionalHeader.DataDirectory[entry].Size /= 2;
        assert(resolve(image, delay) == STATUS_INVALID_IMAGE_FORMAT);
    }
    *(UINT16 *)(dep + 708) = 5;
    assert(!LdrGetProcAddress((UINT64)dep, "Present"));
    *(UINT16 *)(dep + 708) = 0;
    *(UINT32 *)(dep + 700) = 600;
    strcpy((char *)dep + 600, "fixture.Present");
    assert(!LdrGetProcAddress((UINT64)dep, "Present"));
    assert(g_ldr_depth == 0);
    *(UINT32 *)(dep + 700) = 1024;
    puts("PASS malformed import/export bounds and cyclic forwarder rejection");

    ed->AddressOfFunctions = 731;
    ed->AddressOfNames = 737;
    ed->AddressOfNameOrdinals = 743;
    UINT32 function_rva = 1024, name_rva = 720;
    UINT16 export_ordinal = 0;
    memcpy(dep + 731, &function_rva, 4);
    memcpy(dep + 737, &name_rva, 4);
    memcpy(dep + 743, &export_ordinal, 2);
    assert(LdrGetProcAddress((UINT64)dep, "Present") == (UINT64)dep + 1024);
    assert(LdrpProcByOrdinal((UINT64)dep, 7) == (UINT64)dep + 1024);
    puts("PASS unaligned PE export arrays (lld layout)");

    memcpy(child_image, dep, 4096);
    nt_headers((UINT64)child_image)->OptionalHeader.ImageBase = (UINT64)child;
    import_image(file_image, FALSE, "child.dll");
    strcpy((char *)file_image + 902, "Missing");
    nt_headers((UINT64)file_image)->OptionalHeader.ImageBase = (UINT64)target;
    provide_file = TRUE;
    for (unsigned i = 0; i < 3; i++) {
        unsigned freed_before = frees;
        assert(!LdrLoadLibrary("broken.dll"));
        assert(g_early_context.LoadStatus == STATUS_ENTRYPOINT_NOT_FOUND);
        assert(!lookup_module("broken.dll"));
        assert(!lookup_module("child.dll"));
        assert(lookup_module("fixture.dll") == (UINT64)dep);
        assert(MmGetPhysicalAddress((UINT64)target) == MM_INVALID_PHYS);
        assert(MmGetPhysicalAddress((UINT64)child) == MM_INVALID_PHYS);
        assert(frees == freed_before + 2);
    }
    strcpy((char *)file_image + 902, "Present");
    assert(LdrLoadLibrary("broken.dll") == (UINT64)target);
    assert(*(UINT64 *)(target + 800) == (UINT64)child + 1024);
    LdrpReleaseImagePages((UINT64)target, 4096);
    LdrpReleaseImagePages((UINT64)child, 4096);
    puts("PASS failed DLL rollback, repeated failure and repaired retry");

    unsigned freed_before = frees;
    fail_allocate = 1;
    assert(LdrpAllocateImagePages(0x100000, 3 * PAGE_SIZE) == STATUS_NO_MEMORY);
    assert(frees == freed_before + 1);
    assert(MmGetPhysicalAddress(0x100000) == MM_INVALID_PHYS);
    fail_allocate = -1;
    fail_map = 1;
    assert(LdrpAllocateImagePages(0x100000, PAGE_SIZE) == STATUS_CONFLICTING_ADDRESSES);
    assert(frees == freed_before + 2);
    puts("PASS image allocation and mapping failure cleanup");
    fail_map = 0;
    UINT64 tls_start = g_early_context.TlsRegionNext;
    freed_before = frees;
    fail_allocate = 1;
    assert(!LdrpTlsAllocate(&g_early_context, 3 * PAGE_SIZE));
    assert(frees == freed_before + 1 &&
           g_early_context.TlsRegionNext == tls_start &&
           MmGetPhysicalAddress(tls_start) == MM_INVALID_PHYS);
    fail_allocate = -1;
    fail_map = 1;
    assert(!LdrpTlsAllocate(&g_early_context, PAGE_SIZE));
    assert(frees == freed_before + 2 &&
           g_early_context.TlsRegionNext == tls_start);
    fail_map = 0;
    puts("PASS TLS allocation failure rollback");
    UINT64 first = LdrpTlsAllocate(&g_early_context, 2 * PAGE_SIZE);
    assert(first == tls_start);
    UINT64 highwater = g_early_context.TlsRegionNext;
    LdrpTlsReleasePages(first, first + 2 * PAGE_SIZE);
    assert(LdrpTlsAllocate(&g_early_context, 2 * PAGE_SIZE) == first);
    assert(g_early_context.TlsRegionNext == highwater);
    LdrpTlsReleasePages(first, first + 2 * PAGE_SIZE);
    puts("PASS TLS virtual-address reuse after thread release");
    free(child); free(target); free(image); free(dep);
    return 0;
}
