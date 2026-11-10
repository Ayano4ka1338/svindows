/* OLE data-transfer ownership. Do not conflate the release owner with the
 * medium: streams and filenames still have their own release operation. */
typedef struct STORAGE_UNKNOWN STORAGE_UNKNOWN;
typedef struct {
    LONG (*Query)(STORAGE_UNKNOWN *, const void *, void **);
    DWORD (*AddRef)(STORAGE_UNKNOWN *);
    DWORD (*Release)(STORAGE_UNKNOWN *);
} STORAGE_UNKNOWN_METHODS;
struct STORAGE_UNKNOWN { const STORAGE_UNKNOWN_METHODS *Methods; };
typedef struct {
    DWORD Type;
    void *Data;
    STORAGE_UNKNOWN *Owner;
} STORAGE_MEDIUM;
typedef struct { LONG Mode, XExtent, YExtent; HANDLE Metafile; } STORAGE_METAFILE;
_Static_assert(sizeof(STORAGE_MEDIUM)==24, "STGMEDIUM x64");
_Static_assert(sizeof(STORAGE_METAFILE)==24, "METAFILEPICT x64");

__declspec(dllexport) void ReleaseStgMedium(STORAGE_MEDIUM *medium)
{
    if (!medium) return;
    DWORD type = medium->Type;
    void *data = medium->Data;
    STORAGE_UNKNOWN *owner = medium->Owner;
    BOOL (*delete_graphic)(HANDLE) = 0;
    if (!owner && data && (type==16 || type==32 || type==64)) {
        HANDLE gdi = GetModuleHandleW(L"gdi32.dll");
        delete_graphic = (BOOL (*)(HANDLE))GetProcAddress(gdi,
            type==16 ? "DeleteObject" : type==32 ? "DeleteMetaFile" : "DeleteEnhMetaFile");
        if (!delete_graphic) { SetLastError(50); return; }
    }
    if (type!=0 && type!=1 && type!=2 && type!=4 && type!=8 &&
        type!=16 && type!=32 && type!=64) { SetLastError(87); return; }
    /* Callbacks can destroy the container which held the medium. */
    medium->Type=0; medium->Data=0; medium->Owner=0;
    if (data) {
        if (type==4 || type==8) {
            STORAGE_UNKNOWN *object = data;
            object->Methods->Release(object);
        } else if (type==2) {
            if (!owner) DeleteFileW(data);
            CoTaskMemFree(data);
        } else if (!owner) {
            if (type==1) GlobalFree(data);
            else if (type==16 || type==64) delete_graphic(data);
            else if (type==32) {
                STORAGE_METAFILE *picture = GlobalLock(data);
                if (picture) {
                    delete_graphic(picture->Metafile);
                    GlobalUnlock(data);
                }
                GlobalFree(data);
            }
        }
    }
    if (owner) owner->Methods->Release(owner);
}
