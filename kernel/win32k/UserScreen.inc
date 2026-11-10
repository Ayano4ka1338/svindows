/* win32k screen composition: backdrop, z-ordered windows, pointer.
 *
 * Included by the owning translation unit (kernel/ke/Syscall.c) after the
 * window, message and callback parts, which provide the window records and the
 * message queue. GDI retains each window's client raster in kernel memory.
 * Composition reads the last completed paint and draws backdrop, chrome,
 * clients into an off-screen raster before presenting damage.
 * Moving, exposing or covering a window never requires application repaint.
 */

#define W32K_SLOTS        WINPAINT_SLOTS
#define W32K_SHADOW_MARGIN 10 /* shadow extends nine pixels below the window */

/* Non-client parts, in the order a hit test reports them. */
#define W32K_PART_NONE     0
#define W32K_PART_CLIENT   1
#define W32K_PART_CAPTION  2
#define W32K_PART_MINIMIZE 3
#define W32K_PART_MAXIMIZE 4
#define W32K_PART_CLOSE    5
#define W32K_PART_BORDER   6
#define W32K_PART_MENU     7
#define W32K_MENU_HEIGHT  19

/* Windows 10 frame metrics at 100% scaling come from svindows/Win32k.h, so the
 * services that size a window from its style and the painter below agree. */

/* Window style bits user32 and this file agree on. */
#ifndef WS_BORDER
#define WS_BORDER       0x00800000u
#define WS_DLGFRAME     0x00400000u
#define WS_CAPTION      0x00C00000u
#define WS_SYSMENU      0x00080000u
#define WS_THICKFRAME   0x00040000u
#define WS_MINIMIZEBOX  0x00020000u
#define WS_MAXIMIZEBOX  0x00010000u
#define WS_CHILD        0x40000000u
#define WS_POPUP        0x80000000u
#endif

/* System commands and the messages the window manager itself delivers. */
#define W32K_SC_CLOSE     0xF060
#define W32K_SC_MAXIMIZE  0xF030
#define W32K_SC_MINIMIZE  0xF020
#define W32K_SC_RESTORE   0xF120
#define W32K_WM_MOUSEMOVE   0x0200
#define W32K_WM_LBUTTONDOWN 0x0201
#define W32K_WM_LBUTTONUP   0x0202
#define W32K_WM_KEYDOWN     0x0100
#define W32K_WM_KEYUP       0x0101
#define W32K_WM_CHAR        0x0102
#define W32K_WM_SIZE        0x0005
#define W32K_WM_SETFOCUS    0x0007
#define W32K_WM_KILLFOCUS   0x0008
#define W32K_WM_CLOSE       0x0010
#define W32K_WM_SYSCOMMAND  0x0112
#define W32K_WM_NCACTIVATE  0x0086

typedef struct _W32K_WINDOW_STATE {
    UINT8   Kind;
    UINT8   Visible;
    UINT8   Active;
    UINT8   Minimized;
    UINT8   Maximized;
    UINT8   HotPart;        /* non-client part under the pointer            */
    UINT8   Dragging;       /* moving the window by its caption             */
    UINT8   TrackLeave;
    INT32   X, Y;
    UINT32  W, H;
    INT32   DragPointerX, DragPointerY;
    INT32   DragOriginX, DragOriginY;
    UINT32  RestoreX, RestoreY, RestoreW, RestoreH;
    UINT16  Parent;
    UINT16  ZPrev, ZNext;   /* z-order list, ZNext toward the top           */
    UINT32  Style;          /* cached: the record lives in user memory      */
    UINT32  ExStyle;
    WCHAR   Title[W32K_TITLE_CHARS];
    BOOLEAN MenuPresent,IsStatic;
    UINT32 MenuCount;
    WCHAR MenuLabels[16][64];
} W32K_WINDOW_STATE;

static W32K_WINDOW_STATE g_w32k[W32K_SLOTS];
static UINT16 g_w32k_focus;
static UINT16 g_w32k_top;        /* topmost window slot, 0 = none          */
static UINT16 g_w32k_active;     /* foreground window slot, 0 = none       */
static INT32 g_w32k_pointer_x, g_w32k_pointer_y;
static UINT8 g_w32k_pointer_buttons;
static BOOLEAN g_w32k_pointer_valid;
static UINT16 g_w32k_capture;    /* window holding the mouse capture       */
static UINT16 g_w32k_drag;       /* caption drag keeps its owner across hit tests */
static BOOLEAN g_w32k_shift, g_w32k_ctrl, g_w32k_alt;
static INT32 g_w32k_cascade;     /* CW_USEDEFAULT placement walk           */
static volatile UINT32 g_w32k_xorg_frame_sequence;

static void W32kComposeRect(INT32 x, INT32 y, INT32 width, INT32 height);
static void W32kInvalidateWindow(UINT16 slot);
static void W32kSendWindowMessage(UINT16 slot, UINT32 message, UINT64 wparam,
                                  INT64 lparam);
static void W32kRaise(UINT16 slot);

static void W32kScreenPosition(UINT16 slot, INT32 *x, INT32 *y);
static UINT16 W32kChildAt(UINT16 parent, INT32 x, INT32 y, unsigned depth);

/* --- geometry ------------------------------------------------------------ */

BOOLEAN W32kWindowExists(UINT16 slot)
{
    return slot && slot < W32K_SLOTS && g_w32k[slot].Kind != W32K_KIND_NONE;
}

UINT16 W32kActiveSlot(void)
{
    return g_w32k_active;
}

/* Kernel-side geometry read: the services that answer ring 3 probe the user
 * buffer themselves, so window-to-window arithmetic stays inside the kernel. */
BOOLEAN W32kWindowRect(UINT16 slot, INT32 *x, INT32 *y, UINT32 *width,
                       UINT32 *height)
{
    if (!W32kWindowExists(slot))
        return FALSE;
    W32kScreenPosition(slot, x, y);
    *width = g_w32k[slot].W;
    *height = g_w32k[slot].H;
    return TRUE;
}

static BOOLEAN W32kWindowLocalRect(UINT16 slot, INT32 *x, INT32 *y,
                                    UINT32 *width, UINT32 *height)
{
    if (!W32kWindowExists(slot))
        return FALSE;
    *x = g_w32k[slot].X;
    *y = g_w32k[slot].Y;
    *width = g_w32k[slot].W;
    *height = g_w32k[slot].H;
    return TRUE;
}

void W32kRaiseSlot(UINT16 slot)
{
    W32kRaise(slot);
}

/* Screen rectangle of a window's client area. */
void W32kClientOrigin(UINT16 slot, INT32 *x, INT32 *y, INT32 *width,
                      INT32 *height)
{
    W32K_WINDOW_STATE *window = &g_w32k[slot];
    INT32 top = window->Kind == W32K_KIND_TOPLEVEL ? W32K_BORDER : 0;
    INT32 border = window->Kind == W32K_KIND_TOPLEVEL ? W32K_BORDER : 0;
    if (window->Kind == W32K_KIND_TOPLEVEL) {
        /* The style was cached at creation: the window record itself lives in
         * the owning process's address space and the input thread has no
         * mapping for it. */
        if (window->Style & WS_CAPTION)
            top = W32K_CAPTION_HEIGHT;
    }
    if (window->Kind == W32K_KIND_TOPLEVEL && window->MenuPresent)
        top += W32K_MENU_HEIGHT;
    INT32 cw = (INT32)window->W - border * 2;
    INT32 ch = (INT32)window->H - top - border;
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;
    W32kScreenPosition(slot, x, y);
    *x += border; *y += top;
    *width = cw;
    *height = ch;
}

static void W32kButtonRect(UINT16 slot, UINT8 part, INT32 *x, INT32 *y,
                           UINT32 *w, UINT32 *h)
{
    W32K_WINDOW_STATE *window = &g_w32k[slot];
    INT32 index = part == W32K_PART_CLOSE ? 0
                : part == W32K_PART_MAXIMIZE ? 1 : 2;
    *w = W32K_BUTTON_WIDTH;
    *h = W32K_CAPTION_HEIGHT - W32K_BORDER;
    *x = window->X + (INT32)window->W - W32K_BORDER -
         (index + 1) * W32K_BUTTON_WIDTH;
    *y = window->Y + W32K_BORDER;
}

