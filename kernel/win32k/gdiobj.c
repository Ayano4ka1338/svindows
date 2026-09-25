/*
 * win32k/gdiobj.c - GDI objects, surfaces, and the drawing services.
 *
 * The genuine gdi32 is a thin client over a kernel that owns every GDI object.
 * A handle is (type << 16) | index; the index selects a 24-byte GDICELL64 in
 * the table at PEB->GdiSharedHandleTable, and gdi32 checks the cell's type,
 * uniqueness and owning process before it will touch the object. The layout and
 * the checks below are not guessed: they were read out of the supplied
 * gdi32/gdi32full binaries.
 *
 *   gdi32!GdiGetEntry(handle, out)
 *       r9 = pGdiSharedHandleTable
 *       index = handle & 0xffff; cell = r9 + index*24; copy 24 bytes
 *       ... and for a handle whose upper bits do not match the cell's byte at
 *       +0x0D it tail-jumps to the NtGdiGetEntry syscall instead, which is the
 *       path every real handle takes. So the kernel is asked for the cell.
 *
 *   gdi32full!GetDCBrushColor(hdc)
 *       index >= *gMaxGdiHandleCount        -> fail
 *       GdiGetEntry(hdc, &cell)             -> negative NTSTATUS -> fail
 *       cell+0x0E (nType) != 1              -> fail        (1 == DC)
 *       cell+0x0C (nUpper) != handle >> 16  -> fail
 *       (cell+0x08 (ProcessId) & ~1) != pid -> fail
 *       cell+0x10 (pUserInfo) == NULL       -> fail
 *       return *(DWORD *)(pUserInfo + 0x34)                (DC_ATTR.ulBrushClr)
 *
 * That last offset pins DC_ATTR to the layout the ReactOS headers describe, so
 * the colour fields below follow it. It matters because SetTextColor and
 * SetBkColor are pure client-side writes into DC_ATTR -- they never enter the
 * kernel -- so the drawing services have to read the current colours out of the
 * block rather than keeping their own copy.
 */
#include <svindows/win32k.h>
#include <svindows/ps.h>
#include <svindows/mm.h>
#include <svindows/gfx.h>
#include <svindows/ex.h>
#include <svindows/ke.h>
#include <svindows/rtl.h>
#include <nt/ntstatus.h>

extern const UINT8 Font9x20[95][20][5];


#include "gdi/objects.inc"
#include "gdi/drawing.inc"
#include "gdi/device.inc"
