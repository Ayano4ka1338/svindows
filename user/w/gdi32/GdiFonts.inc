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

__declspec(dllimport) void RtlSetLastWin32Error(DWORD);
typedef struct { UINT Charset, CodePage; DWORD Unicode[4], CodePages[2]; } SV_CHARSET_INFO;
GDI32_API BOOL TranslateCharsetInfo(DWORD *source, SV_CHARSET_INFO *out, DWORD flags)
{
    static const struct { UINT Charset, CodePage, Bit; } pages[] = {
        {0,1252,0},{238,1250,1},{204,1251,2},{161,1253,3},
        {162,1254,4},{177,1255,5},{178,1256,6},{186,1257,7},
        {163,1258,8},{222,874,16},{128,932,17},{134,936,18},
        {129,949,19},{136,950,20},{130,1361,21},{2,42,31}
    };
    if (!out || flags<1 || flags>3 || (flags==3 && !source)) {
        RtlSetLastWin32Error(87); return FALSE;
    }
    for (UINT i=0; i<sizeof(pages)/sizeof(pages[0]); i++) {
        BOOL match = flags==1 ? (ULONG_PTR)source==pages[i].Charset :
                     flags==2 ? (ULONG_PTR)source==pages[i].CodePage :
                     (source[0] & (1u<<pages[i].Bit))!=0;
        if (match) {
            out->Charset=pages[i].Charset; out->CodePage=pages[i].CodePage;
            for (UINT j=0;j<4;j++) out->Unicode[j]=0;
            out->CodePages[0]=1u<<pages[i].Bit; out->CodePages[1]=0;
            return TRUE;
        }
    }
    RtlSetLastWin32Error(87); return FALSE;
}

GDI32_API int SetROP2(HDC dc, int mode)
{
    if (!dc_attr(dc)) { RtlSetLastWin32Error(6); return 0; }
    if (mode!=13) { RtlSetLastWin32Error(mode<1 || mode>16 ? 87 : 50); return 0; }
    return 13; /* The pen rasterizer currently supports R2_COPYPEN only. */
}
GDI32_API UINT SetTextAlign(HDC dc, UINT alignment)
{
    if (!dc_attr(dc)) { RtlSetLastWin32Error(6); return (UINT)-1; }
    if (alignment) { RtlSetLastWin32Error(50); return (UINT)-1; }
    return 0; /* TA_LEFT | TA_TOP | TA_NOUPDATECP */
}