static UINT8 W32kHitTest(UINT16 slot, INT32 x, INT32 y, BOOLEAN *in_client)
{
    W32K_WINDOW_STATE *window = &g_w32k[slot];
    UINT32 style = window->Style;

    if (window->Kind == W32K_KIND_CHILD) {
        INT32 sx, sy; W32kScreenPosition(slot, &sx, &sy);
        *in_client = x >= sx && y >= sy && x < sx + (INT32)window->W &&
                     y < sy + (INT32)window->H;
        return *in_client ? W32K_PART_CLIENT : W32K_PART_NONE;
    }
    *in_client = FALSE;
    if (x < window->X || y < window->Y ||
        x >= window->X + (INT32)window->W ||
        y >= window->Y + (INT32)window->H)
        return W32K_PART_NONE;
    if (window->Kind != W32K_KIND_TOPLEVEL)
        return W32K_PART_CLIENT;

    if (style & WS_CAPTION) {
        if (y < window->Y + W32K_CAPTION_HEIGHT) {
            if (style & WS_SYSMENU) {
                for (UINT8 part = W32K_PART_MINIMIZE;
                     part <= W32K_PART_CLOSE; ++part) {
                    INT32 bx, by;
                    UINT32 bw, bh;
                    W32kButtonRect(slot, part, &bx, &by, &bw, &bh);
                    if (x >= bx && x < bx + (INT32)bw && y >= by &&
                        y < by + (INT32)bh)
                        return part;
                }
            }
            return W32K_PART_CAPTION;
        }
    }
    INT32 menu_y=window->Y+((style&WS_CAPTION)?W32K_CAPTION_HEIGHT:W32K_BORDER);
    if(window->MenuPresent&&y>=menu_y&&y<menu_y+W32K_MENU_HEIGHT)return W32K_PART_MENU;
    INT32 cx, cy, cw, ch;
    W32kClientOrigin(slot, &cx, &cy, &cw, &ch);
    if (x >= cx && y >= cy && x < cx + cw && y < cy + ch) {
        *in_client = TRUE;
        return W32K_PART_CLIENT;
    }
    return W32K_PART_BORDER;
}

/* --- z-order ------------------------------------------------------------- */

static void W32kUnlink(UINT16 slot)
{
    UINT64 flags = KiIrqSave();
    W32K_WINDOW_STATE *window = &g_w32k[slot];
    if (g_w32k_top == slot)
        g_w32k_top = window->ZPrev;
    if (window->ZPrev)
        g_w32k[window->ZPrev].ZNext = window->ZNext;
    if (window->ZNext)
        g_w32k[window->ZNext].ZPrev = window->ZPrev;
    window->ZPrev = window->ZNext = 0;
    KiIrqRestore(flags);
}

static void W32kRaise(UINT16 slot)
{
    if (!W32kWindowExists(slot))
        return;
    if (g_w32k[slot].Parent) W32kRaise(W32kRootSlot(slot));
    UINT64 flags = KiIrqSave();
    W32kUnlink(slot);
    /* Desktop < application windows < taskbar and shell-style popup windows. */
    UINT8 rank = g_w32k[slot].Kind == W32K_KIND_DESKTOP ? 0 :
                 g_w32k[slot].Kind == W32K_KIND_TASKBAR ? 2 : 1;
    UINT16 below = 0;
    for (UINT16 current = g_w32k_top; current;
         current = g_w32k[current].ZPrev) {
        UINT8 current_rank = g_w32k[current].Kind == W32K_KIND_DESKTOP ? 0 :
                             g_w32k[current].Kind == W32K_KIND_TASKBAR ? 2 : 1;
        if (current_rank <= rank) { below = current; break; }
    }
    if (below) {
        UINT16 above = g_w32k[below].ZNext;
        g_w32k[slot].ZPrev = below;
        g_w32k[slot].ZNext = above;
        g_w32k[below].ZNext = slot;
        if (above) g_w32k[above].ZPrev = slot;
        else g_w32k_top = slot;
    } else if (g_w32k_top) {
        UINT16 bottom = g_w32k_top;
        while (g_w32k[bottom].ZPrev) bottom = g_w32k[bottom].ZPrev;
        g_w32k[slot].ZPrev = 0;
        g_w32k[slot].ZNext = bottom;
        g_w32k[bottom].ZPrev = slot;
    } else {
        g_w32k[slot].ZPrev = g_w32k[slot].ZNext = 0;
        g_w32k_top = slot;
    }
    KiIrqRestore(flags);
}

/* ZPrev points down; HWND_BOTTOM and sibling handles must retain the
 * requested ordering instead of implicitly raising a background control. */
void W32kSetZOrder(UINT16 slot, UINT64 after)
{
    if (!W32kWindowExists(slot)) return;
    if (after == 0 || after == (UINT64)-1 || after == (UINT64)-2) {
        W32kRaise(slot);
        return;
    }
    UINT16 anchor = 0;
    if (after == 1) {
        for (UINT16 w = g_w32k_top; w; w = g_w32k[w].ZPrev)
            if (w != slot && g_w32k[w].Parent == g_w32k[slot].Parent)
                anchor = w;
    } else {
        anchor = (UINT16)after;
        if (!W32kWindowExists(anchor) || anchor == slot ||
            g_w32k[anchor].Parent != g_w32k[slot].Parent) return;
    }
    if (!anchor) return;
    UINT64 flags = KiIrqSave();
    W32kUnlink(slot);
    UINT16 below = g_w32k[anchor].ZPrev;
    g_w32k[slot].ZPrev = below;
    g_w32k[slot].ZNext = anchor;
    g_w32k[anchor].ZPrev = slot;
    if (below) g_w32k[below].ZNext = slot;
    W32kScreenCompose();
    KiIrqRestore(flags);
}

static UINT16 W32kWindowAt(INT32 x, INT32 y)
{
    for (UINT16 slot = g_w32k_top; slot; slot = g_w32k[slot].ZPrev) {
        W32K_WINDOW_STATE *window = &g_w32k[slot];
        if (!window->Kind || !window->Visible || window->Minimized || window->Parent ||
            window->Kind == W32K_KIND_CHILD || (window->Style & 0x08000000u))
            continue;
        if (x >= window->X && y >= window->Y &&
            x < window->X + (INT32)window->W &&
            y < window->Y + (INT32)window->H)
            return W32kChildAt(slot, x, y, 0);
    }
    return 0;
}

static UINT16 W32kFocusWindow(void)
{
    if (!W32kWindowExists(g_w32k_active) || !g_w32k[g_w32k_active].Visible ||
        g_w32k[g_w32k_active].Minimized || (g_w32k[g_w32k_active].Style & 0x08000000u)) return 0;
    if (W32kWindowExists(g_w32k_focus) && W32kRootSlot(g_w32k_focus) == g_w32k_active) {
        UINT16 slot = g_w32k_focus;
        for (unsigned depth = 0; slot && depth < W32K_SLOTS; ++depth) {
            if (!g_w32k[slot].Visible || g_w32k[slot].Minimized || (g_w32k[slot].Style & 0x08000000u))
                return g_w32k_active;
            slot = g_w32k[slot].Parent;
        }
        return g_w32k_focus;
    }
    return g_w32k_active;
}

static void W32kSetActive(UINT16 slot)
{
    if (g_w32k_active == slot)
        return;
    UINT64 flags = KiIrqSave();
    UINT16 previous = g_w32k_active;
    g_w32k_active = slot;
    if (W32kWindowExists(previous)) {
        g_w32k[previous].Active = FALSE;
        W32kComposeRect(g_w32k[previous].X-W32K_SHADOW_MARGIN, g_w32k[previous].Y-W32K_SHADOW_MARGIN,
                        (INT32)g_w32k[previous].W+2*W32K_SHADOW_MARGIN,
                        (INT32)g_w32k[previous].H+2*W32K_SHADOW_MARGIN);
    }
    if (W32kWindowExists(slot)) {
        g_w32k[slot].Active = TRUE;
        W32kComposeRect(g_w32k[slot].X-W32K_SHADOW_MARGIN, g_w32k[slot].Y-W32K_SHADOW_MARGIN,
                        (INT32)g_w32k[slot].W+2*W32K_SHADOW_MARGIN,
                        (INT32)g_w32k[slot].H+2*W32K_SHADOW_MARGIN);
    }
    if (previous) W32kSendWindowMessage(previous, W32K_WM_NCACTIVATE, 0, 0);
    if (slot)
        W32kSendWindowMessage(slot, W32K_WM_NCACTIVATE, 1, 0);
    KiIrqRestore(flags);
}

/* --- painting ------------------------------------------------------------ */

