/* GDI shared handles, kernel objects, surfaces and device contexts.
 * Included by kernel/win32k/Gdiobj.c. */
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
    W32K_SURFACE  DeviceSurface; /* screen DC or retained window drawing buffer */
    W32K_SURFACE  PresentedSurface; /* last completed window paint */
    void *WindowStorage;
    BOOLEAN Painting;
    BOOLEAN Dirty;
    INT32 DirtyLeft, DirtyTop, DirtyRight, DirtyBottom;
    UINT64 Hwnd;
    INT32  OrgX, OrgY;           /* where (0,0) of the DC lands on the surface */
    INT32  ClipX, ClipY, ClipW, ClipH; /* in surface coordinates */
    INT32  CurX, CurY;           /* current position for MoveTo/LineTo */
    UINT64 SelectedBrush;
    UINT64 SelectedPen;
    UINT64 SelectedFont;
    UINT64 SelectedBitmap;
} W32K_DC;

static BOOLEAN dc_prepare_window(W32K_DC *dc);
static void dc_dispose_window(W32K_DC *dc);
static void dc_damage(W32K_DC *dc, INT32 x, INT32 y, INT32 w, INT32 h);
static void dc_present(W32K_DC *dc);

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
    UINT16 Generation;
    PKPROCESS Owner;
    UINT64 UserInfo;
    union {
        W32K_DC     Dc;
        W32K_BRUSH  Brush;
        W32K_BITMAP Bitmap;
    } u;
} W32K_OBJECT;

static W32K_OBJECT g_gdi_objects[GDI_MAX_OBJECTS];
static UINT16 g_gdi_next_index = 1;
#define W32K_DC_CACHE 128
static UINT64 g_window_dc[W32K_DC_CACHE];

static PKPROCESS gdi_current_process(void)
{
    PKTHREAD thread = KeGetCurrentThread();
    return thread ? thread->Process : NULL;
}

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
    if (handle >> 32 || !object->Type || object->Owner != gdi_current_process() ||
        (type && object->Type != type))
        return NULL;
    if (object->Upper != (UINT16)(handle >> 16))
        return NULL;
    return object;
}

/* Allocate a typed, generation-tagged handle and publish its owner cell. */
static UINT64 gdi_allocate(UINT16 type, UINT64 user_info, W32K_OBJECT **out)
{
    PKPROCESS owner = gdi_current_process();
    if (!owner || !owner->ProcessId || owner->Exiting) return 0;
    for (UINT16 tries = 0; tries < GDI_MAX_OBJECTS; tries++) {
        UINT16 index = g_gdi_next_index;
        g_gdi_next_index = (UINT16)(index + 1);
        if (g_gdi_next_index >= GDI_MAX_OBJECTS)
            g_gdi_next_index = 1;
        /* Retire exhausted generations instead of reviving stale handles. */
        if (!index || g_gdi_objects[index].Type || g_gdi_objects[index].Generation == 255)
            continue;

        W32K_OBJECT *object = &g_gdi_objects[index];
        UINT16 generation = object->Generation + 1;
        memset(object, 0, sizeof(*object));
        object->Generation = generation;
        object->Type = type;
        object->Upper = (UINT16)((generation << 8) | type);
        object->Owner = owner;
        object->UserInfo = user_info;

        GDICELL64 *cell = &gdi_table()[index];
        cell->pKernelInfo = (UINT64)object;
        cell->ProcessId = owner->ProcessId;
        cell->nUpper = object->Upper;
        cell->nType = type;
        cell->pUserInfo = user_info;

        if (out)
            *out = object;
        return ((UINT64)object->Upper << 16) | index;
    }
    KeLog("[w32k] GDI handle table full\n");
    return 0;
}

