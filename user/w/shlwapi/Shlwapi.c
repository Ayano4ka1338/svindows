typedef unsigned short WCHAR;

/* The Windows 7 calculator imports SHLWAPI ordinal 225, which is
 * SHStripMneumonicW. Remove mnemonic markers in place; doubled ampersands
 * represent one literal ampersand. */
__declspec(dllexport) WCHAR *SHStripMneumonicW(WCHAR *text)
{
    if (!text) return 0;
    WCHAR *read = text, *write = text;
    while (*read) {
        if (*read == '&') {
            read++;
            if (*read != '&') continue;
        }
        *write++ = *read++;
    }
    *write = 0;
    return text;
}