/* Activation belongs to a root window; keyboard focus may belong to a child. */
void W32kActivateWindow(UINT64 hwnd)
{
    W32kSetActive(W32kRootSlot((UINT16)hwnd));
}
void W32kSetFocus(UINT64 hwnd)
{
    UINT16 slot = hwnd ? (UINT16)(hwnd & 0xFFFF) : 0;
    if (!W32kWindowExists(slot))
        slot = 0;
    UINT16 previous = W32kFocusWindow();
    if (previous == slot)
        return;
    W32kSetActive(W32kRootSlot(slot));
    g_w32k_focus = slot;
    if (previous)
        W32kSendWindowMessage(previous, W32K_WM_KILLFOCUS, slot ? KiUserWindowHandle(slot) : 0, 0);
    if (slot)
        W32kSendWindowMessage(slot, W32K_WM_SETFOCUS, previous ? KiUserWindowHandle(previous) : 0, 0);
}

static void W32kInvalidateWindow(UINT16 slot)
{
    if (!W32kWindowExists(slot))
        return;
    KiUserInvalidateWindow(slot);
}

#include "ScreenRaster.inc"
#include "UserChildren.inc"

static void W32kFillRectClipped(INT32 x, INT32 y, INT32 width, INT32 height,
                                UINT32 color, INT32 cx, INT32 cy, INT32 cw,
                                INT32 ch)
{
    if (x < cx) { width -= cx - x; x = cx; }
    if (y < cy) { height -= cy - y; y = cy; }
    if (x + width > cx + cw) width = cx + cw - x;
    if (y + height > cy + ch) height = cy + ch - y;
    if (width <= 0 || height <= 0)
        return;
    W32kRasterFill(x, y, width, height,
                GfxColor((UINT8)(color >> 16), (UINT8)(color >> 8),
                         (UINT8)color));
}

/* The desktop layer is a flat Windows-style background. */
static void W32kPaintBackdrop(INT32 x, INT32 y, INT32 width, INT32 height)
{
    UINT32 sw = GfxFramebuffer.Width, sh = GfxFramebuffer.Height;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > (INT32)sw) width = (INT32)sw - x;
    if (y + height > (INT32)sh) height = (INT32)sh - y;
    if (width <= 0 || height <= 0 || !GfxAvailable())
        return;
    for (INT32 row = 0; row < height; ++row) {
        INT32 t = ((y + row) * 255) / (sh > 1 ? (INT32)sh - 1 : 1);
        UINT8 r = (UINT8)((0x0A * (255 - t) + 0x03 * t) / 255);
        UINT8 g = (UINT8)((0x32 * (255 - t) + 0x10 * t) / 255);
        UINT8 b = (UINT8)((0x53 * (255 - t) + 0x1E * t) / 255);
        W32kRasterFill(x, y + row, width, 1, GfxColor(r, g, b));
    }
}

static void W32kGlyphMinimize(INT32 cx, INT32 cy, UINT32 color)
{
    W32kRasterFill(cx - 5, cy, 10, 1, color);
}

static void W32kGlyphMaximize(INT32 cx, INT32 cy, UINT32 color)
{
    W32kRasterFill((INT32)(cx - 5), (INT32)(cy - 5), 10, 1, color);
    W32kRasterFill((INT32)(cx - 5), (INT32)(cy + 4), 10, 1, color);
    W32kRasterFill((INT32)(cx - 5), (INT32)(cy - 5), 1, 10, color);
    W32kRasterFill((INT32)(cx + 4), (INT32)(cy - 5), 1, 10, color);
}

static void W32kGlyphRestore(INT32 cx, INT32 cy, UINT32 color)
{
    W32kRasterFill((INT32)(cx - 3), (INT32)(cy - 2), 8, 1, color);
    W32kRasterFill((INT32)(cx - 3), (INT32)(cy + 5), 8, 1, color);
    W32kRasterFill((INT32)(cx - 3), (INT32)(cy - 2), 1, 8, color);
    W32kRasterFill((INT32)(cx + 4), (INT32)(cy - 2), 1, 8, color);
    W32kRasterFill((INT32)(cx - 5), (INT32)(cy - 4), 8, 1, color);
    W32kRasterFill((INT32)(cx - 5), (INT32)(cy - 4), 1, 8, color);
}

static void W32kGlyphClose(INT32 cx, INT32 cy, UINT32 color)
{
    for (INT32 i = -5; i <= 5; ++i) {
        W32kRasterPixel((INT32)(cx + i), (INT32)(cy + i), color);
        W32kRasterPixel((INT32)(cx + i), (INT32)(cy - i), color);
    }
}

/* The Windows 10 frame: 1 pixel border, a light caption with the title, and
 * the three caption buttons. The button under the pointer is highlighted the
 * way Windows does it - light grey for minimize and maximize, #E81123 for
 * close. */
static void W32kPaintFrame(UINT16 slot, INT32 cx, INT32 cy, INT32 cw, INT32 ch)
{
    W32K_WINDOW_STATE *window = &g_w32k[slot];
    UINT32 style = window->Style;

    BOOLEAN active = g_w32k_active == slot;
    INT32 x = window->X, y = window->Y;
    INT32 w = (INT32)window->W, h = (INT32)window->H;
    UINT32 border = active ? 0xC8C8C8 : 0xDCDCDC;

    static const UINT8 shadow_alpha[8] = {56, 42, 30, 21, 14, 9, 5, 2};
    for (INT32 i = 8; i >= 1; --i) {
        UINT32 alpha = shadow_alpha[i-1];
        if (!active) alpha = alpha*3/4;
        INT32 sx = x-i, sy = y-i+2, sw = w+i*2, sh = h+i*2;
        W32kRasterShadow(sx, sy, sw, 1, alpha);
        W32kRasterShadow(sx, sy+sh-1, sw, 1, alpha);
        W32kRasterShadow(sx, sy+1, 1, sh-2, alpha);
        W32kRasterShadow(sx+sw-1, sy+1, 1, sh-2, alpha);
    }
    W32kRasterShadow(x, y+h, w, 2, active ? 64 : 48);
    /* The completed client raster is layered over this base before presenting
     * the damage, so no intermediate shadow or fill reaches the display. */
    W32kFillRectClipped(x, y, w, h, 0xFFFFFF, cx, cy, cw, ch);
    W32kFillRectClipped(x, y, w, 1, border, cx, cy, cw, ch);
    W32kFillRectClipped(x, y, 1, h, border, cx, cy, cw, ch);
    W32kFillRectClipped(x + w - 1, y, 1, h, border, cx, cy, cw, ch);
    W32kFillRectClipped(x, y + h - 1, w, 1, border, cx, cy, cw, ch);
    if(window->MenuPresent){
        INT32 my=y+((style&WS_CAPTION)?W32K_CAPTION_HEIGHT:W32K_BORDER),mx=x+1;
        W32kFillRectClipped(mx,my,w-2,W32K_MENU_HEIGHT,0xf0f0f0,cx,cy,cw,ch);
        for(UINT32 i=0;i<window->MenuCount;i++){
            INT32 width=16;for(UINT32 j=0;window->MenuLabels[i][j];j++)width+=GfxUiCharWidth(window->MenuLabels[i][j]);
            INT32 available=x+w-1-(mx+8);if(available<=0)break;
            if(available>width-16)available=width-16;
            W32kRasterText(mx+8,my+(W32K_MENU_HEIGHT+GfxUiAscent())/2,window->MenuLabels[i],0,available,my,W32K_MENU_HEIGHT);
            mx+=width;
        }
    }
    if (!(style & WS_CAPTION))
        return;

    if (style & WS_SYSMENU) {
        for (UINT8 part = W32K_PART_MINIMIZE; part <= W32K_PART_CLOSE; ++part) {
            INT32 bx, by;
            UINT32 bw, bh;
            W32kButtonRect(slot, part, &bx, &by, &bw, &bh);
            if (window->HotPart == part) {
                UINT32 fill = part == W32K_PART_CLOSE ? 0xE81123 : 0xE5E5E5;
                W32kFillRectClipped(bx, by, (INT32)bw, (INT32)bh, fill, cx, cy,
                                    cw, ch);
            }
            UINT32 glyph = (window->HotPart == part &&
                            part == W32K_PART_CLOSE) ? 0xFFFFFF : 0x000000;
            UINT32 color = GfxColor((UINT8)(glyph >> 16), (UINT8)(glyph >> 8),
                                    (UINT8)glyph);
            INT32 gx = bx + (INT32)bw / 2, gy = by + (INT32)bh / 2;
            if (part == W32K_PART_MINIMIZE)
                W32kGlyphMinimize(gx, gy, color);
            else if (part == W32K_PART_MAXIMIZE) {
                if (window->Maximized)
                    W32kGlyphRestore(gx, gy, color);
                else
                    W32kGlyphMaximize(gx, gy, color);
            } else
                W32kGlyphClose(gx, gy, color);
        }
    }

    if (window->Title[0]) {
        /* The UI face is proportional, so the title is measured and drawn with
         * the same metrics a client would get from GetTextExtentPoint32W. */
        INT32 baseline = y + (W32K_CAPTION_HEIGHT + GfxUiAscent()) / 2;
        INT32 title_width = w - 16;
        if (style & WS_SYSMENU) title_width -= 3 * W32K_BUTTON_WIDTH;
        W32kRasterText(x + 8, baseline, window->Title, 0x000000,
                       title_width, y + W32K_BORDER,
                       W32K_CAPTION_HEIGHT - W32K_BORDER);
    }
}

