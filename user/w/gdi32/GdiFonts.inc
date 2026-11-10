/* The GDI raster uses the built-in Selawik atlas at every supported size. */
static const WCHAR gdi_face_name[] = L"Selawik";

GDI32_API int GetTextFaceW(HDC dc, int capacity, WCHAR *name)
{
    if (object_type(dc) != SV_GDI_TYPE_DC)
        return 0;

    int length = sizeof(gdi_face_name) / sizeof(gdi_face_name[0]);
    if (!name) return length;
    if (capacity <= 0) return 0;
    if (length > capacity) length = capacity;
    memcpy(name, gdi_face_name, (length - 1) * sizeof(*name));
    name[length - 1] = 0;
    return length;
}

static BOOL is_builtin_face(const WCHAR *name)
{
    for (UINT i = 0; i < sizeof(gdi_face_name) / sizeof(gdi_face_name[0]); i++) {
        WCHAR ch = name[i];
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        WCHAR expected = gdi_face_name[i];
        if (expected >= 'A' && expected <= 'Z') expected += 'a' - 'A';
        if (ch != expected) return FALSE;
    }
    return TRUE;
}

GDI32_API int EnumFontsW(HDC dc, const WCHAR *name,
    int (*callback)(const LOGFONTW *, const TEXTMETRICW *, DWORD, LONG_PTR),
    LONG_PTR param)
{
    if (!callback || object_type(dc) != SV_GDI_TYPE_DC) return 0;
    if (name && *name && !is_builtin_face(name)) return 1;

    HDC metrics_dc = CreateCompatibleDC(dc);
    if (!metrics_dc) return 0;
    TEXTMETRICW metrics;
    BOOL ok = GetTextMetricsW(metrics_dc, &metrics);
    DeleteDC(metrics_dc);
    if (!ok) return 0;

    LOGFONTW font = {0};
    font.lfHeight = metrics.tmHeight;
    font.lfWeight = metrics.tmWeight;
    font.lfCharSet = metrics.tmCharSet;
    font.lfPitchAndFamily = 0x22; /* VARIABLE_PITCH | FF_SWISS */
    memcpy(font.lfFaceName, gdi_face_name, sizeof(gdi_face_name));
    return callback(&font, &metrics, 1 /* RASTER_FONTTYPE */, param);
}
