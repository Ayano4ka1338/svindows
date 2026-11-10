/* There is no native ACL property-sheet editor yet. */
__declspec(dllimport) void SetLastError(unsigned long error);
__declspec(dllexport) int EditSecurity(void *owner, void *security_information)
{
    (void)owner;
    SetLastError(security_information ? 50 : 87);
    return 0;
}
