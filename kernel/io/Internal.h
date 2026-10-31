/* Private I/O manager path helpers; ABI translation stays in each personality. */
#ifndef _SVINDOWS_IO_INTERNAL_H_
#define _SVINDOWS_IO_INTERNAL_H_

#include <svindows/Io.h>

char IoPathLower(char c);
BOOLEAN IoPathSame(const char *left, const char *right);
BOOLEAN IoPathIsWithin(const char *directory, const char *path);

#endif
