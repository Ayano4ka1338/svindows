#include "Internal.h"

__declspec(dllimport) LONG_PTR GetWindowLongPtrW(HWND, int);
__declspec(dllimport) LONG_PTR SetWindowLongPtrW(HWND, int, LONG_PTR);
__declspec(dllimport) int CompareStringOrdinal(const WCHAR *, int, const WCHAR *, int, BOOL);

typedef struct TREE_NODE TREE_NODE;
typedef struct {
    UINT Mask;
    TREE_NODE *Handle;
    UINT State, StateMask;
    WCHAR *Text;
    int TextCapacity, Image, SelectedImage, Children;
    LPARAM Param;
} TREE_ITEM;
typedef struct { TREE_NODE *Parent, *After; TREE_ITEM Item; } TREE_INSERT;
typedef struct {
    NMHDR Header;
    UINT Action;
    TREE_ITEM Old, New;
    POINT Drag;
} TREE_NOTIFY;
_Static_assert(sizeof(TREE_ITEM) == 56, "TVITEMW x64");
_Static_assert(sizeof(TREE_NOTIFY) == 152, "NMTREEVIEWW x64");

struct TREE_NODE {
    TREE_NODE *AllNext, *Parent, *Next, *Child;
    WCHAR *Text;
    UINT State;
    int Image, SelectedImage, Children;
    LPARAM Param;
    BOOL Dead, Deleting, TextCallback;
};

typedef struct {
    HWND Window, Parent;
    UINT_PTR Id;
    TREE_NODE *All, *Roots, *Selected, *Drop;
    HANDLE Images;
    UINT References, Count;
    int Indent, RowHeight, FirstRow;
    BOOL Dead;
} TREE_STATE;

static TREE_NODE *tree_node(TREE_STATE *tree, const void *handle)
{
    for (TREE_NODE *node = tree->All; node; node = node->AllNext)
        if (node == handle) return node->Dead ? 0 : node;
    return 0;
}

static TREE_ITEM tree_item(TREE_NODE *node)
{
    TREE_ITEM item = {0};
    if (node) {
        item.Mask = 0x7f;
        item.Handle = node;
        item.State = node->State;
        item.StateMask = (UINT)-1;
        item.Text = node->Text;
        item.Image = node->Image;
        item.SelectedImage = node->SelectedImage;
        item.Children = node->Children;
        item.Param = node->Param;
    }
    return item;
}

static LRESULT tree_notify(TREE_STATE *tree, UINT code, UINT action,
                            TREE_NODE *old, TREE_NODE *node)
{
    TREE_NOTIFY notify = {{tree->Window, tree->Id, code}, action,
                           tree_item(old), tree_item(node), {0}};
    return tree->Dead ? 0 : SendMessageW(tree->Parent, WM_NOTIFY, tree->Id, (LPARAM)&notify);
}

static WCHAR *tree_copy(const WCHAR *text)
{
    ULONG_PTR size = 1;
    while (text && text[size - 1]) size++;
    WCHAR *copy = SvComctlAlloc(size * sizeof(*copy));
    if (!copy) return 0;
    for (ULONG_PTR i = 0; i + 1 < size; i++) copy[i] = text[i];
    copy[size - 1] = 0;
    return copy;
}

static BOOL tree_set_item(TREE_NODE *node, const TREE_ITEM *item)
{
    WCHAR *text = 0;
    BOOL callback = (UINT_PTR)item->Text == (UINT_PTR)-1;
    if (item->Mask & 1) {
        text = tree_copy(callback ? L"" : item->Text);
        if (!text) return FALSE;
        SvComctlFree(node->Text);
        node->Text = text;
        node->TextCallback = callback;
    }
    if (item->Mask & 2) node->Image = item->Image;
    if (item->Mask & 0x20) node->SelectedImage = item->SelectedImage;
    if (item->Mask & 4) node->Param = item->Param;
    if (item->Mask & 0x40) node->Children = item->Children;
    if (item->Mask & 8)
        node->State = (node->State & ~item->StateMask) | (item->State & item->StateMask);
    return TRUE;
}

static TREE_NODE *tree_next_visible(TREE_NODE *node)
{
    if (node->Child && (node->State & 0x20)) return node->Child;
    while (node && !node->Next) node = node->Parent;
    return node ? node->Next : 0;
}

static int tree_row(TREE_STATE *tree, TREE_NODE *target)
{
    int row = 0;
    for (TREE_NODE *node = tree->Roots; node; node = tree_next_visible(node), row++)
        if (node == target) return row;
    return -1;
}

