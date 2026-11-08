typedef unsigned short WCHAR;
typedef unsigned char BYTE;
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef unsigned long long SIZE_T;
typedef long HRESULT;

__declspec(dllimport) void *GlobalAlloc(unsigned int flags, SIZE_T bytes);
__declspec(dllimport) void *GlobalFree(void *memory);

#define GMEM_FIXED 0
#define VT_EMPTY 0
#define VT_BSTR 8
#define VT_BYREF 0x4000
#define DISP_E_BADVARTYPE ((HRESULT)0x80020008u)

static WCHAR *ole_alloc_bstr(const void *source, UINT bytes)
{
    BYTE *allocation = (BYTE *)GlobalAlloc(GMEM_FIXED,
                                  (SIZE_T)sizeof(DWORD) + bytes + sizeof(WCHAR));
    if (!allocation) return 0;
    *(DWORD *)allocation = bytes;
    WCHAR *result = (WCHAR *)(allocation + sizeof(DWORD));
    if (source && bytes) {
        const BYTE *in = (const BYTE *)source;
        for (UINT i = 0; i < bytes; i++) allocation[sizeof(DWORD) + i] = in[i];
    }
    result[bytes / sizeof(WCHAR)] = 0;
    return result;
}

__declspec(dllexport) WCHAR *SysAllocString(const WCHAR *text)
{
    if (!text) return 0;
    UINT units = 0;
    while (text[units] && units < 0x3fffffffu) units++;
    if (units == 0x3fffffffu) return 0;
    return ole_alloc_bstr(text, units * sizeof(WCHAR));
}

__declspec(dllexport) WCHAR *SysAllocStringByteLen(const char *text,
                                                    UINT length)
{
    return ole_alloc_bstr(text, length);
}

__declspec(dllexport) void SysFreeString(WCHAR *text)
{
    if (text) (void)GlobalFree((BYTE *)text - sizeof(DWORD));
}

__declspec(dllexport) UINT SysStringLen(const WCHAR *text)
{
    if (!text) return 0;
    return *(const DWORD *)((const BYTE *)text - sizeof(DWORD)) / sizeof(WCHAR);
}

__declspec(dllexport) void VariantInit(void *variant)
{
    if (!variant) return;
    BYTE *p = (BYTE *)variant;
    for (UINT i = 0; i < 24; i++) p[i] = 0;
}

__declspec(dllexport) HRESULT VariantClear(void *variant)
{
    if (!variant) return DISP_E_BADVARTYPE;
    BYTE *p = (BYTE *)variant;
    unsigned short type = *(unsigned short *)p;
    if (!(type & VT_BYREF) && (type & 0x0fff) == VT_BSTR)
        SysFreeString(*(WCHAR **)(p + 8));
    /* Interface variants own one COM reference. The IUnknown vtable starts
     * with QueryInterface/AddRef/Release under the standard COM ABI. */
    else if (!(type & VT_BYREF) && ((type & 0x0fff) == 9 ||
                                   (type & 0x0fff) == 13)) {
        void **object = *(void ***)(p + 8);
        if (object && object[0]) {
            void (**vtable)(void) = (void (**)(void))object[0];
            ((unsigned long long (*)(void *))vtable[2])(object);
        }
    } else if ((type & 0x0fff) > 14 && !(type & VT_BYREF)) {
        return DISP_E_BADVARTYPE;
    }
    VariantInit(variant);
    return 0;
}
