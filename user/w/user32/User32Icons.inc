#include "User32IconDib.inc"
typedef struct { HICON Handle; HINSTANCE Instance; HANDLE Group; } SV_USER_ICON;
static SV_USER_ICON g_user_icons[256];

static HICON load_icon_resource(HINSTANCE instance, const WCHAR *name, BOOL shared)
{
    if (!name) return NULL;
    UINT_PTR key = (UINT_PTR)name;
    HANDLE group = 0;
    if (!instance) {
        if (key < 32512 || key > 32518) return NULL;
    } else {
        group = FindResourceW(instance, name, MAKEINTRESOURCEW(14));
        if (!group || SizeofResource(instance, group) < 6) return NULL;
        key = (UINT_PTR)group;
    }
    HICON icon = (HICON)(UINT_PTR)SvNtUserCreateIconHandle(instance, key, shared);
    if (!icon) return NULL;
    SV_USER_ICON *free_slot = 0;
    for (unsigned i = 0; i < 256; ++i) {
        if (g_user_icons[i].Handle == icon) return icon;
        if (!g_user_icons[i].Handle && !free_slot) free_slot = &g_user_icons[i];
    }
    if (free_slot) *free_slot = (SV_USER_ICON){icon, instance, group};
    return icon;
}

USER32_API HICON LoadIconW(HINSTANCE instance, const WCHAR *name)
{ return load_icon_resource(instance, name, TRUE); }

USER32_API BOOL DestroyIcon(HICON icon)
{
    if (icon && SvNtUserDestroyIcon(icon)) {
        for (unsigned i = 0; i < 256; ++i)
            if (g_user_icons[i].Handle == icon) g_user_icons[i] = (SV_USER_ICON){0};
        return TRUE;
    }
    SetLastError(1402);
    return FALSE;
}

/* Internal bridge: image lists own a rendered bitmap, never the source icon. */
USER32_API HBITMAP SvUserIconToBitmap(HICON icon, int width, int height, COLORREF background)
{
    SV_USER_ICON *source = 0;
    for (unsigned i = 0; i < 256; ++i)
        if (g_user_icons[i].Handle == icon) source = &g_user_icons[i];
    if (!source || !source->Group || width <= 0 || width > 256 ||
        height <= 0 || height > 256) return 0;
    const unsigned char *group = LockResource(LoadResource(source->Instance, source->Group));
    DWORD size = SizeofResource(source->Instance, source->Group);
    if (!group || size < 6 || icon_u16(group) || icon_u16(group + 2) != 1) return 0;
    unsigned count = icon_u16(group + 4);
    if (!count || count > (size - 6) / 14) return 0;
    const unsigned char *best = 0;
    DWORD best_size = 0;
    unsigned best_score = ~0u;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *entry = group + 6 + i * 14;
        unsigned w = entry[0] ? entry[0] : 256, h = entry[1] ? entry[1] : 256;
        unsigned score = (w > (unsigned)width ? w - width : width - w) +
                         (h > (unsigned)height ? h - height : height - h);
        HANDLE resource = FindResourceW(source->Instance,
            MAKEINTRESOURCEW(icon_u16(entry + 12)), MAKEINTRESOURCEW(3));
        if (!resource) continue;
        DWORD bytes = SizeofResource(source->Instance, resource);
        const unsigned char *data = LockResource(LoadResource(source->Instance, resource));
        if (!data || bytes < 40 || icon_u32(data) < 40 || icon_u32(data) > bytes) continue;
        if (score < best_score || (score == best_score && best &&
            icon_u16(data + 14) > icon_u16(best + 14))) {
            best = data; best_size = bytes; best_score = score;
        }
    }
    if (!best) return 0;
    struct { DWORD Size; LONG Width, Height; WORD Planes, Bits;
        DWORD Compression, Bytes; LONG X, Y; DWORD Used, Important;
    } info = {40, width, -height, 1, 32, 0, 0, 0, 0, 0, 0};
    void *pixels = 0;
    HBITMAP bitmap = CreateDIBSection(0, &info, 0, &pixels, 0, 0);
    if (!bitmap) return 0;
    if (!sv_icon_dib(best, best_size, width, height, background, pixels)) {
        DeleteObject(bitmap); return 0;
    }
    return bitmap;
}
