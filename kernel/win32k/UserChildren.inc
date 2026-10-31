/* Parent-relative USER geometry, descendant clipping and child hit testing.
 * A child retains its own client raster but is composited within its parent's
 * client area. Moving an ancestor changes coordinates, never child pixels. */

UINT16 W32kWindowParent(UINT16 slot)
{
    return W32kWindowExists(slot) ? g_w32k[slot].Parent : 0;
}

UINT16 W32kRootSlot(UINT16 slot)
{
    for (unsigned depth = 0; depth < W32K_SLOTS && W32kWindowExists(slot); ++depth) {
        UINT16 parent = g_w32k[slot].Parent;
        if (!parent) return slot;
        slot = parent;
    }
    return 0;
}

static void W32kScreenPosition(UINT16 slot, INT32 *x, INT32 *y)
{
    *x = g_w32k[slot].X; *y = g_w32k[slot].Y;
    for (unsigned depth = 0; depth < W32K_SLOTS; ++depth) {
        slot = g_w32k[slot].Parent;
        if (!W32kWindowExists(slot)) return;
        const W32K_WINDOW_STATE *parent = &g_w32k[slot];
        int border = parent->Kind == W32K_KIND_TOPLEVEL ? W32K_BORDER : 0;
        int top = border;
        if (parent->Kind == W32K_KIND_TOPLEVEL && (parent->Style & WS_CAPTION))
            top = W32K_CAPTION_HEIGHT;
        *x += parent->X + border;
        *y += parent->Y + top;
    }
}

void W32kScreenSetParent(UINT16 slot, UINT16 parent)
{
    if (!W32kWindowExists(slot) || !W32kWindowExists(parent) || slot == parent ||
        g_w32k[slot].Kind != W32K_KIND_CHILD) return;
    for (UINT16 ancestor = parent; ancestor; ancestor = W32kWindowParent(ancestor))
        if (ancestor == slot) return;
    UINT64 flags = KiIrqSave();
    g_w32k[slot].Parent = parent;
    KiIrqRestore(flags);
}

static UINT16 W32kChildAt(UINT16 parent, INT32 x, INT32 y, unsigned depth)
{
    for (; depth < W32K_SLOTS; ++depth) {
        INT32 cx, cy, cw, ch;
        W32kClientOrigin(parent, &cx, &cy, &cw, &ch);
        if (x < cx || y < cy || x >= cx+cw || y >= cy+ch) return parent;
        UINT16 child;
        for (child = g_w32k_top; child; child = g_w32k[child].ZPrev) {
            const W32K_WINDOW_STATE *w = &g_w32k[child];
            if (w->Parent != parent || !w->Visible || w->Minimized || (w->Style & 0x08000000u)) continue;
            INT32 sx, sy; W32kScreenPosition(child, &sx, &sy);
            if (x >= sx && y >= sy && x < sx + (INT32)w->W && y < sy + (INT32)w->H) break;
        }
        if (!child) return parent;
        parent = child;
    }
    return parent;
}

static void W32kPaintChildren(UINT16 parent, INT32 x, INT32 y, INT32 w, INT32 h,
                              unsigned unused)
{
    (void)unused;
    /* Iterative DFS keeps the kernel stack bounded even for 127 descendants. */
    struct FRAME { UINT16 parent, next; INT32 x, y, w, h; } frames[W32K_SLOTS];
    unsigned depth = 0;
    UINT16 bottom = g_w32k_top;
    while (bottom && g_w32k[bottom].ZPrev) bottom = g_w32k[bottom].ZPrev;
    frames[0] = (struct FRAME){parent, bottom, x, y, w, h};
    for (;;) {
        struct FRAME *f = &frames[depth];
        INT32 cx, cy, cw, ch;
        W32kClientOrigin(f->parent, &cx, &cy, &cw, &ch);
        INT32 right = f->x + f->w < cx + cw ? f->x + f->w : cx + cw;
        INT32 below = f->y + f->h < cy + ch ? f->y + f->h : cy + ch;
        if (f->x < cx) f->x = cx;
        if (f->y < cy) f->y = cy;
        f->w = right - f->x; f->h = below - f->y;
        UINT16 child = f->next;
        while (child && (g_w32k[child].Parent != f->parent ||
               !g_w32k[child].Visible || g_w32k[child].Minimized))
            child = g_w32k[child].ZNext;
        if (!child || f->w <= 0 || f->h <= 0 || depth + 1 >= W32K_SLOTS) {
            if (!depth) return;
            --depth; continue;
        }
        f->next = g_w32k[child].ZNext;
        W32kCopyWindowClient(child, g_w32k_composite, g_w32k_composite_w,
                             f->x, f->y, f->w, f->h);
        frames[depth+1] = (struct FRAME){child, bottom, f->x, f->y, f->w, f->h};
        ++depth;
    }
}
