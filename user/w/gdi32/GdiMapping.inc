/* The current raster operates in device pixels, with an optional offset. */
__declspec(dllimport) UINT64 SvNtGdiViewportOrigin(HDC dc, const POINT *origin,
                                                  POINT *previous);
#define SV_MM_TEXT 1

GDI32_API BOOL GdiFlush(void)
{
    return TRUE;
}

GDI32_API DWORD GetLayout(HDC dc)
{
    if (object_type(dc) != SV_GDI_TYPE_DC)
        return 0xffffffffu;
    return 0;
}

GDI32_API int SetMapMode(HDC dc, int mode)
{
    if (object_type(dc) != SV_GDI_TYPE_DC || mode != SV_MM_TEXT)
        return 0;
    return SV_MM_TEXT;
}

GDI32_API BOOL SetViewportOrgEx(HDC dc, int x, int y, POINT *previous)
{
    const POINT origin = {x, y};
    return SvNtGdiViewportOrigin(dc, &origin, previous) != 0;
}

GDI32_API BOOL SetWindowOrgEx(HDC dc, int x, int y, POINT *previous)
{
    return SetViewportOrgEx(dc, x, y, previous);
}

GDI32_API BOOL OffsetViewportOrgEx(HDC dc, int dx, int dy, POINT *previous)
{
    POINT current;
    if (!SvNtGdiViewportOrigin(dc, 0, &current))
        return FALSE;
    POINT updated = {current.x + dx, current.y + dy};
    return SvNtGdiViewportOrigin(dc, &updated, previous) != 0;
}

GDI32_API BOOL LPtoDP(HDC dc, POINT *points, int count)
{
    POINT origin;
    if (count < 0 || (count && !points) || !SvNtGdiViewportOrigin(dc, 0, &origin))
        return FALSE;
    const LONG min = -(1L << 26);
    const LONG max = (1L << 26) - 1;
    for (int i = 0; i < count; i++) {
        INT64 x = (INT64)points[i].x + origin.x;
        INT64 y = (INT64)points[i].y + origin.y;
        if (x < min || x > max || y < min || y > max) return FALSE;
        points[i] = (POINT){(LONG)x, (LONG)y};
    }
    return TRUE;
}

static BOOL fixed_dc_extents(HDC dc, SIZE *previous)
{
    if (object_type(dc) != SV_GDI_TYPE_DC)
        return FALSE;
    if (previous)
        *previous = (SIZE){1, 1};
    return TRUE;
}

GDI32_API BOOL SetWindowExtEx(HDC dc, int x, int y, SIZE *previous)
{
    (void)x;
    (void)y;
    return fixed_dc_extents(dc, previous);
}

GDI32_API BOOL SetViewportExtEx(HDC dc, int x, int y, SIZE *previous)
{
    (void)x;
    (void)y;
    return fixed_dc_extents(dc, previous);
}
