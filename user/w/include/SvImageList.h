#ifndef SV_IMAGE_LIST_H
#define SV_IMAGE_LIST_H
/* Shared in-process image-list representation consumed by standard controls. */
#define SV_IMAGE_LIST_MAGIC 0x53494c31u
#define SV_IMAGE_LIST_MAX 64
typedef struct {
    unsigned int Magic;
    int Width, Height, Flags, Count, Capacity;
    HBITMAP Images[SV_IMAGE_LIST_MAX];
    unsigned int Background;
} SV_IMAGELIST;
#endif
