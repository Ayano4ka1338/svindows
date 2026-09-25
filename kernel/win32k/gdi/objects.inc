/* GDI shared handles, kernel objects, surfaces and device contexts.
 * Included by kernel/win32k/gdiobj.c. */
/* ------------------------------------------------------------------ */
/* The shared handle table                                             */
/* ------------------------------------------------------------------ */

typedef struct _GDICELL64 {
    UINT64 pKernelInfo;
    UINT32 ProcessId;
    UINT16 nUpper;
    UINT16 nType;
    UINT64 pUserInfo;
} GDICELL64;

_Static_assert(sizeof(GDICELL64) == 24, "GDICELL64 must be 24 bytes");

/* nType values (the GDI handle type nibble, LO_*_TYPE >> 16). */
#define GDI_TYPE_DC     0x01
#define GDI_TYPE_REGION 0x04
#define GDI_TYPE_BITMAP 0x05
#define GDI_TYPE_FONT   0x0A
#define GDI_TYPE_BRUSH  0x10
#define GDI_TYPE_PEN    0x30

/* DC_ATTR offsets (see the header comment). */
#define DCATTR_PVLDC       0x00
#define DCATTR_ULDIRTY     0x08
#define DCATTR_HBRUSH      0x10
#define DCATTR_HPEN        0x18
#define DCATTR_CRBACKGND   0x20
#define DCATTR_ULBACKGND   0x24
#define DCATTR_CRFOREGND   0x28
#define DCATTR_ULFOREGND   0x2C
#define DCATTR_CRBRUSH     0x30
#define DCATTR_ULBRUSH     0x34
#define DCATTR_CRPEN       0x38
#define DCATTR_ULPEN       0x3C
#define DCATTR_SIZE        0x200

#define GDI_MAX_OBJECTS 512   /* handle indices 1..511 */
#define GDI_TABLE_ENTRIES 0x10000

/* ------------------------------------------------------------------ */
/* Kernel-side objects                                                 */
/* ------------------------------------------------------------------ */

/* A drawing target: either the linear framebuffer or a pool-backed bitmap.
 * Both are 32 bits per pixel in the framebuffer's channel order, so a blit
 * between them is a straight copy. */
typedef struct _W32K_SURFACE {
    UINT32 *Bits;
    UINT32  Width;
    UINT32  Height;
    UINT32  Stride;   /* pixels per scanline */
    BOOLEAN IsScreen;
} W32K_SURFACE;

typedef struct _W32K_DC {
    W32K_SURFACE *Surface;
    W32K_SURFACE  ScreenSurface; /* used when the DC draws to the screen */
    UINT64 Hwnd;
    INT32  OrgX, OrgY;           /* where (0,0) of the DC lands on the surface */
    INT32  ClipX, ClipY, ClipW, ClipH; /* in surface coordinates */
    INT32  CurX, CurY;           /* current position for MoveTo/LineTo */
    UINT64 SelectedBrush;
    UINT64 SelectedPen;
    UINT64 SelectedFont;
    UINT64 SelectedBitmap;
} W32K_DC;

typedef struct _W32K_BRUSH {
    UINT32 Color;   /* COLORREF */
    BOOLEAN Hollow;
} W32K_BRUSH;

typedef struct _W32K_BITMAP {
    W32K_SURFACE Surface;
    void *Storage;
} W32K_BITMAP;

typedef struct _W32K_OBJECT {
    UINT16 Type;      /* 0 when the slot is free */
    UINT16 Upper;     /* the handle's high 16 bits */
    union {
        W32K_DC     Dc;
        W32K_BRUSH  Brush;
        W32K_BITMAP Bitmap;
    } u;
} W32K_OBJECT;

static W32K_OBJECT g_gdi_objects[GDI_MAX_OBJECTS];
static UINT16 g_gdi_next_index = 1;
static UINT64 g_gdi_attr_next = PROCESS_GDI_ATTR_VA;

static GDICELL64 *gdi_table(void)
{
    return (GDICELL64 *)PROCESS_GDI_SHARED_TABLE_VA;
}

