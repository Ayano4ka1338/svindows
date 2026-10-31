/* Minimal C runtime support for freestanding Linux guest programs. */
#include <stddef.h>

void *memset(void *destination, int value, size_t length)
{
    volatile unsigned char *bytes = destination;
    for (size_t i = 0; i < length; i++)
        bytes[i] = (unsigned char)value;
    return destination;
}
