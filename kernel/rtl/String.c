/*
 * rtl/String.c - freestanding C string helpers.
 */
#include <svindows/Rtl.h>

SIZE_T strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (SIZE_T)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(UCHAR)*a - (int)(UCHAR)*b;
}

int strncmp(const char *a, const char *b, SIZE_T n)
{
    while (n && *a && (*a == *b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(UCHAR)*a - (int)(UCHAR)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, SIZE_T n)
{
    SIZE_T i = 0;
    while (i < n && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    while (i < n) {
        dst[i] = '\0';
        ++i;
    }
    return dst;
}


/* Freestanding integer conversion used by the lwIP protocol stack. */
#include <ctype.h>
#include <limits.h>

static unsigned int rtl_digit_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return (unsigned int)(c - '0');
    if (c >= 'a' && c <= 'z') return (unsigned int)(c - 'a') + 10u;
    if (c >= 'A' && c <= 'Z') return (unsigned int)(c - 'A') + 10u;
    return 255u;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    unsigned long value = 0;
    int any = 0;
    while (isspace((unsigned char)*s)) ++s;
    if (*s == '+') ++s;
    else if (*s == '-') {
        /* C specifies unsigned negation modulo ULONG_MAX + 1. */
        ++s;
        if (base == 0) base = 10;
        if (base < 2 || base > 36) { if (endptr) *endptr = (char *)nptr; return 0; }
        while (1) {
            unsigned int d = rtl_digit_value((unsigned char)*s);
            if (d >= (unsigned int)base) break;
            value = value * (unsigned int)base + d;
            any = 1; ++s;
        }
        if (endptr) *endptr = (char *)(any ? s : nptr);
        return 0ul - value;
    }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16; s += 2;
    } else if (base == 0) {
        base = (*s == '0') ? 8 : 10;
    }
    if (base < 2 || base > 36) { if (endptr) *endptr = (char *)nptr; return 0; }
    while (1) {
        unsigned int d = rtl_digit_value((unsigned char)*s);
        if (d >= (unsigned int)base) break;
        value = value * (unsigned int)base + d;
        any = 1; ++s;
    }
    if (endptr) *endptr = (char *)(any ? s : nptr);
    return value;
}

long strtol(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    int negative = 0;
    unsigned long value;
    while (isspace((unsigned char)*s)) ++s;
    if (*s == '-') { negative = 1; ++s; }
    else if (*s == '+') ++s;
    value = strtoul(s, endptr, base);
    if (endptr && *endptr == s) *endptr = (char *)nptr;
    if (negative) return -(long)value;
    return (long)value;
}