static W32K_OBJECT *gdi_object(UINT64 handle, UINT16 type)
{
    UINT16 index = (UINT16)(handle & 0xFFFF);
    if (!index || index >= GDI_MAX_OBJECTS)
        return NULL;
    W32K_OBJECT *object = &g_gdi_objects[index];
    if (!object->Type || (type && object->Type != type))
        return NULL;
    if (object->Upper != (UINT16)(handle >> 16))
        return NULL;
    return object;
}

/* Allocate a handle of `type`, publish its cell, and return the handle. The
 * handle's upper half is the type, which is what gdi32 compares the cell's
 * nUpper against. */
static UINT64 gdi_allocate(UINT16 type, UINT64 user_info, W32K_OBJECT **out)
{
    for (UINT16 tries = 0; tries < GDI_MAX_OBJECTS; tries++) {
        UINT16 index = g_gdi_next_index;
        g_gdi_next_index = (UINT16)(index + 1);
        if (g_gdi_next_index >= GDI_MAX_OBJECTS)
            g_gdi_next_index = 1;
        if (!index || g_gdi_objects[index].Type)
            continue;

        W32K_OBJECT *object = &g_gdi_objects[index];
        memset(object, 0, sizeof(*object));
        object->Type = type;
        object->Upper = type;

        GDICELL64 *cell = &gdi_table()[index];
        cell->pKernelInfo = (UINT64)object;
        cell->ProcessId = PROCESS_CLIENT_ID; /* must be even; see ps.h */
        cell->nUpper = type;
        cell->nType = type;
        cell->pUserInfo = user_info;

        if (out)
            *out = object;
        return ((UINT64)type << 16) | index;
    }
    KeLog("[w32k] GDI handle table full\n");
    return 0;
}

static void gdi_free(UINT64 handle)
{
    UINT16 index = (UINT16)(handle & 0xFFFF);
    if (!index || index >= GDI_MAX_OBJECTS || !g_gdi_objects[index].Type)
        return;
    g_gdi_objects[index].Type = 0;
    memset(&gdi_table()[index], 0, sizeof(GDICELL64));
}

/* Hand out a zeroed DC_ATTR block from the user-writable arena. */
static UINT64 gdi_allocate_attr(void)
{
    if (g_gdi_attr_next + DCATTR_SIZE >
        PROCESS_GDI_ATTR_VA + PROCESS_GDI_ATTR_SIZE)
        return 0;
    UINT64 block = g_gdi_attr_next;
    g_gdi_attr_next += DCATTR_SIZE;
    memset((void *)block, 0, DCATTR_SIZE);
    return block;
}

void W32kInitializeGdi(void)
{
    memset(g_gdi_objects, 0, sizeof(g_gdi_objects));
    g_gdi_next_index = 1;
    g_gdi_attr_next = PROCESS_GDI_ATTR_VA;
    memset(gdi_table(), 0, GDI_TABLE_ENTRIES * sizeof(GDICELL64));
    KeLog("[w32k] GDI object table ready (%u handles, cells at 0x%lx, "
          "attributes at 0x%lx)\n", (unsigned)GDI_MAX_OBJECTS - 1,
          (unsigned long)PROCESS_GDI_SHARED_TABLE_VA,
          (unsigned long)PROCESS_GDI_ATTR_VA);
}

/* ------------------------------------------------------------------ */
/* Surfaces and clipped drawing                                        */
/* ------------------------------------------------------------------ */

/* COLORREF is 0x00BBGGRR; the framebuffer's channel order comes from the
 * Multiboot2 framebuffer tag, so go through GfxColor rather than assuming. */
static UINT32 gdi_pixel(UINT32 colorref)
{
    return GfxColor((UINT8)(colorref & 0xFF), (UINT8)((colorref >> 8) & 0xFF),
                    (UINT8)((colorref >> 16) & 0xFF));
}