/* A window DC is backed by the screen. Client painting must never overwrite
 * a higher window, regardless of the order in which WM_PAINT gets dispatched.
 * Screen coordinates are used here because each DC already translates its
 * client origin before writing pixels. */
BOOLEAN W32kScreenPixelVisible(UINT64 hwnd, INT32 x, INT32 y)
{
    UINT16 slot = (UINT16)hwnd;
    if (!W32kWindowExists(slot) || !g_w32k[slot].Visible ||
        g_w32k[slot].Minimized)
        return FALSE;
    for (UINT16 above = g_w32k[slot].ZNext; above;
         above = g_w32k[above].ZNext) {
        const W32K_WINDOW_STATE *window = &g_w32k[above];
        if (window->Visible && !window->Minimized &&
            x >= window->X && y >= window->Y &&
            x < window->X + (INT32)window->W &&
            y < window->Y + (INT32)window->H)
            return FALSE;
    }
    return TRUE;
}

/* Fast path for large fills and surface copies when no higher window touches
 * the destination rectangle. A partly covered rectangle uses pixel clipping. */
BOOLEAN W32kScreenRectUnobscured(UINT64 hwnd, INT32 x, INT32 y,
                                 INT32 width, INT32 height)
{
    UINT16 slot = (UINT16)hwnd;
    if (!W32kWindowExists(slot) || !g_w32k[slot].Visible ||
        g_w32k[slot].Minimized || width <= 0 || height <= 0)
        return FALSE;
    for (UINT16 above = g_w32k[slot].ZNext; above;
         above = g_w32k[above].ZNext) {
        const W32K_WINDOW_STATE *window = &g_w32k[above];
        if (window->Visible && !window->Minimized &&
            x < window->X + (INT32)window->W &&
            y < window->Y + (INT32)window->H &&
            x + width > window->X && y + height > window->Y)
            return FALSE;
    }
    return TRUE;
}

/* Assemble one complete damage rectangle from retained layers. No WM_PAINT
 * is generated here: visibility changes do not modify application content. */
static void W32kComposeRect(INT32 x, INT32 y, INT32 width, INT32 height)
{
    if (!GfxAvailable() || width <= 0 || height <= 0)
        return;
    INT32 sw = (INT32)GfxFramebuffer.Width, sh = (INT32)GfxFramebuffer.Height;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > sw) width = sw - x;
    if (y + height > sh) height = sh - y;
    if (width <= 0 || height <= 0)
        return;

    /* The current kernel is UP. Keep geometry, front rasters and the shared
     * composition scratch consistent across the input worker and GDI calls. */
    UINT64 flags = KiIrqSave();
    if (!W32kEnsureComposite()) { KiIrqRestore(flags); return; }
    HalConsoleSetGraphicsMode(TRUE);
    g_w32k_clip_x = x; g_w32k_clip_y = y;
    g_w32k_clip_w = width; g_w32k_clip_h = height;

    W32kPaintBackdrop(x, y, width, height);

    UINT16 list[W32K_SLOTS];
    UINT32 count = 0;
    for (UINT16 slot = g_w32k_top; slot && count < W32K_SLOTS;
         slot = g_w32k[slot].ZPrev)
        list[count++] = slot;
    for (UINT32 i = count; i > 0; --i) {
        UINT16 slot = list[i - 1];
        W32K_WINDOW_STATE *window = &g_w32k[slot];
        if (!window->Kind || !window->Visible || window->Minimized ||
            window->Kind == W32K_KIND_CHILD || !window->W || !window->H)
            continue;
        if (window->Kind == W32K_KIND_DESKTOP)
            continue; /* the backdrop layer already covers the screen */
        INT32 margin = window->Kind == W32K_KIND_TOPLEVEL ? W32K_SHADOW_MARGIN : 0;
        if (window->X - margin >= x + width || window->Y - margin >= y + height ||
            window->X + (INT32)window->W + margin <= x ||
            window->Y + (INT32)window->H + margin <= y)
            continue;
        if (window->Kind == W32K_KIND_TOPLEVEL)
            W32kPaintFrame(slot, x, y, width, height);
        else
            W32kFillRectClipped(window->X, window->Y, (INT32)window->W,
                                (INT32)window->H, 0x1F1F1F, x, y, width,
                                height);
        W32kCopyWindowClient(slot, g_w32k_composite, g_w32k_composite_w,
                             x, y, width, height);
        W32kPaintChildren(slot, x, y, width, height, 0);
    }

    W32kPresentDamage(x, y, width, height);
    __sync_add_and_fetch(&g_w32k_xorg_frame_sequence, 1);
    KiIrqRestore(flags);
}

void W32kScreenDamage(INT32 x, INT32 y, INT32 width, INT32 height)
{
    W32kComposeRect(x, y, width, height);
}

void W32kScreenCompose(void)
{
    W32kComposeRect(0, 0, (INT32)GfxFramebuffer.Width,
                    (INT32)GfxFramebuffer.Height);
}

/* --- lifecycle ----------------------------------------------------------- */

/* CW_USEDEFAULT arrives as 0x80000000; Windows cascades such windows instead
 * of placing them at the raw value, which would be far off screen. Zero
 * dimensions are literal and must not create a default-sized visible window. */
static void W32kResolveDefaultPosition(INT32 *x, INT32 *y, UINT32 *w, UINT32 *h)
{
    INT32 sw = (INT32)GfxFramebuffer.Width, sh = (INT32)GfxFramebuffer.Height;
    if ((UINT32)*w == 0x80000000u)
        *w = sw > 900 ? 800 : (UINT32)(sw * 3 / 4);
    if ((UINT32)*h == 0x80000000u)
        *h = sh > 700 ? 600 : (UINT32)(sh * 2 / 3);
    if (*w > (UINT32)(sw - 40))
        *w = (UINT32)(sw - 40);
    if (*h > (UINT32)(sh - 80))
        *h = (UINT32)(sh - 80);
    if ((UINT32)*x == 0x80000000u) {
        g_w32k_cascade = (g_w32k_cascade + 1) % 8;
        *x = 24 + g_w32k_cascade * 24;
        *y = 24 + g_w32k_cascade * 24;
    } else if ((UINT32)*y == 0x80000000u) {
        *y = 24 + g_w32k_cascade * 24;
    }
}

UINT8 W32kClassifyWindow(const WCHAR *class_name, UINT32 style)
{
    if (style & WS_CHILD) return W32K_KIND_CHILD;
    if (!class_name) return W32K_KIND_TOPLEVEL;
    for (UINT32 i = 0; class_name[i]; ++i) {
        if (class_name[i] == 'W' && class_name[i + 1] == 'o' &&
            class_name[i + 2] == 'r' && class_name[i + 3] == 'k' &&
            class_name[i + 4] == 'e' && class_name[i + 5] == 'r' &&
            class_name[i + 6] == 'W')
            return W32K_KIND_DESKTOP;
        if (class_name[i] == 'P' && class_name[i + 1] == 'r' &&
            class_name[i + 2] == 'o' && class_name[i + 3] == 'g' &&
            class_name[i + 4] == 'm' && class_name[i + 5] == 'a' &&
            class_name[i + 6] == 'n')
            return W32K_KIND_DESKTOP;
        if (class_name[i] == 'S' && class_name[i + 1] == 'h' &&
            class_name[i + 2] == 'e' && class_name[i + 3] == 'l' &&
            class_name[i + 4] == 'l' && class_name[i + 5] == '_')
            return W32K_KIND_TASKBAR;
    }
    return W32K_KIND_TOPLEVEL;
}

