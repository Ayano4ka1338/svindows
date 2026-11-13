/* Host-only regression checks; never copied onto the OS disk. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <svindows/Mm.h>
#include <svindows/Gfx.h>
#include <svindows/Multiboot2.h>
#include <svindows/Io.h>
#include <svindows/Rtl.h>
#include <svindows/Hal.h>
#include <svindows/Ke.h>
#include <svindows/Ps.h>
#include <svindows/Win32k.h>
#include <nt/Pe.h>
#include "../../kernel/ldr/pe/Validation.inc"

UINT64 MmDirectMapBase;
static UINT64 root[512] __attribute__((aligned(4096)));
static UINT64 pdpt[512] __attribute__((aligned(4096)));
static UINT64 pd[512] __attribute__((aligned(4096)));
static UINT64 pt[512] __attribute__((aligned(4096)));
UINT64 MmGetCurrentAddressSpace(void) { return (UINT64)root; }
static SIZE_T released_pages;
void MmFreePages(UINT64 phys, SIZE_T count)
{
    assert(phys && count == 1);
    released_pages += count;
}
void MmFreePageList(const UINT64 *pages, SIZE_T count)
{ for (SIZE_T i = 0; i < count; ++i) MmFreePages(pages[i], 1); }

static void check_thread_user_pages(void)
{
    static UINT64 foreign_root[512] __attribute__((aligned(4096)));
    static UINT64 foreign_pdpt[512] __attribute__((aligned(4096)));
    static UINT64 foreign_pd[512] __attribute__((aligned(4096)));
    static UINT64 foreign_pt[512] __attribute__((aligned(4096)));
    static UINT8 foreign_pages[8192] __attribute__((aligned(4096)));
    const UINT64 rw = PTE_PRESENT | PTE_USER | PTE_WRITE;
    UINT64 base = 0x30000000;
    foreign_root[0] = (UINT64)foreign_pdpt | rw;
    foreign_pdpt[0] = (UINT64)foreign_pd | rw;
    foreign_pd[(base >> 21) & 511] = (UINT64)foreign_pt | rw;
    foreign_pt[0] = (UINT64)foreign_pages | rw;
    foreign_pt[1] = (UINT64)(foreign_pages + PAGE_SIZE) | rw;
    pt[0] = (UINT64)foreign_pages | rw;
    SIZE_T before = released_pages;
    MmReleaseUserPagesInAddressSpace((UINT64)foreign_root, base, 2);
    assert(released_pages == before + 2);
    assert(foreign_pt[0] == 0 && foreign_pt[1] == 0);
    assert(pt[0] != 0); /* the current process was untouched */
    MmReleaseUserPagesInAddressSpace((UINT64)foreign_root, base, 2);
    MmReleaseUserPagesInAddressSpace((UINT64)foreign_root, MM_USER_MAX, 2);
    assert(released_pages == before + 2);
    puts("PASS thread user-page cleanup across address spaces and duplicate release");
}

