/* Station-owned clipboard transport; all buffers are captured by the kernel. */
#ifndef SVINDOWS_CLIPBOARD_ABI_H
#define SVINDOWS_CLIPBOARD_ABI_H
#define SV_CLIP_OPEN 0u
#define SV_CLIP_CLOSE 1u
#define SV_CLIP_EMPTY 2u
#define SV_CLIP_OWNER 3u
#define SV_CLIP_OPEN_WINDOW 4u
#define SV_CLIP_SEQUENCE 5u
#define SV_CLIP_COUNT 6u
#define SV_CLIP_AVAILABLE 7u
#define SV_CLIP_ENUMERATE 8u
#define SV_CLIP_REGISTER_FORMAT 9u
#define SV_CLIP_FORMAT_NAME 10u
#define SV_CLIP_ADD_LISTENER 11u
#define SV_CLIP_REMOVE_LISTENER 12u
#define SV_CLIP_CONTENT_REVISION 13u
#define SV_CLIP_EXACT_FORMAT 14u
#define SV_CLIP_CHANGE_EVENT 15u
typedef struct _SV_CLIPBOARD_DATA {
    UINT64 Bytes;
    UINT64 Revision;
    UINT64 Version;
    UINT32 Format;
    UINT32 Reserved;
} SV_CLIPBOARD_DATA;
#endif
