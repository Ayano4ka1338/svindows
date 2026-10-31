#ifndef _WIN32K_RASTER_H_
#define _WIN32K_RASTER_H_

/* Rows are arrays of 32-bit pixels. Copy a pixel at a time rather than using
 * the kernel's byte-wise memcpy in composition and window publication. */
static inline void W32kCopyPixels(UINT32 *target, const UINT32 *source,
                                  SIZE_T count)
{
    for (SIZE_T i = 0; i < count; ++i) target[i] = source[i];
}

#endif