static void check_shared_handles(void)
{
    static UINT64 other_root[512] __attribute__((aligned(4096)));
    static UINT64 other_pdpt[512] __attribute__((aligned(4096)));
    static UINT64 other_pd[512] __attribute__((aligned(4096)));
    static UINT64 other_pt[512] __attribute__((aligned(4096)));
    static UINT8 owner_pages[8192] __attribute__((aligned(4096)));
    static UINT8 current_pages[8192] __attribute__((aligned(4096)));
    const UINT64 rw = PTE_PRESENT | PTE_USER | PTE_WRITE;
    root[0] = (UINT64)pdpt | rw;
    pdpt[0] = (UINT64)pd | rw;
    pd[8] = (UINT64)pt | rw;
    other_root[0] = (UINT64)other_pdpt | rw;
    other_pdpt[0] = (UINT64)other_pd | rw;
    other_pd[8] = (UINT64)other_pt | rw;
    UINT64 va = PROCESS_USER_SHARED_TABLE_VA + 341 * 24;
    UINT32 index = (va >> 12) & 511;
    pt[index] = (UINT64)current_pages | rw;
    pt[index + 1] = (UINT64)(current_pages + 4096) | rw;
    other_pt[index] = (UINT64)owner_pages | rw;
    other_pt[index + 1] = (UINT64)(owner_pages + 4096) | rw;
    KPROCESS owner = {0};
    owner.DirectoryTableBase = (UINT64)other_root;
    memset(current_pages, 0xaa, sizeof(current_pages));
    memset(owner_pages, 0xbb, sizeof(owner_pages));
    assert(W32kClearSharedHandle(&owner, FALSE, 341));
    for (UINT32 i = 0; i < sizeof(owner_pages); i++) {
        assert(current_pages[i] == 0xaa);
        assert(owner_pages[i] == ((i >= 4088 && i < 4112) ? 0 : 0xbb));
    }
    memset(owner_pages, 0xbb, sizeof(owner_pages));
    other_pt[index + 1] = 0;
    assert(!W32kClearSharedHandle(&owner, FALSE, 341));
    for (UINT32 i = 0; i < sizeof(owner_pages); i++) assert(owner_pages[i] == 0xbb);
    other_pt[0] = (UINT64)owner_pages | rw;
    assert(W32kClearSharedHandle(&owner, TRUE, 1));
    for (UINT32 i = 0; i < 64; i++) assert(owner_pages[i] == ((i >= 24 && i < 48) ? 0 : 0xbb));
    assert(MmGetPhysicalAddressInAddressSpace(0, va) == MM_INVALID_PHYS);
    assert(MmUnmapPageInAddressSpace((UINT64)other_root, PAGE_ALIGN(va)));
    assert(MmGetPhysicalAddressInAddressSpace((UINT64)other_root, va) == MM_INVALID_PHYS);
    assert(MmGetPhysicalAddress(va) == (UINT64)current_pages + (va & PAGE_MASK));
    assert(!MmUnmapPageInAddressSpace(0, va));
    puts("PASS GUI shared-cell cleanup: foreign page tables, page crossing, missing page rollback");
}

/* Fake disk can simulate a transfer which writes bytes before failing. */
static UINT8 disk[128][ATA_SECTOR_SIZE];
static int fail_read;
static UINT32 irp_completions;
static void check_irp_completion(IO_DEVICE_OBJECT *device, IO_IRP *irp,
                                 void *context)
{
    assert(device && irp->Completed && context == &irp_completions);
    ++irp_completions;
}
static NTSTATUS check_irp_dispatch(IO_DEVICE_OBJECT *device, IO_IRP *irp)
{
    IoCompleteRequest(device, irp, STATUS_SUCCESS, 512);
    IoCompleteRequest(device, irp, STATUS_SUCCESS, 1024);
    return STATUS_SUCCESS;
}
static void check_irp(void)
{
    IO_DRIVER_OBJECT driver = {{check_irp_dispatch, NULL, NULL}};
    IO_DEVICE_OBJECT device = {&driver, NULL};
    IO_IRP irp = {0};
    irp.Completion = check_irp_completion;
    irp.CompletionContext = &irp_completions;
    assert(IoCallDriver(&device, &irp) == STATUS_SUCCESS);
    assert(irp.Completed && irp.Information == 512 && irp_completions == 1);
    irp.Stack.MajorFunction = IRP_MJ_WRITE;
    assert(IoCallDriver(&device, &irp) == STATUS_NOT_SUPPORTED);
    puts("PASS IRP dispatch, one-shot completion and status propagation");
}
BOOLEAN AtaReadSectors(UINT32 lba, UINT8 count, void *buffer)
{
    if (lba >= 128 || count != 1) return FALSE;
    memcpy(buffer, disk[lba], ATA_SECTOR_SIZE);
    if (fail_read) {
        memset(buffer, 0xee, ATA_SECTOR_SIZE);
        return FALSE;
    }
    return TRUE;
}
BOOLEAN AtaWriteSectors(UINT32 lba, UINT8 count, const void *buffer)
{
    if (lba >= 128 || count != 1) return FALSE;
    memcpy(disk[lba], buffer, ATA_SECTOR_SIZE);
    return TRUE;
}
BOOLEAN AtaFlushCache(void) { return TRUE; }