static TREE_NODE *tree_row_node(TREE_STATE *tree, int row)
{
    if (row < 0) return 0;
    TREE_NODE *node = tree->Roots;
    while (node && row-- > 0) node = tree_next_visible(node);
    return node;
}

static void tree_ensure_visible(TREE_STATE *tree, TREE_NODE *node)
{
    int row = tree_row(tree, node);
    if (row < 0) return;
    RECT rect;
    GetClientRect(tree->Window, &rect);
    int rows = rect.bottom / tree->RowHeight;
    if (rows < 1) rows = 1;
    if (row < tree->FirstRow) tree->FirstRow = row;
    if (row >= tree->FirstRow + rows) tree->FirstRow = row - rows + 1;
    InvalidateRect(tree->Window, 0, FALSE);
}

static BOOL tree_select(TREE_STATE *tree, TREE_NODE *node, UINT action)
{
    if (tree->Selected == node) return TRUE;
    TREE_NODE *previous = tree->Selected;
    if (tree_notify(tree, (UINT)-450, action, previous, node) ||
        tree->Dead || (node && node->Dead))
        return FALSE;
    if (previous) previous->State &= ~2u;
    tree->Selected = node;
    if (node) node->State |= 2;
    tree_notify(tree, (UINT)-451, action, previous, node);
    if (!tree->Dead) tree_ensure_visible(tree, node);
    return TRUE;
}

static BOOL tree_expand(TREE_STATE *tree, TREE_NODE *node, UINT action)
{
    if (!node) return FALSE;
    BOOL expand = (action & 3) == 3 ? !(node->State & 0x20) : (action & 2) != 0;
    if (expand == ((node->State & 0x20) != 0)) return TRUE;
    UINT change = expand ? 2 : 1;
    if (tree_notify(tree, (UINT)-454, change, 0, node) || tree->Dead || node->Dead)
        return FALSE;
    if (expand) node->State |= 0x60;
    else node->State &= ~0x20u;
    tree_notify(tree, (UINT)-455, change, 0, node);
    if (!tree->Dead) InvalidateRect(tree->Window, 0, FALSE);
    return TRUE;
}

static void tree_delete(TREE_STATE *tree, TREE_NODE *node)
{
    if (!node || node->Dead || node->Deleting) return;
    node->Deleting = TRUE;
    if (tree->Selected == node) tree->Selected = 0;
    if (tree->Drop == node) tree->Drop = 0;
    TREE_NODE **link = node->Parent ? &node->Parent->Child : &tree->Roots;
    while (*link && *link != node) link = &(*link)->Next;
    if (*link) *link = node->Next;
    while (node->Child) tree_delete(tree, node->Child);
    node->Dead = TRUE;
    tree->Count--;
    tree_notify(tree, (UINT)-458, 0, node, 0);
    /* Keep the item handle as a tombstone until the control is destroyed,
     * including while a parent's notification callback uses it. */
}

static TREE_NODE *tree_insert(TREE_STATE *tree, const TREE_INSERT *insert)
{
    TREE_NODE *parent = 0;
    if (insert->Parent && (DWORD)(UINT_PTR)insert->Parent != 0xffff0000u) {
        parent = tree_node(tree, insert->Parent);
        if (!parent || parent->Deleting) return 0;
    }
    TREE_NODE **head = parent ? &parent->Child : &tree->Roots;
    TREE_NODE **link = head;
    DWORD after = (DWORD)(UINT_PTR)insert->After;
    if (after != 0xffff0001u) {
        if (after == 0xffff0003u) {
            while (*link && CompareStringOrdinal((*link)->Text, -1,
                    (insert->Item.Mask & 1) && (UINT_PTR)insert->Item.Text != (UINT_PTR)-1
                        ? insert->Item.Text : L"", -1, TRUE) <= 2)
                link = &(*link)->Next;
        } else if (after == 0xffff0002u || !insert->After) {
            while (*link) link = &(*link)->Next;
        } else {
            TREE_NODE *previous = tree_node(tree, insert->After);
            if (!previous || previous->Parent != parent) return 0;
            link = &previous->Next;
        }
    }
    TREE_NODE *node = SvComctlAlloc(sizeof(*node));
    if (!node) return 0;
    node->Text = tree_copy(L"");
    if (!node->Text || !tree_set_item(node, &insert->Item)) {
        SvComctlFree(node->Text);
        SvComctlFree(node);
        return 0;
    }
    node->Parent = parent;
    node->Next = *link;
    *link = node;
    node->AllNext = tree->All;
    tree->All = node;
    tree->Count++;
    InvalidateRect(tree->Window, 0, FALSE);
    return node;
}