void W32kScreenWindowCreated(UINT16 slot, const WCHAR *title, UINT32 style,
                             INT32 x, INT32 y, UINT32 w, UINT32 h, UINT8 kind)
{
    if (!slot || slot >= W32K_SLOTS)
        return;
    UINT64 flags = KiIrqSave();
    W32K_WINDOW_STATE *window = &g_w32k[slot];
    memset(window, 0, sizeof(*window));
    window->Kind = kind;
    window->Visible = !!(style & 0x10000000u);
    /* The style decides which parts of the frame exist, so it is cached here:
     * the window record itself belongs to the owning process's address space
     * and the compositor also runs on threads that cannot reach it. */
    window->Style = style;
    if (kind == W32K_KIND_DESKTOP)
        window->Style = WS_POPUP;
    if (kind == W32K_KIND_TOPLEVEL)
        W32kResolveDefaultPosition(&x, &y, &w, &h);
    window->X = x;
    window->Y = y;
    window->W = w;
    window->H = h;
    if (title) {
        UINT32 i = 0;
        for (; i < W32K_TITLE_CHARS - 1 && title[i]; ++i)
            window->Title[i] = title[i];
        window->Title[i] = 0;
    }
    W32kRaise(slot);
    if (kind == W32K_KIND_DESKTOP) {
        window->Visible = TRUE;
        W32kScreenCompose();
    } else {
        W32kScreenCompose();
    }
    (void)style;
    KiIrqRestore(flags);
}

void W32kScreenWindowDestroyed(UINT16 slot)
{
    if (!W32kWindowExists(slot))
        return;
    UINT64 flags = KiIrqSave();
    INT32 x, y; W32kScreenPosition(slot, &x, &y);
    INT32 w = (INT32)g_w32k[slot].W, h = (INT32)g_w32k[slot].H;
    W32kUnlink(slot);
    if (g_w32k_active == slot)
        g_w32k_active = 0;
    if (g_w32k_capture == slot)
        g_w32k_capture = 0;
    if (g_w32k_focus == slot) g_w32k_focus = 0;
    if (g_w32k_drag == slot)
        g_w32k_drag = 0;
    memset(&g_w32k[slot], 0, sizeof(g_w32k[slot]));
    W32kComposeRect(x - W32K_SHADOW_MARGIN, y - W32K_SHADOW_MARGIN,
                    w + 2 * W32K_SHADOW_MARGIN, h + 2 * W32K_SHADOW_MARGIN);
    KiIrqRestore(flags);
}

void W32kScreenSetTitle(UINT16 slot, const WCHAR *title)
{
    if (!W32kWindowExists(slot))
        return;
    UINT64 flags = KiIrqSave();
    UINT32 i = 0;
    if (title)
        for (; i < W32K_TITLE_CHARS - 1 && title[i]; ++i)
            g_w32k[slot].Title[i] = title[i];
    g_w32k[slot].Title[i] = 0;
    W32kComposeRect(g_w32k[slot].X, g_w32k[slot].Y, (INT32)g_w32k[slot].W,
                    W32K_CAPTION_HEIGHT + 2);
    KiIrqRestore(flags);
}

/* SetWindowTextW reaches the frame by handle, the way user32 calls it. */
void W32kSetWindowText(UINT64 hwnd, const WCHAR *text)
{
    W32kScreenSetTitle((UINT16)(hwnd & 0xFFFF), text);
}

void W32kMoveWindow(UINT64 hwnd, INT32 x, INT32 y, UINT32 width, UINT32 height)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!W32kWindowExists(slot))
        return;
    UINT64 flags = KiIrqSave();
    W32K_WINDOW_STATE *window = &g_w32k[slot];
    if (window->X == x && window->Y == y && window->W == width &&
        window->H == height)
    {
        KiIrqRestore(flags);
        return;
    }
    INT32 old_x, old_y; W32kScreenPosition(slot, &old_x, &old_y);
    UINT32 old_w = window->W, old_h = window->H;
    BOOLEAN resized = window->W != width || window->H != height;
    window->X = x;
    window->Y = y;
    window->W = width;
    window->H = height;
    if (window->Kind == W32K_KIND_DESKTOP) {
        W32kScreenCompose();
        KiIrqRestore(flags);
        return;
    }
    W32kScreenPosition(slot, &x, &y);
    INT32 damage_left = old_x < x ? old_x : x;
    INT32 damage_top = old_y < y ? old_y : y;
    INT32 old_right = old_x + (INT32)old_w;
    INT32 new_right = x + (INT32)width;
    INT32 old_bottom = old_y + (INT32)old_h;
    INT32 new_bottom = y + (INT32)height;
    INT32 damage_right = old_right > new_right ? old_right : new_right;
    INT32 damage_bottom = old_bottom > new_bottom ? old_bottom : new_bottom;
    /* Recompose the union once so the old and new positions never appear as
     * two separately presented intermediate frames during a drag. */
    W32kComposeRect(damage_left - W32K_SHADOW_MARGIN, damage_top - W32K_SHADOW_MARGIN,
                    damage_right - damage_left + 2 * W32K_SHADOW_MARGIN,
                    damage_bottom - damage_top + 2 * W32K_SHADOW_MARGIN);
    KiIrqRestore(flags);
    if (resized && window->Visible) {
        W32kInvalidateWindow(slot);
        INT32 cx, cy, cw, ch;
        W32kClientOrigin(slot, &cx, &cy, &cw, &ch);
        W32kSendWindowMessage(slot, W32K_WM_SIZE, 0,
                              (INT64)(UINT32)((((UINT32)ch & 0xFFFF) << 16) |
                                              ((UINT32)cw & 0xFFFF)));
    }
}

static void W32kShowWindowLocked(UINT64 hwnd, UINT32 command)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!W32kWindowExists(slot))
        return;
    if ((command == 0 || command == 2 || command == 6 || command == 7) &&
        g_w32k_drag == slot) {
        g_w32k_drag = 0;
        g_w32k[slot].Dragging = 0;
    }
    if (g_w32k[slot].Kind == W32K_KIND_DESKTOP) {
        g_w32k[slot].Visible = TRUE;
        W32kScreenCompose();
        return;
    }
    BOOLEAN visible = command != 0;
    if (g_w32k[slot].Kind == W32K_KIND_CHILD) {
        g_w32k[slot].Visible = visible;
        if (visible) W32kInvalidateWindow(slot);
        INT32 sx, sy; W32kScreenPosition(slot, &sx, &sy);
        W32kComposeRect(sx, sy, (INT32)g_w32k[slot].W, (INT32)g_w32k[slot].H);
        return;
    }
    if (command == 2 || command == 6 || command == 7) {
        g_w32k[slot].Visible = TRUE;
        g_w32k[slot].Minimized = TRUE;
        if (g_w32k_active == slot) W32kSetFocus(0);
        W32kComposeRect(g_w32k[slot].X - W32K_SHADOW_MARGIN,
                        g_w32k[slot].Y - W32K_SHADOW_MARGIN,
                        (INT32)g_w32k[slot].W + 2 * W32K_SHADOW_MARGIN,
                        (INT32)g_w32k[slot].H + 2 * W32K_SHADOW_MARGIN);
        return;
    }
    g_w32k[slot].Visible = visible;
    if (visible) {
        g_w32k[slot].Minimized = FALSE;
        W32kInvalidateWindow(slot);
        W32kRaise(slot);
        if (command != 4 && command != 8) {
            UINT16 focus = W32kWindowExists(g_w32k_focus) &&
                           W32kRootSlot(g_w32k_focus) == slot ? g_w32k_focus : slot;
            W32kSetFocus(KiUserWindowHandle(focus));
        }
        W32kComposeRect(g_w32k[slot].X - W32K_SHADOW_MARGIN,
                        g_w32k[slot].Y - W32K_SHADOW_MARGIN,
                        (INT32)g_w32k[slot].W + 2 * W32K_SHADOW_MARGIN,
                        (INT32)g_w32k[slot].H + 2 * W32K_SHADOW_MARGIN);
        INT32 cx, cy, cw, ch;
        W32kClientOrigin(slot, &cx, &cy, &cw, &ch);
        W32kSendWindowMessage(slot, W32K_WM_SIZE, 0,
                              (INT64)(UINT32)((((UINT32)ch & 0xFFFF) << 16) |
                                              ((UINT32)cw & 0xFFFF)));
    } else {
        W32kComposeRect(g_w32k[slot].X - W32K_SHADOW_MARGIN,
                        g_w32k[slot].Y - W32K_SHADOW_MARGIN,
                        (INT32)g_w32k[slot].W + 2 * W32K_SHADOW_MARGIN,
                        (INT32)g_w32k[slot].H + 2 * W32K_SHADOW_MARGIN);
        if (g_w32k_active == slot)
            W32kSetFocus(0);
    }
}

void W32kShowWindow(UINT64 hwnd, UINT32 command)
{
    UINT64 flags = KiIrqSave();
    W32kShowWindowLocked(hwnd, command);
    KiIrqRestore(flags);
}

