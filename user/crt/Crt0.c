/*
 * user/crt/Crt0.c - the C runtime startup (mainCRTStartup).
 *
 * This is the entry point the linker uses for a console program by default:
 * a program just defines `int main(...)` and gets started here. We set up a
 * minimal argc/argv from the command line, call main, and exit with its return
 * value -- the same contract the real CRT startup provides, so a standard C
 * program needs no OS-specific glue of its own.
 */

typedef unsigned long DWORD;

/* MSVC RTTI type descriptors reference the type_info vftable. C++ programs
 * that use exceptions need the symbol even though Svindows does not implement
 * typeid/dynamic_cast; one shared empty table satisfies the reference. */
const void *__cxx_type_info_vftable __asm__("??_7type_info@@6B@") = 0;

extern char *GetCommandLineA(void);       /* kernel32 */
extern void  ExitProcess(DWORD code);     /* kernel32 */

/* The program's entry. Declared with the widest form; a `main(void)` simply
 * ignores the arguments. */
extern int main(int argc, char **argv, char **envp);

static char  g_cmdline[260];
static char *g_argv[32];

void mainCRTStartup(void)
{
    /* Copy the command line so we can tokenize it in place. */
    char *src = GetCommandLineA();
    int i = 0;
    for (; src && src[i] && i < (int)sizeof(g_cmdline) - 1; i++)
        g_cmdline[i] = src[i];
    g_cmdline[i] = 0;

    /* Split in place, retaining spaces inside a quoted argument and removing
     * the quote delimiters. This is enough for normal command paths without
     * pulling a full CRT command-line parser into every small utility. */
    int argc = 0;
    char *p = g_cmdline, *write = g_cmdline;
    while (*p && argc < 31) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        g_argv[argc++] = write;
        int quoted = 0;
        while (*p) {
            if (*p == '"') {
                quoted = !quoted;
                p++;
                continue;
            }
            if (!quoted && *p == ' ')
                break;
            *write++ = *p++;
        }
        while (*p == ' ')
            p++;
        *write++ = 0;
    }
    g_argv[argc] = 0;

    int ret = main(argc, g_argv, &g_argv[argc]); /* empty envp */
    ExitProcess((DWORD)ret);
}