static void tree_draw(TREE_STATE *tree)
{
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(tree->Window, &paint);
    if (!dc) return;
    RECT bounds;
    GetClientRect(tree->Window, &bounds);
    SvComctlFill(dc, &bounds, RGB(255,255,255));
    SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    int y = 0;
    for (TREE_NODE *node = tree_row_node(tree, tree->FirstRow);
         node && y < bounds.bottom; node = tree_next_visible(node), y += tree->RowHeight) {
        int depth = 0;
        for (TREE_NODE *parent = node->Parent; parent; parent = parent->Parent) depth++;
        int x = 4 + depth * tree->Indent;
        RECT row = {x + 18, y, bounds.right, y + tree->RowHeight};
        if (node->State & 2) SvComctlFill(dc, &row, RGB(0,120,215));
        SetTextColor(dc, node->State & 2 ? RGB(255,255,255) : RGB(0,0,0));
        DrawTextW(dc, node->Text, -1, &row, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        if (node->Child || node->Children) {
            RECT box = {x, y + 3, x + 15, y + tree->RowHeight - 3};
            SetTextColor(dc, RGB(0,0,0));
            DrawTextW(dc, node->State & 0x20 ? L"-" : L"+", 1, &box,
                       DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
    }
    EndPaint(tree->Window, &paint);
}

static LRESULT tree_message(TREE_STATE *tree, UINT message, WPARAM wp, LPARAM lp)
{
    TREE_NODE *node;
    switch (message) {
    case WM_PAINT: tree_draw(tree); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(tree->Window, 0, FALSE); return 0;
    case 0x1132: return lp ? (LRESULT)tree_insert(tree, (const TREE_INSERT *)lp) : 0;
    case 0x1101:
        if ((DWORD)(UINT_PTR)lp == 0xffff0000u || !lp) {
            while (tree->Roots) tree_delete(tree, tree->Roots);
        } else {
            node = tree_node(tree, (void *)lp);
            if (!node) return FALSE;
            tree_delete(tree, node);
        }
        if (!tree->Dead) InvalidateRect(tree->Window, 0, FALSE);
        return TRUE;
    case 0x1102: return tree_expand(tree, tree_node(tree, (void *)lp), (UINT)wp);
    case 0x1105: return tree->Count;
    case 0x1106: return tree->Indent;
    case 0x1107: tree->Indent = (int)wp > 0 ? (int)wp : 16; return 0;
    case 0x1108: return (LRESULT)tree->Images;
    case 0x1109: { HANDLE previous = tree->Images; tree->Images = (HANDLE)lp; return (LRESULT)previous; }
    case 0x110a:
        node = tree_node(tree, (void *)lp);
        switch (wp) {
        case 0: return (LRESULT)tree->Roots;
        case 1: return node ? (LRESULT)node->Next : 0;
        case 2: {
            TREE_NODE *previous = 0;
            for (TREE_NODE *sibling = node ? (node->Parent ? node->Parent->Child : tree->Roots) : 0;
                 sibling && sibling != node; sibling = sibling->Next) previous = sibling;
            return (LRESULT)previous;
        }
        case 3: return node ? (LRESULT)node->Parent : 0;
        case 4: return node ? (LRESULT)node->Child : 0;
        case 5: return (LRESULT)tree_row_node(tree, tree->FirstRow);
        case 6: return node ? (LRESULT)tree_next_visible(node) : 0;
        case 7: return node ? (LRESULT)tree_row_node(tree, tree_row(tree, node) - 1) : 0;
        case 8: return (LRESULT)tree->Drop;
        case 9: return (LRESULT)tree->Selected;
        }
        return 0;
    case 0x110b:
        node = tree_node(tree, (void *)lp);
        if (lp && !node) return FALSE;
        if (wp == 9) return tree_select(tree, node, 0);
        if (wp == 8) { tree->Drop = node; return TRUE; }
        if (wp == 5 && node) { tree->FirstRow = tree_row(tree, node); return TRUE; }
        return FALSE;
    case 0x113e: {
        TREE_ITEM *item = (TREE_ITEM *)lp;
        node = item ? tree_node(tree, item->Handle) : 0;
        if (!node) return FALSE;
        TREE_ITEM value = tree_item(node);
        if (item->Mask & 1) {
            if (item->Text && item->TextCapacity > 0) {
                int n = 0;
                while (node->Text[n] && n + 1 < item->TextCapacity) {
                    item->Text[n] = node->Text[n];
                    n++;
                }
                item->Text[n] = 0;
            }
        }
        if (item->Mask & 2) item->Image = value.Image;
        if (item->Mask & 0x20) item->SelectedImage = value.SelectedImage;
        if (item->Mask & 4) item->Param = value.Param;
        if (item->Mask & 8) item->State = value.State & item->StateMask;
        if (item->Mask & 0x40) item->Children = node->Child ? 1 : value.Children;
        return TRUE;
    }
    case 0x113f: {
        TREE_ITEM *item = (TREE_ITEM *)lp;
        node = item ? tree_node(tree, item->Handle) : 0;
        if (!node || !tree_set_item(node, item)) return FALSE;
        InvalidateRect(tree->Window, 0, FALSE);
        return TRUE;
    }
    case 0x1114:
        node = tree_node(tree, (void *)lp);
        if (!node) return FALSE;
        for (TREE_NODE *parent = node->Parent; parent; parent = parent->Parent)
            if (!tree_expand(tree, parent, 2) || tree->Dead || node->Dead) return FALSE;
        tree_ensure_visible(tree, node);
        return TRUE;
    case 0x2006: return 1; /* CCM_GETUNICODEFORMAT */
    case 0x2005: return 1; /* CCM_SETUNICODEFORMAT */
    case WM_LBUTTONDOWN: {
        SetFocus(tree->Window);
        int y = (short)HIWORD(lp);
        if (y < 0) return 0;
        node = tree_row_node(tree, tree->FirstRow + y / tree->RowHeight);
        if (!node) return 0;
        int depth = 0;
        for (TREE_NODE *parent = node->Parent; parent; parent = parent->Parent) depth++;
        if ((short)LOWORD(lp) < 4 + depth * tree->Indent + 18)
            tree_expand(tree, node, 3);
        else
            tree_select(tree, node, 1);
        return 0;
    }
    case WM_KEYDOWN: {
        node = tree->Selected;
        int row = tree_row(tree, node);
        if (wp == VK_DOWN) tree_select(tree, tree_row_node(tree, row + 1), 2);
        else if (wp == VK_UP && row > 0) tree_select(tree, tree_row_node(tree, row - 1), 2);
        else if (node && wp == VK_RIGHT) {
            if (node->State & 0x20) { if (node->Child) tree_select(tree, node->Child, 2); }
            else tree_expand(tree, node, 2);
        } else if (node && wp == VK_LEFT) {
            if (node->State & 0x20) tree_expand(tree, node, 1);
            else if (node->Parent) tree_select(tree, node->Parent, 2);
        }
        return 0;
    }
    }
    return DefWindowProcW(tree->Window, message, wp, lp);
}

static LRESULT tree_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lp;
        TREE_STATE *tree = SvComctlAlloc(sizeof(*tree));
        if (!tree) return FALSE;
        tree->Window = window;
        tree->Parent = create->hwndParent;
        tree->Id = (UINT_PTR)create->hMenu;
        tree->Indent = 18;
        tree->RowHeight = 22;
        tree->References = 1;
        SetWindowLongPtrW(window, -21, (LONG_PTR)tree);
        return TRUE;
    }
    TREE_STATE *tree = (TREE_STATE *)GetWindowLongPtrW(window, -21);
    if (!tree) return DefWindowProcW(window, message, wp, lp);
    tree->References++;
    LRESULT result = 0;
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, -21, 0);
        tree->Dead = TRUE;
        tree->References--;
    } else if (!tree->Dead) {
        result = tree_message(tree, message, wp, lp);
    }
    if (!--tree->References) {
        TREE_NODE *node = tree->All;
        while (node) {
            TREE_NODE *next = node->AllNext;
            SvComctlFree(node->Text);
            SvComctlFree(node);
            node = next;
        }
        SvComctlFree(tree);
    }
    return result;
}

BOOL SvRegisterTreeView(void)
{
    WNDCLASSEXW cls = {0};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = tree_proc;
    cls.lpszClassName = L"SysTreeView32";
    cls.hCursor = LoadCursorW(0, IDC_ARROW);
    return RegisterClassExW(&cls) != 0;
}