BOOLEAN W32kGetRect(UINT64 hwnd, INT32 *rect, BOOLEAN client)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!W32kWindowExists(slot))
        return FALSE;
    if (!rect || !MmProbeForWrite((UINT64)rect, 16))
        return FALSE;
    if (client) {
        INT32 x, y, w, h;
        W32kClientOrigin(slot, &x, &y, &w, &h);
        rect[0] = 0;
        rect[1] = 0;
        rect[2] = w;
        rect[3] = h;
        return TRUE;
    }
    UINT32 width, height;
    if (!W32kWindowRect(slot, &rect[0], &rect[1], &width, &height)) return FALSE;
    rect[2] = rect[0] + (INT32)width;
    rect[3] = rect[1] + (INT32)height;
    return TRUE;
}

static void W32kMinimizeWindow(UINT16 slot, BOOLEAN minimize)
{
    if (!W32kWindowExists(slot))
        return;
    W32K_WINDOW_STATE *window = &g_w32k[slot];
    if (window->Minimized == minimize)
        return;
    UINT64 flags = KiIrqSave();
    window->Minimized = minimize;
    W32kComposeRect(window->X - W32K_SHADOW_MARGIN, window->Y - W32K_SHADOW_MARGIN,
                    (INT32)window->W + 2 * W32K_SHADOW_MARGIN,
                    (INT32)window->H + 2 * W32K_SHADOW_MARGIN);
    if (minimize) {
        if (g_w32k_active == slot)
            W32kSetFocus(0);
    } else {
        W32kRaise(slot);
        W32kSetActive(slot);
    }
    KiIrqRestore(flags);
}

static BOOLEAN g_w32k_maximize_notified = FALSE;

static void W32kMaximizeWindow(UINT16 slot, BOOLEAN maximize)
{
    if (!W32kWindowExists(slot))
        return;
    W32K_WINDOW_STATE *window = &g_w32k[slot];
    if (window->Maximized == maximize)
        return;
    UINT64 flags = KiIrqSave();
    window->Maximized = maximize;
    if (maximize) {
        window->RestoreX = (UINT32)window->X;
        window->RestoreY = (UINT32)window->Y;
        window->RestoreW = window->W;
        window->RestoreH = window->H;
        W32kMoveWindow(KiUserWindowHandle(slot), 0, 0, GfxFramebuffer.Width,
                       GfxFramebuffer.Height);
    } else {
        W32kMoveWindow(KiUserWindowHandle(slot), (INT32)window->RestoreX,
                       (INT32)window->RestoreY, window->RestoreW,
                       window->RestoreH);
    }
    g_w32k_maximize_notified = FALSE;
    KiIrqRestore(flags);
}

/* Public wrapper used when the frame needs to repaint after a state change. */
void W32kInvalidateFrame(UINT16 slot)
{
    if (!W32kWindowExists(slot))
        return;
    W32kComposeRect(g_w32k[slot].X, g_w32k[slot].Y, (INT32)g_w32k[slot].W,
                    W32K_CAPTION_HEIGHT + 2);
}

/* --- input --------------------------------------------------------------- */

static UINT64 W32kMouseLParam(INT32 x, INT32 y)
{
    return (UINT64)(UINT32)(((UINT16)(y & 0xFFFF) << 16) |
                            (UINT16)(x & 0xFFFF));
}

static UINT64 W32kMouseWParam(UINT8 buttons)
{
    UINT64 wparam = 0;
    if (buttons & 1) wparam |= 0x0001; /* MK_LBUTTON */
    if (buttons & 2) wparam |= 0x0002; /* MK_RBUTTON */
    if (buttons & 4) wparam |= 0x0010; /* MK_MBUTTON */
    if (g_w32k_shift) wparam |= 0x0004;
    if (g_w32k_ctrl) wparam |= 0x0008;
    return wparam;
}

static void W32kSendWindowMessage(UINT16 slot, UINT32 message, UINT64 wparam,
                                  INT64 lparam)
{
    if (!W32kWindowExists(slot))
        return;
    UINT64 hwnd = KiUserWindowHandle(slot);
    if (!hwnd)
        return;
    KiUserEnqueueMessage(hwnd, message, wparam, lparam,
                         g_w32k_pointer_x, g_w32k_pointer_y);
}

/* Character input follows the keyboard, which reports translated characters:
 * delivering them here keeps the layout, the shift state and dead keys in one
 * place instead of re-deriving them from virtual keys. */
void W32kPostCharacter(UINT16 character)
{
    UINT16 target = W32kFocusWindow();
    if (!target)
        return;
    W32kSendWindowMessage(target, W32K_WM_CHAR, character, 1);
}

static void W32kHandleCaptionDrag(UINT16 slot)
{
    W32K_WINDOW_STATE *window = &g_w32k[slot];
    INT32 x = window->DragOriginX + (g_w32k_pointer_x - window->DragPointerX);
    INT32 y = window->DragOriginY + (g_w32k_pointer_y - window->DragPointerY);
    INT32 limit = (INT32)GfxFramebuffer.Height - W32K_CAPTION_HEIGHT - 8;
    if (y > limit) y = limit;
    W32kMoveWindow(KiUserWindowHandle(slot), x, y, window->W, window->H);
}

static void W32kInputMouseMoved(INT32 x, INT32 y, UINT8 buttons)
{
    UINT16 slot = g_w32k_drag ? g_w32k_drag :
                  g_w32k_capture ? g_w32k_capture : W32kWindowAt(x, y);
    BOOLEAN in_client = FALSE;
    UINT8 part = slot ? W32kHitTest(slot, x, y, &in_client) : W32K_PART_NONE;

    UINT16 hovered = W32kWindowAt(x, y);
    for (UINT16 other = 1; other < W32K_SLOTS; ++other) {
        if (g_w32k[other].TrackLeave && hovered != other) {
            g_w32k[other].TrackLeave = FALSE;
            W32kSendWindowMessage(other, 0x02A3, 0, 0); /* WM_MOUSELEAVE */
        }
    }
    for (UINT16 other = 1; other < W32K_SLOTS; ++other) {
        UINT8 hot = W32K_PART_NONE;
        if (other == hovered && g_w32k[other].Kind == W32K_KIND_TOPLEVEL) {
            BOOLEAN client = FALSE;
            hot = W32kHitTest(other, x, y, &client);
            if (hot < W32K_PART_MINIMIZE || hot > W32K_PART_CLOSE)
                hot = W32K_PART_NONE;
        }
        if (g_w32k[other].HotPart != hot) {
            g_w32k[other].HotPart = hot;
            W32kInvalidateFrame(other);
        }
    }

    if (slot && g_w32k[slot].Dragging) {
        W32kHandleCaptionDrag(slot);
        part = W32K_PART_CAPTION;
    }
    if (slot && (part == W32K_PART_CLIENT || g_w32k_capture == slot)) {
        INT32 cx, cy, cw, ch;
        W32kClientOrigin(slot, &cx, &cy, &cw, &ch);
        W32kSendWindowMessage(slot, W32K_WM_MOUSEMOVE, W32kMouseWParam(buttons),
                              W32kMouseLParam(x - cx, y - cy));
    }
}

