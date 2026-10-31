/* Svindows freestanding C string declarations. */
#ifndef _SVINDOWS_STRING_H
#define _SVINDOWS_STRING_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
#ifdef __cplusplus
}
#endif
#endif
