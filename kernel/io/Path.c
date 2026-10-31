/* Shared DOS/FAT path rules for every personality using the I/O manager. */
#include "Internal.h"
#include <svindows/Rtl.h>

char IoPathLower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

BOOLEAN IoPathSame(const char *left, const char *right)
{
    while (*left && IoPathLower(*left) == IoPathLower(*right)) {
        left++;
        right++;
    }
    return (BOOLEAN)(*left == 0 && *right == 0);
}

BOOLEAN IoCanonicalizeDosPathKernel(const char *input, char output[260])
{
    if (!input || !*input)
        return FALSE;
    UINT32 in = 0, out = 0;
    if (input[1] == ':') {
        if (IoPathLower(input[0]) != 'c')
            return FALSE;
        in = 2;
    }
    output[out++] = 'C';
    output[out++] = ':';
    output[out++] = '\\';
    while (input[in] == '\\' || input[in] == '/') in++;

    while (input[in]) {
        char component[260];
        UINT32 length = 0;
        while (input[in] && input[in] != '\\' && input[in] != '/') {
            if (length + 1 >= sizeof(component))
                return FALSE;
            component[length++] = input[in++];
        }
        while (input[in] == '\\' || input[in] == '/') in++;
        component[length] = 0;
        if (!length || (length == 1 && component[0] == '.'))
            continue;
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            if (out > 3) {
                out--;
                while (out > 3 && output[out - 1] != '\\') out--;
            }
            continue;
        }
        if (out > 3 && output[out - 1] != '\\') {
            if (out + 1 >= 260) return FALSE;
            output[out++] = '\\';
        }
        if (out + length >= 260)
            return FALSE;
        memcpy(output + out, component, length);
        out += length;
    }
    output[out] = 0;
    return TRUE;
}

BOOLEAN IoResolveDosPathKernel(const char *input, const char *current_directory,
                               char output[260])
{
    if (!input || !*input) return FALSE;
    char joined[520];
    UINT32 prefix = 0;
    if (input[1] == ':') {
        if (input[2] != '\\' && input[2] != '/') return FALSE;
    } else if (input[0] == '\\' || input[0] == '/') {
        joined[prefix++] = 'C';
        joined[prefix++] = ':';
    } else {
        const char *cwd = current_directory && *current_directory
                              ? current_directory : "C:\\";
        while (cwd[prefix] && prefix < 259) {
            joined[prefix] = cwd[prefix];
            prefix++;
        }
        if (cwd[prefix]) return FALSE;
        if (joined[prefix - 1] != '\\' && joined[prefix - 1] != '/')
            joined[prefix++] = '\\';
    }
    UINT32 n = 0;
    while (input[n] && prefix + n + 1 < sizeof(joined)) n++;
    if (input[n]) return FALSE;
    memcpy(joined + prefix, input, n + 1);
    return IoCanonicalizeDosPathKernel(joined, output);
}

BOOLEAN IoPathIsWithin(const char *directory, const char *path)
{
    while (*directory && IoPathLower(*directory) == IoPathLower(*path)) {
        directory++;
        path++;
    }
    if (*directory)
        return FALSE;
    return *path == 0 || *path == '\\' || *path == '/';
}