static void W32kInputMouseLocked(INT32 x, INT32 y, UINT8 buttons)
{
    BOOLEAN pressed = (buttons & 1) && !(g_w32k_pointer_buttons & 1);
    BOOLEAN released = !(buttons & 1) && (g_w32k_pointer_buttons & 1);
    if (!g_w32k_pointer_valid) {
        g_w32k_pointer_valid = TRUE;
        g_w32k_pointer_x = x;
        g_w32k_pointer_y = y;
        g_w32k_pointer_buttons = buttons;
        return;
    }

    g_w32k_pointer_x = x;
    g_w32k_pointer_y = y;

    UINT16 target = g_w32k_drag ? g_w32k_drag :
                    g_w32k_capture ? g_w32k_capture : W32kWindowAt(x, y);
    BOOLEAN in_client = FALSE;
    UINT8 part = target ? W32kHitTest(target, x, y, &in_client)
                        : W32K_PART_NONE;

    if (pressed && target) {
        if (g_w32k[target].Kind == W32K_KIND_TOPLEVEL &&
            part != W32K_PART_NONE && part != W32K_PART_CLIENT) {
            W32kRaise(target);
            if (g_w32k_active != target) W32kSetFocus(KiUserWindowHandle(target));
            if(part==W32K_PART_MENU){
                W32kSendWindowMessage(target,W32K_WM_SYSCOMMAND,0xf090,W32kMouseLParam(x,y));
            } else if (part == W32K_PART_CAPTION) {
                W32K_WINDOW_STATE *window = &g_w32k[target];
                window->Dragging = 1;
                g_w32k_drag = target;
                window->DragPointerX = x;
                window->DragPointerY = y;
                window->DragOriginX = window->X;
                window->DragOriginY = window->Y;
            } else if (part == W32K_PART_MINIMIZE) {
                W32kSendWindowMessage(target, W32K_WM_SYSCOMMAND,
                                      W32K_SC_MINIMIZE, 0);
                W32kMinimizeWindow(target, TRUE);
            } else if (part == W32K_PART_MAXIMIZE) {
                BOOLEAN maximize = !g_w32k[target].Maximized;
                W32kSendWindowMessage(target, W32K_WM_SYSCOMMAND,
                                      maximize ? W32K_SC_MAXIMIZE
                                               : W32K_SC_RESTORE,
                                      0);
                W32kMaximizeWindow(target, maximize);
            } else if (part == W32K_PART_CLOSE) {
                W32kSendWindowMessage(target, W32K_WM_SYSCOMMAND, W32K_SC_CLOSE,
                                      0);
            }
        } else {
            /* Child HWNDs share the global z-order list, but clicking one must
             * only raise its top-level owner. Raising a group-box child above
             * its siblings paints its retained frame over every control it
             * contains; ordinary child activation must preserve sibling order. */
            UINT16 root = W32kRootSlot(target);
            if (root && root != target) W32kRaise(root);
            else W32kRaise(target);
            W32kSetFocus(KiUserWindowHandle(target));
        }
    }

    if (released) {
        for (UINT16 slot = 1; slot < W32K_SLOTS; ++slot)
            g_w32k[slot].Dragging = 0;
        g_w32k_drag = 0;
    }

    g_w32k_pointer_buttons = buttons;

    if (target && (pressed || released) &&
        (part == W32K_PART_CLIENT || g_w32k_capture == target)) {
        INT32 cx, cy, cw, ch;
        W32kClientOrigin(target, &cx, &cy, &cw, &ch);
        W32kSendWindowMessage(target,
                              pressed ? W32K_WM_LBUTTONDOWN
                                      : W32K_WM_LBUTTONUP,
                              W32kMouseWParam(buttons),
                              W32kMouseLParam(x - cx, y - cy));
    }

    W32kInputMouseMoved(x, y, buttons);
}

void W32kInputMouse(INT32 x, INT32 y, UINT8 buttons)
{
    UINT64 flags = KiIrqSave();
    W32kInputMouseLocked(x, y, buttons);
    KiIrqRestore(flags);
}

/* Scan code to virtual key: the Set 1 make codes Windows uses. */
static UINT8 W32kVirtualKey(UINT16 scan)
{
    switch (scan) {
    case 0x01: return 0x1B;
    case 0x0C: return 0xBD; case 0x0D: return 0xBB;
    case 0x1A: return 0xDB; case 0x1B: return 0xDD;
    case 0x27: return 0xBA; case 0x28: return 0xDE;
    case 0x29: return 0xC0; case 0x2B: return 0xDC;
    case 0x33: return 0xBC; case 0x34: return 0xBE;
    case 0x35: return 0xBF;
    case 0x37: return 0x6A; case 0x4A: return 0x6D;
    case 0x4E: return 0x6B;
    case 0x3A: return 0x14;
    case 0x0E: return 0x08;
    case 0x0F: return 0x09;
    case 0x1C: return 0x0D;
    case 0x1D: return 0x11;
    case 0x2A: return 0x10;
    case 0x36: return 0x10;
    case 0x38: return 0x12;
    case 0x39: return 0x20;
    case 0x3B: return 0x70; case 0x3C: return 0x71;
    case 0x3D: return 0x72; case 0x3E: return 0x73;
    case 0x3F: return 0x74; case 0x40: return 0x75;
    case 0x41: return 0x76; case 0x42: return 0x77;
    case 0x43: return 0x78; case 0x44: return 0x79;
    case 0x57: return 0x7A; case 0x58: return 0x7B;
    case 0x47: return 0x24; case 0x48: return 0x26;
    case 0x49: return 0x21; case 0x4B: return 0x25;
    case 0x4D: return 0x27; case 0x4F: return 0x23;
    case 0x50: return 0x28; case 0x51: return 0x22;
    case 0x52: return 0x2D; case 0x53: return 0x2E;
    case 0x5B: return 0x5B; case 0x5C: return 0x5C;
    default: break;
    }
    if (scan >= 0x02 && scan <= 0x0B) {
        static const UINT8 digits[10] = {'1','2','3','4','5','6','7','8','9','0'};
        return digits[scan - 0x02];
    }
    if (scan >= 0x10 && scan <= 0x19) {
        static const UINT8 row[10] = {'Q','W','E','R','T','Y','U','I','O','P'};
        return row[scan - 0x10];
    }
    if (scan >= 0x1E && scan <= 0x26) {
        static const UINT8 row[9] = {'A','S','D','F','G','H','J','K','L'};
        return row[scan - 0x1E];
    }
    if (scan >= 0x2C && scan <= 0x32) {
        static const UINT8 row[7] = {'Z','X','C','V','B','N','M'};
        return row[scan - 0x2C];
    }
    return 0;
}

void W32kInputKey(UINT16 event, UINT16 character)
{
    BOOLEAN release = (event & 0x100) != 0;
    UINT16 scan = event & 0x7F;
    UINT8 vk = W32kVirtualKey(scan);
    if ((event & 0x200) && scan == 0x35) vk = 0x6F;
    if (vk == 0x10) g_w32k_shift = !release;
    else if (vk == 0x11) g_w32k_ctrl = !release;
    else if (vk == 0x12) g_w32k_alt = !release;
    if (!vk)
        return;
    UINT16 target = W32kFocusWindow();
    if (!target)
        return;
    UINT64 lparam = 1 | ((UINT64)(scan & 0xFF) << 16) |
                    ((event & 0x200) ? 1ULL << 24 : 0) |
                    (release ? 0xC0000000ULL : 0);
    /* Carry layout translation privately with the key. TranslateMessage
     * posts it only if the application has not consumed an accelerator. */
    BOOLEAN system = g_w32k_alt || vk == 0x12;
    if (g_w32k_alt) lparam |= 1ULL << 29;
    KiUserEnqueueMessagePrivate(KiUserWindowHandle(target),
        system ? (release ? 0x105 : 0x104) :
                 (release ? W32K_WM_KEYUP : W32K_WM_KEYDOWN),
        vk, (INT64)lparam, g_w32k_pointer_x, g_w32k_pointer_y,
        !release && character ? 0x10000u | character : 0, FALSE);
}

/* Keyboard ownership: once a window has focus the window manager turns raw key
 * transitions into WM_KEYDOWN/WM_KEYUP and consumes them. */
BOOLEAN W32kKeyboardOwner(void)
{
    return W32kFocusWindow() != 0;
}

/* --- services ------------------------------------------------------------ */

static UINT64 NtUserSetCapture(UINT64 *a)
{
    UINT64 previous = KiUserWindowHandle(g_w32k_capture);
    g_w32k_capture = 0;
    if (a[0] && KiUserWindowValid(a[0], TRUE))
        g_w32k_capture = (UINT16)(a[0] & 0xFFFF);
    if ((UINT16)previous && (UINT16)previous != g_w32k_capture)
        W32kSendWindowMessage((UINT16)previous, 0x0215, 0, (INT64)KiUserWindowHandle(g_w32k_capture));
    return previous;
}

static UINT64 NtUserReleaseCapture(UINT64 *a)
{
    (void)a;
    UINT64 previous = KiUserWindowHandle(g_w32k_capture);
    g_w32k_capture = 0;
    if ((UINT16)previous) W32kSendWindowMessage((UINT16)previous, 0x0215, 0, 0);
    return previous;
}

static UINT64 NtUserSetFocus(UINT64 *a)
{
    if (a[0] && !KiUserWindowValid(a[0], TRUE))
        return 0;
    UINT64 previous = KiUserWindowHandle(W32kFocusWindow());
    W32kSetFocus(a[0]);
    return previous;
}

static UINT64 NtUserGetFocus(UINT64 *a)
{
    (void)a;
    PKTHREAD caller = KeGetCurrentThread();
    UINT16 focus = W32kFocusWindow();
    return caller && focus && g_window_owner[focus] == caller->ThreadId ? KiUserWindowHandle(focus) : 0;
}

static UINT64 NtUserWindowFromPoint(UINT64 *a)
{
    /* (POINT pt) by value on x64: x in a[0], y in a[1]. */
    UINT16 slot = W32kWindowAt((INT32)a[0], (INT32)a[1]);
    return KiUserWindowHandle(slot);
}

static void W32kSetStyle(UINT16 slot, UINT32 style)
{
    if(!W32kWindowExists(slot))return;
    BOOLEAN was_visible=g_w32k[slot].Visible;
    g_w32k[slot].Style=style;
    if(was_visible!=((style&WS_VISIBLE_)!=0))W32kShowWindow(KiUserWindowHandle(slot),(style&WS_VISIBLE_)?5:0);
}

