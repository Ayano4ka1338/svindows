/* INF-based registration is not available. Callers can detect the failure
 * and keep their ordinary UI operations independent of file associations. */
__declspec(dllexport) long RegInstallW(void *module, const char *section,
    const void *substitutions)
{
    (void)module; (void)section; (void)substitutions;
    return (long)0x80004001u; /* E_NOTIMPL */
}