static void surface_fill(W32K_SURFACE *surface, INT32 x, INT32 y, INT32 w,
                         INT32 h, UINT32 pixel)
{
    if (!surface || !surface->Bits || w <= 0 || h <= 0)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (INT32)surface->Width || y >= (INT32)surface->Height)
        return;
    if (x + w > (INT32)surface->Width)
        w = (INT32)surface->Width - x;
    if (y + h > (INT32)surface->Height)
        h = (INT32)surface->Height - y;
    if (w <= 0 || h <= 0)
        return;

    for (INT32 row = 0; row < h; row++) {
        UINT32 *line = surface->Bits + (UINT32)(y + row) * surface->Stride + x;
        for (INT32 col = 0; col < w; col++)
            line[col] = pixel;
    }
}

static void surface_pixel(W32K_SURFACE *surface, INT32 x, INT32 y, UINT32 pixel)
{
    if (!surface || !surface->Bits || x < 0 || y < 0 ||
        x >= (INT32)surface->Width || y >= (INT32)surface->Height)
        return;
    surface->Bits[(UINT32)y * surface->Stride + (UINT32)x] = pixel;
}

/* Intersect a DC-relative rectangle with the DC's clip box and translate it
 * into surface coordinates. Returns FALSE when nothing is left. */
static BOOLEAN dc_clip(W32K_DC *dc, INT32 *x, INT32 *y, INT32 *w, INT32 *h)
{
    INT32 left = dc->OrgX + *x, top = dc->OrgY + *y;
    INT32 right = left + *w, bottom = top + *h;
    INT32 clip_right = dc->ClipX + dc->ClipW, clip_bottom = dc->ClipY + dc->ClipH;

    if (left < dc->ClipX) left = dc->ClipX;
    if (top < dc->ClipY) top = dc->ClipY;
    if (right > clip_right) right = clip_right;
    if (bottom > clip_bottom) bottom = clip_bottom;
    if (right <= left || bottom <= top)
        return FALSE;
    *x = left;
    *y = top;
    *w = right - left;
    *h = bottom - top;
    return TRUE;
}

static void dc_fill(W32K_DC *dc, INT32 x, INT32 y, INT32 w, INT32 h,
                    UINT32 colorref)
{
    if (!dc_clip(dc, &x, &y, &w, &h))
        return;
    surface_fill(dc->Surface, x, y, w, h, gdi_pixel(colorref));
}

static UINT32 blend_pixel(UINT32 fg, UINT32 bg, UINT8 alpha)
{
    UINT32 fr = (fg >> GfxFramebuffer.RedShift) & 0xff;
    UINT32 fgreen = (fg >> GfxFramebuffer.GreenShift) & 0xff;
    UINT32 fb = (fg >> GfxFramebuffer.BlueShift) & 0xff;
    UINT32 br = (bg >> GfxFramebuffer.RedShift) & 0xff;
    UINT32 bgreen = (bg >> GfxFramebuffer.GreenShift) & 0xff;
    UINT32 bb = (bg >> GfxFramebuffer.BlueShift) & 0xff;
    return GfxColor((UINT8)((fr * alpha + br * (15 - alpha) + 7) / 15),
                    (UINT8)((fgreen * alpha + bgreen * (15 - alpha) + 7) / 15),
                    (UINT8)((fb * alpha + bb * (15 - alpha) + 7) / 15));
}

/* Draw one 9x20 glyph at a DC-relative position, clipped, with an opaque or
 * transparent background. */