static void gdi_free(UINT64 handle)
{
    W32K_OBJECT *object = gdi_object(handle, 0);
    if (!object) return;
    UINT16 index = (UINT16)(handle & 0xFFFF);
    if (object->Type == GDI_TYPE_BITMAP && object->u.Bitmap.Storage)
        ExFreePool(object->u.Bitmap.Storage);
    if (object->Type == GDI_TYPE_DC)
        dc_dispose_window(&object->u.Dc);
    object->Type = 0;
    object->Owner = NULL;
    object->UserInfo = 0;
    for (UINT16 i = 0; i < W32K_DC_CACHE; i++)
        if (g_window_dc[i] == handle) g_window_dc[i] = 0;
    memset(&gdi_table()[index], 0, sizeof(GDICELL64));
}

/* Hand out a zeroed DC_ATTR block from the user-writable arena. */
static UINT64 gdi_allocate_attr(void)
{
    PKPROCESS owner = gdi_current_process();
    for (UINT64 block = PROCESS_GDI_ATTR_VA;
         block < PROCESS_GDI_ATTR_VA + PROCESS_GDI_ATTR_SIZE; block += DCATTR_SIZE) {
        BOOLEAN used = FALSE;
        for (UINT16 i = 1; i < GDI_MAX_OBJECTS; i++)
            if (g_gdi_objects[i].Type && g_gdi_objects[i].Owner == owner &&
                g_gdi_objects[i].UserInfo == block) { used = TRUE; break; }
        if (!used) {
            memset((void *)block, 0, DCATTR_SIZE);
            return block;
        }
    }
    return 0;
}

void W32kInitializeGdi(void)
{
    /* New process CR3 is active here; never reset another process's objects. */
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
    UINT32 pixel = gdi_pixel(colorref);
    dc_damage(dc, x, y, w, h);
    if (!dc->Surface->IsScreen || !dc->Hwnd ||
        W32kScreenRectUnobscured(dc->Hwnd, x, y, w, h)) {
        surface_fill(dc->Surface, x, y, w, h, pixel);
        return;
    }
    for (INT32 row = 0; row < h; ++row)
        for (INT32 col = 0; col < w; ++col)
            if (W32kScreenPixelVisible(dc->Hwnd, x + col, y + row))
                surface_pixel(dc->Surface, x + col, y + row, pixel);
}

static void dc_pixel(W32K_DC *dc, INT32 x, INT32 y, UINT32 pixel)
{
    dc_damage(dc, x, y, 1, 1);
    if (!dc->Surface->IsScreen || !dc->Hwnd ||
        W32kScreenPixelVisible(dc->Hwnd, x, y))
        surface_pixel(dc->Surface, x, y, pixel);
}

static UINT32 blend_pixel(UINT32 fg, UINT32 bg, UINT8 alpha)
{
    UINT32 fr = (fg >> GfxFramebuffer.RedShift) & 0xff;
    UINT32 fgreen = (fg >> GfxFramebuffer.GreenShift) & 0xff;
    UINT32 fb = (fg >> GfxFramebuffer.BlueShift) & 0xff;
    UINT32 br = (bg >> GfxFramebuffer.RedShift) & 0xff;
    UINT32 bgreen = (bg >> GfxFramebuffer.GreenShift) & 0xff;
    UINT32 bb = (bg >> GfxFramebuffer.BlueShift) & 0xff;
    UINT32 inverse = 255u - alpha;
    return GfxColor((UINT8)((fr * alpha + br * inverse + 127) / 255),
                    (UINT8)((fgreen * alpha + bgreen * inverse + 127) / 255),
                    (UINT8)((fb * alpha + bb * inverse + 127) / 255));
}

/*
 * Draw one character of the UI face at a DC-relative position, clipped, with an
 * opaque or transparent background.
 *
 * The face is the proportional Selawik atlas - the same one the window manager
 * draws captions with - because every Win32 text path (TextOut, ExtTextOut,
 * DrawText, GetTextExtent) has to agree on advances, and a fixed 9 pixel grid
 * would mis-measure every string a client lays out. The character cell keeps
 * the console font's 20 pixel line box for vertical placement, which is what a
 * DC's text metrics report.
 */