static void check_paging(void)
{
    const UINT64 rw = PTE_PRESENT | PTE_USER | PTE_WRITE;
    root[0] = (UINT64)pdpt | rw;
    pdpt[0] = (UINT64)pd | rw;
    pd[0] = (UINT64)pt | rw;
    pt[1] = 0x123000 | rw;
    assert(MmProbeForRead(0x1000, 4096));
    assert(MmProbeForWrite(0x1000, 4096));
    assert(!MmProbeForRead(0x1000, 4097));
    assert(!MmProbeForRead(~(UINT64)0, 2));
    assert(!MmProbeForRead(MM_USER_MAX, 1));
    assert(MmProbeForRead(0, 0));
    pt[1] &= ~PTE_USER;
    assert(!MmProbeForRead(0x1000, 1));
    pt[1] |= PTE_USER;
    root[0] &= ~PTE_USER;
    assert(!MmProbeForRead(0x1000, 1));
    root[0] |= PTE_USER;
    pdpt[0] &= ~PTE_WRITE;
    assert(MmProbeForRead(0x1000, 1));
    assert(!MmProbeForWrite(0x1000, 1));
    pdpt[0] |= PTE_WRITE | PTE_NX;
    assert(MmGetPageFlags(0x1000) & PTE_NX);
    pd[0] = 0x200000 | rw | PTE_LARGE | PTE_NX;
    assert(MmGetPhysicalAddress(0x1234) == 0x201234);
    puts("PASS paging permissions and NX address mask");
}

static void check_multiboot(void)
{
    struct __attribute__((aligned(8))) {
        MB_INFO_HEADER header;
        UINT32 type, size, entry_size, version;
        MB_MMAP_ENTRY entry;
        MB_TAG end;
    } block = {
        {sizeof(block), 0}, MB_TAG_TYPE_MMAP, 40, 24, 0,
        {0x100000, 0x1000000, MB_MEMORY_AVAILABLE, 0}, {0, 8}
    };
    MM_MEMORY_MAP map;
    assert(MmParseMultibootMemoryMap((UINT64)&block, &map));
    assert(map.count == 1 && map.highest_address == 0x1100000);
    block.entry_size = 0;
    assert(!MmParseMultibootMemoryMap((UINT64)&block, &map));
    block.entry_size = 8;
    assert(!MmParseMultibootMemoryMap((UINT64)&block, &map));
    block.entry_size = 24;
    block.size = 0;
    assert(!MmParseMultibootMemoryMap((UINT64)&block, &map));
    block.size = 400;
    assert(!MmParseMultibootMemoryMap((UINT64)&block, &map));
    block.size = 40;
    block.header.total_size -= 8;
    assert(!MmParseMultibootMemoryMap((UINT64)&block, &map));
    block.header.total_size += 8;
    block.entry.addr = ~(UINT64)0 - 1;
    assert(!MmParseMultibootMemoryMap((UINT64)&block, &map));
    puts("PASS malformed Multiboot tags");
}

static void check_graphics(void)
{
    UINT32 pixels[18] = {0};
    pixels[0] = pixels[17] = 0xdeadbeef;
    GfxFramebuffer.Present = TRUE;
    GfxFramebuffer.Base = pixels + 1;
    GfxFramebuffer.Width = GfxFramebuffer.Height = 4;
    GfxFramebuffer.Pitch = 16;
    GfxFillRect(3, 3, ~(UINT32)0, ~(UINT32)0, 123);
    assert(pixels[16] == 123);
    GfxDrawChar(3, 3, 'A', 0xffffff, 0);
    GfxDrawChar(~(UINT32)0, 0, 'A', 0, 0);
    GfxPutPixel(4, 4, 99);
    assert(pixels[0] == 0xdeadbeef && pixels[17] == 0xdeadbeef);
    GfxFramebuffer.Base = NULL;
    GfxPutPixel(0, 0, 99);
    puts("PASS framebuffer clipping");
}

