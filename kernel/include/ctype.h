/*
 * Freestanding ctype subset for the Svindows kernel.
 * Do not include the host libc header: its private lookup tables are
 * not part of the kernel ABI.
 */
#ifndef _SVINDOWS_CTYPE_H_
#define _SVINDOWS_CTYPE_H_

static inline int isdigit(int c) { return (unsigned)c - (unsigned)'0' <= 9u; }
static inline int islower(int c) { return (unsigned)c - (unsigned)'a' <= 25u; }
static inline int isupper(int c) { return (unsigned)c - (unsigned)'A' <= 25u; }
static inline int isalpha(int c) { return islower(c) || isupper(c); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
static inline int isxdigit(int c)
{
    return isdigit(c) || ((unsigned)c - (unsigned)'a' <= 5u) ||
           ((unsigned)c - (unsigned)'A' <= 5u);
}
static inline int isprint(int c) { return (unsigned)c >= 0x20u && (unsigned)c <= 0x7eu; }
static inline int isgraph(int c) { return (unsigned)c >= 0x21u && (unsigned)c <= 0x7eu; }
static inline int iscntrl(int c) { return (unsigned)c < 0x20u || c == 0x7f; }
static inline int ispunct(int c) { return isgraph(c) && !isalnum(c); }
static inline int isblank(int c) { return c == ' ' || c == '\t'; }
static inline int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
static inline int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }

#endif
