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
        if(parent->Kind==W32K_KIND_TOPLEVEL&&parent->MenuPresent)top+=W32K_MENU_HEIGHT;
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
    /* CreateWindow places a new child below existing siblings. Raising every
     * new child hides overlapping controls that the application created first. */
    UINT16 lowest=0;
    for(UINT16 w=g_w32k_top;w;w=g_w32k[w].ZPrev)
        if(w!=slot&&g_w32k[w].Parent==parent)lowest=w;
    if(lowest){
        W32kUnlink(slot);
        UINT16 below=g_w32k[lowest].ZPrev;
        g_w32k[slot].ZPrev=below;g_w32k[slot].ZNext=lowest;
        g_w32k[lowest].ZPrev=slot;
        if(below)g_w32k[below].ZNext=slot;
    }
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
            /* Default STATIC hit testing passes through unless SS_NOTIFY is
             * set. Background labels must not intercept sibling controls. */
            if(w->IsStatic&&!(w->Style&0x100))continue;
            INT32 sx, sy; W32kScreenPosition(child, &sx, &sy);
            if (x >= sx && y >= sy && x < sx + (INT32)w->W && y < sy + (INT32)w->H) break;
        }
        if (!child) return parent;
        parent = child;
    }
    return parent;
}

static BOOLEAN W32kChildrenUseClipping(UINT16 parent)
{
    for (UINT16 slot = g_w32k_top; slot; slot = g_w32k[slot].ZPrev) {
        const W32K_WINDOW_STATE *window = &g_w32k[slot];
        if (window->Parent == parent && window->Visible &&
            (window->Style & 0x04000000u)) /* WS_CLIPSIBLINGS */
            return TRUE;
    }
    return FALSE;
}

static void W32kPaintChildren(UINT16 parent, INT32 x, INT32 y, INT32 w, INT32 h,
                              unsigned unused)
{
    (void)unused;
    /* Iterative DFS keeps the kernel stack bounded even for 127 descendants. */
    struct FRAME {
        UINT16 parent, next;
        BOOLEAN clipped_siblings;
        INT32 x, y, w, h;
    } frames[W32K_SLOTS];
    unsigned depth = 0;
    UINT16 bottom = g_w32k_top;
    while (bottom && g_w32k[bottom].ZPrev) bottom = g_w32k[bottom].ZPrev;
    /* Classic child windows paint in Z order. Without WS_CLIPSIBLINGS,
     * a lower child can paint over a sibling that was painted earlier. */
    BOOLEAN clipped = W32kChildrenUseClipping(parent);
    frames[0] = (struct FRAME){parent, clipped ? bottom : g_w32k_top,
                              clipped, x, y, w, h};
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
            child = f->clipped_siblings ? g_w32k[child].ZNext : g_w32k[child].ZPrev;
        if (!child || f->w <= 0 || f->h <= 0 || depth + 1 >= W32K_SLOTS) {
            if (!depth) return;
            --depth; continue;
        }
        f->next = f->clipped_siblings ? g_w32k[child].ZNext : g_w32k[child].ZPrev;
        W32kCopyWindowClient(child, g_w32k_composite, g_w32k_composite_w,
                             f->x, f->y, f->w, f->h);
        clipped = W32kChildrenUseClipping(child);
        frames[depth+1] = (struct FRAME){child, clipped ? bottom : g_w32k_top,
                                       clipped, f->x, f->y, f->w, f->h};
        ++depth;
    }
}