static void check_cache(void)
{
    UINT8 buffer[ATA_SECTOR_SIZE];
    for (UINT32 i = 0; i < 128; i++) memset(disk[i], i, sizeof(disk[i]));
    for (UINT32 i = 0; i < 64; i++) assert(IoBlockReadSectors(i, 1, buffer));
    fail_read = 1;
    assert(!IoBlockReadSectors(64, 1, buffer));
    fail_read = 0;
    assert(IoBlockReadSectors(0, 1, buffer));
    assert(buffer[0] == 0); /* Old LBA must not return failed-transfer bytes. */
    assert(IoBlockBeginTransaction());
    memset(buffer, 0xab, sizeof(buffer));
    assert(IoBlockWriteSectors(2, 1, buffer));
    IoBlockSetWriteFault(0);
    assert(!IoBlockCommitTransaction());
    assert(IoBlockRollbackTransaction());
    assert(IoBlockReadSectors(2, 1, buffer) && buffer[0] == 2);
    puts("PASS cache failed read and transaction rollback");
}

static void check_caret(void)
{
    UINT32 text[GFX_FONT_W * GFX_FONT_H] = {0};
    UINT32 caret[GFX_FONT_W * GFX_FONT_H] = {0};
    GfxFramebuffer.Width = GFX_FONT_W;
    GfxFramebuffer.Height = GFX_FONT_H;
    GfxFramebuffer.Pitch = GFX_FONT_W * sizeof(UINT32);
    GfxFramebuffer.RedShift = 16;
    GfxFramebuffer.GreenShift = 8;
    GfxFramebuffer.BlueShift = 0;
    GfxFramebuffer.Base = text;
    HalConsoleRenderCell(0, 0, '|', 7);
    GfxFramebuffer.Base = caret;
    HalConsoleSetUserCursor(0, 0, TRUE, 20, 7);
    assert(memcmp(text, caret, sizeof(text)) == 0);
    unsigned min_x = GFX_FONT_W, max_x = 0, min_y = GFX_FONT_H, max_y = 0;
    for (unsigned y = 0; y < GFX_FONT_H; y++)
        for (unsigned x = 0; x < GFX_FONT_W; x++)
            if (caret[y * GFX_FONT_W + x]) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
    assert(max_y - min_y > max_x - min_x);
    caret[0] = 0x123456;
    HalConsoleSetUserCursor(0, 0, TRUE, 20, 7);
    assert(caret[0] == 0x123456);
    GfxFramebuffer.Base = NULL;
    puts("PASS vertical caret matches font color/coverage and preserves underlying pixels");
}

int main(void)
{
    check_thread_user_pages();
    check_irp();
    check_paging(); check_shared_handles(); check_multiboot(); check_graphics(); check_cache(); check_caret();
    char text[32];
    assert(RtlFormat(text, sizeof(text), "tail%") == 5);
    assert(strcmp(text, "tail%") == 0);
    assert(RtlFormat(text, sizeof(text), "%lld", (-9223372036854775807LL - 1)) == 20);
    assert(strcmp(text, "-9223372036854775808") == 0);
    puts("PASS formatter incomplete conversion and signed minimum");
    UINT8 image[512] __attribute__((aligned(8))) = {0};
    IMAGE_DOS_HEADER *dos = (void *)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 64;
    IMAGE_NT_HEADERS64 *nt = (void *)(image + 64);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = 4096;
    nt->OptionalHeader.SizeOfHeaders = sizeof(image);
    nt->OptionalHeader.NumberOfRvaAndSizes = 16;
    assert(LdrpValidateImageFile(image, sizeof(image)));
    dos->e_lfanew = ~(UINT32)0;
    assert(!LdrpValidateImageFile(image, sizeof(image)));
    dos->e_lfanew = 64;
    nt->OptionalHeader.SizeOfHeaders = 4096;
    assert(!LdrpValidateImageFile(image, sizeof(image)));
    nt->OptionalHeader.SizeOfHeaders = sizeof(image);
    nt->FileHeader.NumberOfSections = 0xffff;
    assert(!LdrpValidateImageFile(image, sizeof(image)));
    nt->FileHeader.NumberOfSections = 0;
    nt->OptionalHeader.DataDirectory[0].VirtualAddress = 4090;
    nt->OptionalHeader.DataDirectory[0].Size = 32;
    assert(!LdrpValidateImageFile(image, sizeof(image)));
    puts("PASS PE header and directory bounds");
    return 0;
}
