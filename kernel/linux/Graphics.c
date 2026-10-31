/* Linux fbdev ABI backed by the boot framebuffer. Mode changes are absent. */
#include "Internal.h"
#include <svindows/Gfx.h>
#include <svindows/Rtl.h>

#define LINUX_FBIOGET_VSCREENINFO 0x4600u
#define LINUX_FBIOPUT_VSCREENINFO 0x4601u
#define LINUX_FBIOGET_FSCREENINFO 0x4602u
#define LINUX_FBIOGETCMAP 0x4604u
#define LINUX_FBIOPUTCMAP 0x4605u
#define LINUX_FBIOBLANK 0x4611u

typedef struct _LINUX_FB_FIXED {
    char Id[16];
    UINT64 MemoryStart;
    UINT32 MemoryLength;
    UINT32 Type;
    UINT32 TypeAux;
    UINT32 Visual;
    UINT16 XPanStep, YPanStep, YWrapStep;
    UINT32 LineLength;
    UINT64 MmioStart;
    UINT32 MmioLength;
    UINT32 Accel;
    UINT16 Capabilities;
    UINT16 Reserved[2];
} LINUX_FB_FIXED;

typedef struct _LINUX_FB_VARIABLE {
    UINT32 Fields[40];
} LINUX_FB_VARIABLE;

typedef struct _LINUX_FB_CMAP {
    UINT32 Start, Length;
    UINT64 Red, Green, Blue, Transparency;
} LINUX_FB_CMAP;

static UINT16 g_linux_fb_palette[3][256];

_Static_assert(sizeof(LINUX_FB_FIXED) == 80, "x86-64 fb_fix_screeninfo");
_Static_assert(sizeof(LINUX_FB_VARIABLE) == 160, "fb_var_screeninfo");
_Static_assert(sizeof(LINUX_FB_CMAP) == 40, "x86-64 fb_cmap");

static UINT64 linux_framebuffer_cmap(UINT64 argument, BOOLEAN put)
{
    if (!MmProbeForRead(argument, sizeof(LINUX_FB_CMAP)))
        return LinuxError(LINUX_EFAULT);
    LINUX_FB_CMAP cmap = *(const LINUX_FB_CMAP *)(ULONG_PTR)argument;
    if (cmap.Start > 256 || cmap.Length > 256 - cmap.Start)
        return LinuxError(LINUX_EINVAL);
    UINT64 arrays[3] = {cmap.Red, cmap.Green, cmap.Blue};
    for (UINT32 component = 0; component < 3; component++) {
        if (cmap.Length && !(put ? MmProbeForRead(arrays[component],
                                      cmap.Length * sizeof(UINT16)) :
                                MmProbeForWrite(arrays[component],
                                      cmap.Length * sizeof(UINT16))))
            return LinuxError(LINUX_EFAULT);
    }
    UINT64 irq = KiIrqSave();
    for (UINT32 component = 0; component < 3; component++) {
        UINT16 *colors = (UINT16 *)(ULONG_PTR)arrays[component];
        for (UINT32 i = 0; i < cmap.Length; i++) {
            if (put) g_linux_fb_palette[component][cmap.Start + i] = colors[i];
            else colors[i] = g_linux_fb_palette[component][cmap.Start + i];
        }
    }
    KiIrqRestore(irq);
    return 0;
}

UINT64 LinuxFramebufferSize(void)
{
    return GfxAvailable() ?
           (UINT64)GfxFramebuffer.Pitch * GfxFramebuffer.Height : 0;
}

UINT64 LinuxFramebufferPhysicalAddress(void)
{
    return GfxAvailable() ? GfxFramebuffer.PhysAddr : 0;
}

BOOLEAN LinuxFramebufferPath(const char *path)
{
    static const char expected[] = "C:\\dev\\fb0";
    for (UINT32 i = 0; i < sizeof(expected); i++) {
        char a = path[i], b = expected[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return FALSE;
        if (!a) return TRUE;
    }
    return FALSE;
}

UINT64 LinuxFramebufferOpen(PKPROCESS process, UINT64 slot, UINT64 flags)
{
    if (!GfxAvailable()) return LinuxError(LINUX_ENODEV);
    if (flags & (0x40u | 0x80u | 0x200u))
        return LinuxError(LINUX_EINVAL);
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[slot];
    memset(entry, 0, sizeof(*entry));
    entry->Used = TRUE;
    entry->Mode = (UINT8)(flags & 3u);
    entry->CloseOnExec = !!(flags & 0x80000u);
    entry->Kind = LINUX_FD_FRAMEBUFFER;
    return slot;
}

UINT64 LinuxFramebufferReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd,
                                 UINT64 buffer, UINT64 length)
{
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    if (write ? entry->Mode == 0 : entry->Mode == 1)
        return LinuxError(LINUX_EBADF);
    if (length > LINUX_IO_MAX) return LinuxError(LINUX_EINVAL);
    UINT64 size = LinuxFramebufferSize();
    if (entry->Offset >= size || !length) return 0;
    if (length > size - entry->Offset) length = size - entry->Offset;
    if (!(write ? MmProbeForRead(buffer, length) :
                  MmProbeForWrite(buffer, length)))
        return LinuxError(LINUX_EFAULT);
    UINT8 *pixels = (UINT8 *)GfxFramebuffer.Base + entry->Offset;
    if (write) memcpy(pixels, (const void *)(ULONG_PTR)buffer, (SIZE_T)length);
    else memcpy((void *)(ULONG_PTR)buffer, pixels, (SIZE_T)length);
    entry->Offset += length;
    return length;
}

