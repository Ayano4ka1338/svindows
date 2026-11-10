__declspec(dllexport) LONG RegConnectRegistryW(const WCHAR *computer, HKEY root,
    HKEY *result)
{
    if (!result) return 87; /* ERROR_INVALID_PARAMETER */
    *result = 0;
    if (!advapi_local_system(computer)) return 1722; /* RPC_S_SERVER_UNAVAILABLE */
    if (root != HKEY_LOCAL_MACHINE && root != HKEY_USERS)
        return 6; /* ERROR_INVALID_HANDLE */
    return RegOpenKeyExW(root, 0, 0, KEY_READ, result);
}

/* The current registry is volatile and no token holds the backup or restore
 * privilege. These calls must fail without importing or changing any keys. */
#define HIVE_PRIVILEGE_NOT_HELD 1314L

__declspec(dllexport) LONG RegRestoreKeyW(HKEY key, const WCHAR *file, DWORD flags)
{
    (void)key;
    (void)file;
    (void)flags;
    return HIVE_PRIVILEGE_NOT_HELD;
}

__declspec(dllexport) LONG RegSaveKeyW(HKEY key, const WCHAR *file,
    const void *security_attributes)
{
    (void)key;
    (void)file;
    (void)security_attributes;
    return HIVE_PRIVILEGE_NOT_HELD;
}

__declspec(dllexport) LONG RegLoadKeyW(HKEY key, const WCHAR *subkey,
    const WCHAR *file)
{
    (void)key;
    (void)subkey;
    (void)file;
    return HIVE_PRIVILEGE_NOT_HELD;
}

__declspec(dllexport) LONG RegUnLoadKeyW(HKEY key, const WCHAR *subkey)
{
    (void)key;
    (void)subkey;
    return HIVE_PRIVILEGE_NOT_HELD;
}

__declspec(dllexport) LONG RegFlushKey(HKEY key)
{
    (void)key;
    return 50; /* ERROR_NOT_SUPPORTED: no backing store. */
}
