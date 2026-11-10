/* The current raster operates in device pixels with a zero viewport origin. */
#define SV_MM_TEXT 1

GDI32_API int SetMapMode(HDC dc, int mode)
{
    if (object_type(dc) != SV_GDI_TYPE_DC || mode != SV_MM_TEXT)
        return 0;
    return SV_MM_TEXT;
}

GDI32_API BOOL LPtoDP(HDC dc, POINT *points, int count)
{
    if (object_type(dc) != SV_GDI_TYPE_DC || count < 0 || (count && !points))
        return FALSE;
    for (int i = 0; i < count; i++) {
        const LONG min = -(1L << 26);
        const LONG max = (1L << 26) - 1;
        if (points[i].x < min || points[i].x > max ||
            points[i].y < min || points[i].y > max)
            return FALSE;
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