UINT64 LinuxFramebufferSeek(PKPROCESS process, UINT64 fd, INT64 offset,
                            UINT64 whence)
{
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    INT64 start = whence == 0 ? 0 :
                  whence == 1 ? (INT64)entry->Offset :
                  whence == 2 ? (INT64)LinuxFramebufferSize() : -1;
    if (start < 0) return LinuxError(LINUX_EINVAL);
    UINT64 backward = offset < 0 ? (UINT64)(-(offset + 1)) + 1 : 0;
    if ((offset < 0 && backward > (UINT64)start) ||
        (offset > 0 && (UINT64)offset > LinuxFramebufferSize() - (UINT64)start))
        return LinuxError(LINUX_EINVAL);
    UINT64 target = offset < 0 ? (UINT64)start - backward :
                    (UINT64)start + (UINT64)offset;
    if (target > LinuxFramebufferSize()) return LinuxError(LINUX_EINVAL);
    entry->Offset = target;
    return target;
}

UINT64 LinuxFramebufferIoctl(PKPROCESS process, UINT64 fd, UINT64 command,
                             UINT64 argument)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used)
        return LinuxError(LINUX_EBADF);
    if (process->LinuxContext->Files[fd].Kind != LINUX_FD_FRAMEBUFFER)
        return LinuxError(LINUX_ENOTTY);
    if (command == LINUX_FBIOGET_FSCREENINFO) {
        if (!MmProbeForWrite(argument, sizeof(LINUX_FB_FIXED)))
            return LinuxError(LINUX_EFAULT);
        LINUX_FB_FIXED info;
        memset(&info, 0, sizeof(info));
        memcpy(info.Id, "svindows-fb", sizeof("svindows-fb"));
        info.MemoryStart = GfxFramebuffer.PhysAddr;
        info.MemoryLength = (UINT32)LinuxFramebufferSize();
        info.Visual = 2; /* FB_VISUAL_TRUECOLOR */
        info.LineLength = GfxFramebuffer.Pitch;
        memcpy((void *)(ULONG_PTR)argument, &info, sizeof(info));
        return 0;
    }
    if (command == LINUX_FBIOGET_VSCREENINFO) {
        if (!MmProbeForWrite(argument, sizeof(LINUX_FB_VARIABLE)))
            return LinuxError(LINUX_EFAULT);
        LINUX_FB_VARIABLE info;
        memset(&info, 0, sizeof(info));
        info.Fields[0] = info.Fields[2] = GfxFramebuffer.Width;
        info.Fields[1] = info.Fields[3] = GfxFramebuffer.Height;
        info.Fields[6] = GfxFramebuffer.Bpp;
        info.Fields[8] = GfxFramebuffer.RedShift;
        info.Fields[9] = 8;
        info.Fields[11] = GfxFramebuffer.GreenShift;
        info.Fields[12] = 8;
        info.Fields[14] = GfxFramebuffer.BlueShift;
        info.Fields[15] = 8;
        info.Fields[22] = info.Fields[23] = 0xffffffffu; /* mm unknown */
        memcpy((void *)(ULONG_PTR)argument, &info, sizeof(info));
        return 0;
    }
    if (command == LINUX_FBIOPUT_VSCREENINFO) {
        if (!MmProbeForRead(argument, sizeof(LINUX_FB_VARIABLE)) ||
            !MmProbeForWrite(argument, sizeof(LINUX_FB_VARIABLE)))
            return LinuxError(LINUX_EFAULT);
        const LINUX_FB_VARIABLE *requested =
            (const LINUX_FB_VARIABLE *)(ULONG_PTR)argument;
        if (requested->Fields[0] != GfxFramebuffer.Width ||
            requested->Fields[1] != GfxFramebuffer.Height ||
            requested->Fields[2] != GfxFramebuffer.Width ||
            requested->Fields[3] != GfxFramebuffer.Height ||
            requested->Fields[4] || requested->Fields[5] ||
            requested->Fields[6] != GfxFramebuffer.Bpp)
            return LinuxError(LINUX_EINVAL);
        /* The boot framebuffer has one fixed mode. The requested mode is
         * already active, including FB_ACTIVATE_TEST requests. */
        return 0;
    }
    if (command == LINUX_FBIOPUTCMAP || command == LINUX_FBIOGETCMAP)
        return linux_framebuffer_cmap(argument, command == LINUX_FBIOPUTCMAP);
    if (command == LINUX_FBIOBLANK)
        return argument == 0 ? 0 : LinuxError(LINUX_EOPNOTSUPP);
    return LinuxError(LINUX_ENOTTY);
}
