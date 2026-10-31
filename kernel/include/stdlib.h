/* Minimal freestanding stdlib declarations for kernel and lwIP code. */
#ifndef _SVINDOWS_STDLIB_H_
#define _SVINDOWS_STDLIB_H_

#include <stddef.h>

long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

static inline int atoi(const char *s)
{
    return (int)strtol(s, (char **)0, 10);
}

#endif