static void dc_glyph(W32K_DC *dc, INT32 x, INT32 y, UINT32 codepoint,
                     UINT32 fg, UINT32 bg, BOOLEAN opaque)
{
    UINT32 fg_pixel = gdi_pixel(fg), bg_pixel = gdi_pixel(bg);
    const GFX_FONT_GLYPH *glyph = GfxUiGlyph(codepoint);
    INT32 baseline = y + GfxUiAscent();
    INT32 advance = (INT32)GfxUiCharWidth(codepoint);

    if (opaque) {
        /* The background fills the whole cell, as Windows does, so a cleared
         * line has no gaps between glyphs. */
        for (INT32 row = 0; row < GFX_FONT_H; ++row) {
            INT32 px = x, py = y + row, pw = advance, ph = 1;
            if (!dc_clip(dc, &px, &py, &pw, &ph))
                continue;
            for (INT32 col = 0; col < pw; ++col)
                dc_pixel(dc, px + col, py, bg_pixel);
        }
    }
    if (!glyph || !glyph->Width || !glyph->Height)
        return;
    const UINT8 *bits = SvUiFontUiFace.Pixels + glyph->Offset;
    INT32 left = x + glyph->BearingX;
    INT32 top = baseline - glyph->BearingY;
    for (UINT32 row = 0; row < glyph->Height; ++row) {
        INT32 py = top + (INT32)row;
        const UINT8 *source = bits + (UINT64)row * glyph->Width;
        for (UINT32 col = 0; col < glyph->Width; ++col) {
            UINT32 coverage = source[col];
            if (!coverage)
                continue;
            INT32 px = left + (INT32)col, clip_y = py, pw = 1, ph = 1;
            if (!dc_clip(dc, &px, &clip_y, &pw, &ph))
                continue;
            if (dc->Surface->IsScreen && dc->Hwnd &&
                !W32kScreenPixelVisible(dc->Hwnd, px, clip_y))
                continue;
            UINT32 under = dc->Surface->Bits[(UINT32)clip_y *
                                             dc->Surface->Stride +
                                             (UINT32)px];
            dc_pixel(dc, px, clip_y,
                      blend_pixel(fg_pixel, under, (UINT8)coverage));
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
    screen_surface(&dc->DeviceSurface);
    dc->Surface = &dc->DeviceSurface;
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
    if (hwnd && !dc_prepare_window(dc)) {
        gdi_free(hdc);
        return 0;
    }

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
    if (!object) return NULL;
    W32K_DC *dc = &object->u.Dc;
    return !dc->Hwnd || dc_prepare_window(dc) ? dc : NULL;
}

/* The DC's user-mode attribute block, or 0. */
static UINT64 dc_attr(UINT64 hdc)
{
    W32K_OBJECT *object = gdi_object(hdc, GDI_TYPE_DC);
    return object ? object->UserInfo : 0;
}

/* Reaping does not dereference a user VA under another process CR3. */
void W32kCleanupProcessGdi(PKPROCESS process)
{
    for (UINT16 i = 1; i < GDI_MAX_OBJECTS; i++) {
        W32K_OBJECT *object = &g_gdi_objects[i];
        if (!object->Type || object->Owner != process) continue;
        (void)W32kClearSharedHandle(process, TRUE, i);
        UINT64 handle = ((UINT64)object->Upper << 16) | i;
        if (object->Type == GDI_TYPE_BITMAP && object->u.Bitmap.Storage)
            ExFreePool(object->u.Bitmap.Storage);
        if (object->Type == GDI_TYPE_DC)
            dc_dispose_window(&object->u.Dc);
        object->Type = 0;
        object->Owner = NULL;
        object->UserInfo = 0;
        for (UINT16 j = 0; j < W32K_DC_CACHE; j++)
            if (g_window_dc[j] == handle) g_window_dc[j] = 0;
    }
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