static UINT64 NtUserGetWindowRect(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE))
        return 0;
    return W32kGetRect(a[0], (INT32 *)a[1], FALSE) ? 1 : 0;
}

/* Private client contract: source HWND, target HWND, POINT array, count. */
static UINT64 NtUserMapWindowPoints(UINT64 *a)
{
    if ((a[0] && !KiUserWindowValid(a[0], TRUE)) ||
        (a[1] && !KiUserWindowValid(a[1], TRUE)) || a[3]>65536 ||
        !MmProbeForWrite(a[2], a[3]*8)) return 0;
    INT32 sx=0,sy=0,tx=0,ty=0,w,h;
    if(a[0])W32kClientOrigin((UINT16)a[0],&sx,&sy,&w,&h);
    if(a[1])W32kClientOrigin((UINT16)a[1],&tx,&ty,&w,&h);
    INT32 dx=sx-tx,dy=sy-ty;INT32 *p=(INT32*)a[2];
    for(UINT64 i=0;i<a[3];i++){p[i*2]+=dx;p[i*2+1]+=dy;}
    return (UINT32)((UINT16)dx|((UINT32)(UINT16)dy<<16));
}

static UINT64 SvNtUserIsIconic(UINT64 *a)
{
    return KiUserWindowValid(a[0], TRUE) && g_w32k[(UINT16)a[0]].Minimized;
}

static UINT64 SvNtUserGetCursorPos(UINT64 *a)
{
    INT32 *point = (INT32 *)a[0];
    if (!MmProbeForWrite(a[0], 2 * sizeof(*point))) return 0;
    UINT64 flags = KiIrqSave();
    INT32 x = g_w32k_pointer_x, y = g_w32k_pointer_y;
    KiIrqRestore(flags);
    point[0] = x;
    point[1] = y;
    return 1;
}

static UINT64 SvNtUserChildWindowFromPoint(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE)) return 0;
    UINT16 parent = (UINT16)a[0];
    INT32 x = (INT32)a[1], y = (INT32)a[2];
    UINT64 flags = KiIrqSave();
    INT32 sx, sy, width, height;
    W32kClientOrigin(parent, &sx, &sy, &width, &height);
    UINT64 result = 0;
    if (x >= 0 && y >= 0 && x < width && y < height) {
        result = a[0];
        for (UINT16 slot = g_w32k_top; slot; slot = g_w32k[slot].ZPrev) {
            const W32K_WINDOW_STATE *child = &g_w32k[slot];
            if (child->Parent != parent || child->Kind != W32K_KIND_CHILD)
                continue;
            INT64 dx = (INT64)x - child->X, dy = (INT64)y - child->Y;
            if (dx >= 0 && dy >= 0 && dx < child->W && dy < child->H) {
                result = KiUserWindowHandle(slot);
                break;
            }
        }
    }
    KiIrqRestore(flags);
    return result;
}

static UINT64 NtUserGetClientRect(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE))
        return 0;
    return W32kGetRect(a[0], (INT32 *)a[1], TRUE) ? 1 : 0;
}

static UINT64 NtUserSetWindowText(UINT64 *a)
{
    if (!KiUserWindowValid(a[0], TRUE))
        return 0;
    const WCHAR *text = (const WCHAR *)a[1];
    WCHAR local[W32K_TITLE_CHARS];
    UINT32 i = 0;
    if (text && MmProbeForRead((UINT64)text, sizeof(WCHAR))) {
        for (; i < W32K_TITLE_CHARS - 1; ++i) {
            if (!MmProbeForRead((UINT64)(text + i), sizeof(WCHAR)))
                break;
            local[i] = text[i];
            if (!local[i])
                break;
        }
    }
    local[i] = 0;
    W32kSetWindowText(a[0], local);
    return 1;
}

static UINT64 NtUserGetWindowText(UINT64 *a)
{
    UINT16 slot = (UINT16)(a[0] & 0xFFFF);
    if (!KiUserWindowValid(a[0], TRUE) || !W32kWindowExists(slot))
        return 0;
    WCHAR *out = (WCHAR *)a[1];
    UINT32 capacity = (UINT32)a[2];
    if (!out || !capacity || !MmProbeForWrite((UINT64)out,
                                              (UINT64)capacity * sizeof(WCHAR)))
        return 0;
    UINT32 i = 0;
    for (; i + 1 < capacity && g_w32k[slot].Title[i]; ++i)
        out[i] = g_w32k[slot].Title[i];
    out[i] = 0;
    return i;
}

/* Public TRACKMOUSEEVENT layout on x64. Hover timers remain unsupported;
 * leave tracking belongs to each HWND and is armed again after delivery. */
static UINT64 NtUserTrackMouseEvent(UINT64 *a)
{
    UINT8 *event = (UINT8 *)a[0];
    if (!event || !MmProbeForRead(a[0], 24) || *(UINT32 *)event != 24) return 0;
    UINT32 flags = *(UINT32 *)(event + 4);
    UINT64 hwnd = *(UINT64 *)(event + 8);
    if (!KiUserWindowValid(hwnd, TRUE)) return 0;
    UINT16 slot = (UINT16)hwnd;
    if (flags & 0x40000000u) {
        if (!MmProbeForWrite(a[0], 24)) return 0;
        *(UINT32 *)(event + 4) = g_w32k[slot].TrackLeave ? 2 : 0;
        return 1;
    }
    if ((flags & ~(0x80000000u | 2u)) || !(flags & 2)) return 0;
    g_w32k[slot].TrackLeave = !(flags & 0x80000000u);
    return 1;
}


void W32kSetMenuBar(UINT16 slot,const WCHAR *labels,UINT32 count,BOOLEAN present)
{
    if(!W32kWindowExists(slot)||g_w32k[slot].Kind!=W32K_KIND_TOPLEVEL)return;
    UINT64 flags=KiIrqSave();
    W32K_WINDOW_STATE *w=&g_w32k[slot];w->MenuPresent=present;
    w->MenuCount=count>16?16:count;
    memset(w->MenuLabels,0,sizeof(w->MenuLabels));
    if(labels)for(UINT32 i=0;i<w->MenuCount;i++){
        memcpy(w->MenuLabels[i],labels+i*64,126);w->MenuLabels[i][63]=0;
    }
    W32kScreenCompose();KiIrqRestore(flags);
}
static UINT64 NtUserMenuBar(UINT64 *a)
{
    if(!KiUserWindowValid(a[0],TRUE))return 0;
    UINT16 slot=(UINT16)a[0];
    if(a[1]==0)return g_window_menus[slot];
    if(a[1]!=1&&a[1]!=2)return 0;
    if(a[4]>16||(a[4]&&!MmProbeForRead(a[3],a[4]*128)))return 0;
    if(a[1]==1)g_window_menus[slot]=a[2];
    W32kSetMenuBar(slot,(const WCHAR*)a[3],(UINT32)a[4],g_window_menus[slot]!=0);
    return 1;
}

/* GW_* traversal follows sibling Z-order, not allocation-slot order. */
static UINT64 NtUserWindowRelation(UINT64 *a)
{
    if(!KiUserWindowValid(a[0],TRUE))return 0;
    UINT16 slot=(UINT16)a[0],parent=g_w32k[slot].Parent;
    UINT32 command=(UINT32)a[1];
    if(command==4){
        if(g_w32k[slot].Style&WS_CHILD)return 0;
        UINT64 owner=*(UINT64*)(PROCESS_USER_OBJECT_ARENA_VA+(UINT64)slot*0x200+WND_PARENT_OFF);
        return owner&&KiUserWindowValid(owner,FALSE)?owner:0;
    }
    UINT16 candidate=0;
    if(command==5){parent=slot;candidate=g_w32k_top;}
    else if(command==0||command==1)candidate=g_w32k_top;
    else if(command==2)candidate=g_w32k[slot].ZPrev;
    else if(command==3)candidate=g_w32k[slot].ZNext;
    else return 0;
    UINT16 result=0;
    for(UINT32 depth=0;candidate&&depth<W32K_SLOTS;depth++){
        if(g_w32k[candidate].Parent==parent){result=candidate;if(command!=1)break;}
        candidate=command==3?g_w32k[candidate].ZNext:g_w32k[candidate].ZPrev;
    }
    return result?KiUserWindowHandle(result):0;
}

void W32kScreenSetStatic(UINT16 slot,BOOLEAN is_static){if(W32kWindowExists(slot))g_w32k[slot].IsStatic=is_static;}