static void dc_glyph(W32K_DC *dc, INT32 x, INT32 y, char c, UINT32 fg,
                     UINT32 bg, BOOLEAN opaque)
{
    UINT8 uc = (UINT8)c;
    const UINT8 (*glyph)[5] =
        (uc >= 32 && uc < 127) ? Font9x20[uc - 32] : Font9x20[0];
    UINT32 fg_pixel = gdi_pixel(fg), bg_pixel = gdi_pixel(bg);

    for (INT32 row = 0; row < GFX_FONT_H; row++) {
        for (INT32 col = 0; col < GFX_FONT_W; col++) {
            UINT8 packed = glyph[row][col / 2];
            UINT8 alpha = (col & 1) ? packed >> 4 : packed & 0x0f;
            if (!alpha && !opaque)
                continue;
            INT32 px = x + col, py = y + row, pw = 1, ph = 1;
            if (!dc_clip(dc, &px, &py, &pw, &ph))
                continue;
            UINT32 under = opaque ? bg_pixel :
                dc->Surface->Bits[(UINT32)py * dc->Surface->Stride +
                                  (UINT32)px];
            surface_pixel(dc->Surface, px, py,
                          alpha ? blend_pixel(fg_pixel, under, alpha) : under);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Device contexts                                                     */
/* ------------------------------------------------------------------ */

static void screen_surface(W32K_SURFACE *surface)
{
    surface->Bits = GfxFramebuffer.Base;
    surface->Width = GfxFramebuffer.Width;
    surface->Height = GfxFramebuffer.Height;
    surface->Stride = GfxFramebuffer.Pitch / 4;
    surface->IsScreen = TRUE;
}

UINT64 W32kCreateWindowDc(UINT64 hwnd, INT32 x, INT32 y, INT32 width,
                          INT32 height)
{
    if (!GfxAvailable())
        return 0;

    UINT64 attr = gdi_allocate_attr();
    if (!attr) {
        KeLog("[w32k] DC_ATTR arena exhausted\n");
        return 0;
    }

    W32K_OBJECT *object = NULL;
    UINT64 hdc = gdi_allocate(GDI_TYPE_DC, attr, &object);
    if (!hdc)
        return 0;

    W32K_DC *dc = &object->u.Dc;
    screen_surface(&dc->ScreenSurface);
    dc->Surface = &dc->ScreenSurface;
    dc->Hwnd = hwnd;

    /* A window with no size yet still gets a DC; it simply clips everything
     * away, which is what a zero-sized client area means. */
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    dc->OrgX = x;
    dc->OrgY = y;
    dc->ClipX = x;
    dc->ClipY = y;
    dc->ClipW = width;
    dc->ClipH = height;

    /* Defaults a fresh DC starts with: black text on white, white brush. */
    *(UINT32 *)(attr + DCATTR_CRFOREGND) = 0x00000000;
    *(UINT32 *)(attr + DCATTR_ULFOREGND) = 0x00000000;
    *(UINT32 *)(attr + DCATTR_CRBACKGND) = 0x00FFFFFF;
    *(UINT32 *)(attr + DCATTR_ULBACKGND) = 0x00FFFFFF;
    *(UINT32 *)(attr + DCATTR_CRBRUSH) = 0x00FFFFFF;
    *(UINT32 *)(attr + DCATTR_ULBRUSH) = 0x00FFFFFF;

    KeLog("[w32k] DC %p for HWND %p at %ld,%ld %ldx%ld (attr 0x%lx)\n",
          (void *)hdc, (void *)hwnd, (long)x, (long)y, (long)width,
          (long)height, (unsigned long)attr);
    return hdc;
}

void W32kReleaseDc(UINT64 hdc)
{
    if (gdi_object(hdc, GDI_TYPE_DC))
        gdi_free(hdc);
}

static W32K_DC *dc_from_handle(UINT64 hdc)
{
    W32K_OBJECT *object = gdi_object(hdc, GDI_TYPE_DC);
    return object ? &object->u.Dc : NULL;
}

/* The DC's user-mode attribute block, or 0. */
static UINT64 dc_attr(UINT64 hdc)
{
    UINT16 index = (UINT16)(hdc & 0xFFFF);
    if (!index || index >= GDI_MAX_OBJECTS)
        return 0;
    return gdi_table()[index].pUserInfo;
}

static UINT32 dc_text_color(UINT64 hdc)
{
    UINT64 attr = dc_attr(hdc);
    return attr ? *(volatile UINT32 *)(attr + DCATTR_CRFOREGND) : 0;
}

static UINT32 dc_bk_color(UINT64 hdc)
{
    UINT64 attr = dc_attr(hdc);
    return attr ? *(volatile UINT32 *)(attr + DCATTR_CRBACKGND) : 0x00FFFFFF;
}

