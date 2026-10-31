/*
 * user/w/crt/Crt0.c - the C runtime startup (mainCRTStartup).
 *
 * This is the entry point the linker uses for a console program by default:
 * a program just defines `int main(...)` and gets started here. We set up a
 * minimal argc/argv from the command line, call main, and exit with its return
 * value -- the same contract the real CRT startup provides, so a standard C
 * program needs no OS-specific glue of its own.
 */

typedef unsigned long DWORD;
typedef unsigned short WCHAR;

/* MSVC RTTI type descriptors reference the type_info vftable. C++ programs
 * that use exceptions need the symbol even though Svindows does not implement
 * typeid/dynamic_cast; one shared empty table satisfies the reference. */
const void *__cxx_type_info_vftable __asm__("??_7type_info@@6B@") = 0;

extern char *GetCommandLineA(void);       /* kernel32 */
extern WCHAR *GetEnvironmentStringsW(void);
extern int FreeEnvironmentStringsW(WCHAR *block);
extern void  ExitProcess(DWORD code);     /* kernel32 */

/* The program's entry. Declared with the widest form; a `main(void)` simply
 * ignores the arguments. */
extern int main(int argc, char **argv, char **envp);

static char  g_cmdline[768];
static char *g_argv[65];
static char  g_environment[2048];
static char *g_envp[65];

void mainCRTStartup(void)
{
    /* Copy the command line so we can tokenize it in place. */
    char *src = GetCommandLineA();
    int i = 0;
    for (; src && src[i] && i < (int)sizeof(g_cmdline) - 1; i++)
        g_cmdline[i] = src[i];
    g_cmdline[i] = 0;

    /* Parse backslashes and quotes with the same rules used when Linux argv
     * is encoded as a PE command line. Parsing in place only shortens it. */
    int argc = 0;
    char *p = g_cmdline, *write = g_cmdline;
    while (*p && argc < 64) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        g_argv[argc++] = write;
        int quoted = 0;
        while (*p) {
            unsigned slashes = 0;
            while (*p == '\\') { slashes++; p++; }
            if (*p == '"') {
                for (unsigned count = slashes / 2; count; count--)
                    *write++ = '\\';
                if (slashes & 1) {
                    *write++ = '"';
                    p++;
                } else if (quoted && p[1] == '"') {
                    *write++ = '"';
                    p += 2;
                } else {
                    quoted = !quoted;
                    p++;
                }
                continue;
            }
            while (slashes--) *write++ = '\\';
            if (!*p || (!quoted && (*p == ' ' || *p == '\t')))
                break;
            *write++ = *p++;
        }
        while (*p == ' ' || *p == '\t')
            p++;
        *write++ = 0;
    }
    g_argv[argc] = 0;

    WCHAR *block = GetEnvironmentStringsW();
    unsigned used = 0, count = 0;
    for (const WCHAR *entry = block; entry && *entry && count < 64; ) {
        g_envp[count++] = g_environment + used;
        while (*entry && used + 2 < sizeof(g_environment)) {
            g_environment[used++] = *entry <= 0x7f ? (char)*entry : '?';
            entry++;
        }
        if (*entry) {
            g_environment[used] = 0;
            count--;
            break;
        }
        g_environment[used++] = 0;
        entry++;
    }
    g_envp[count] = 0;
    if (block) FreeEnvironmentStringsW(block);

    int ret = main(argc, g_argv, g_envp);
    ExitProcess((DWORD)ret);
}
