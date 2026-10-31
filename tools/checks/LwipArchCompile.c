/* Compile against the staged upstream lwIP arch.h before building its objects. */
#include <lwip/arch.h>
#include <ctype.h>
#include <stdlib.h>

#ifndef U16_F
#error "lwIP U16_F formatter is unavailable"
#endif
#ifndef U32_F
#error "lwIP U32_F formatter is unavailable"
#endif
#ifndef INT_MAX
#error "INT_MAX is unavailable to lwIP"
#endif
#ifndef SSIZE_MAX
#error "SSIZE_MAX is unavailable to lwIP"
#endif

void SvLwipArchCompileProbe(u16_t length, u32_t total)
{
    LWIP_PLATFORM_DIAG(("lwIP format probe: len=%" U16_F " total=%" U32_F "\n",
                        (unsigned int)length, (unsigned int)total));
}

/* Verify freestanding libc compatibility without importing host libc. */
int SvLwipLibcCompileProbe(const char *text)
{
    char *end = 0;
    return isdigit((unsigned char)*text) + atoi(text) +
           (int)strtol(text, &end, 10);
}
