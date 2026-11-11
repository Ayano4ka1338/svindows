__declspec(dllexport) unsigned long CredUIPromptForCredentialsW(const void *info,
    const unsigned short *target, void *context, unsigned long error,
    unsigned short *username, unsigned long username_size,
    unsigned short *password, unsigned long password_size,
    int *save, unsigned long flags)
{
    (void)info; (void)target; (void)context; (void)error;
    (void)username; (void)username_size; (void)password; (void)password_size;
    (void)save; (void)flags;
    return 50; /* ERROR_NOT_SUPPORTED: credential/logon UI is unavailable. */
}
