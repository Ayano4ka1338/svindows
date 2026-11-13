/* USER session API: executive-object handles and captured name enumeration. */
__declspec(dllimport) UINT64 NtUserGetProcessWindowStation(void);
__declspec(dllimport) UINT64 NtUserOpenDesktop(const WCHAR *, DWORD, BOOL, DWORD);
__declspec(dllimport) UINT64 NtUserCloseDesktop(HANDLE);
__declspec(dllimport) UINT64 NtUserGetThreadDesktop(DWORD);
__declspec(dllimport) UINT64 NtUserSetThreadDesktop(HANDLE);
__declspec(dllimport) UINT64 NtUserOpenInputDesktop(DWORD, BOOL, DWORD);
__declspec(dllimport) UINT64 NtUserOpenWindowStation(const WCHAR *, BOOL, DWORD);
__declspec(dllimport) UINT64 NtUserSetProcessWindowStation(HANDLE);
__declspec(dllimport) UINT64 NtUserCloseWindowStation(HANDLE);
__declspec(dllimport) UINT64 NtUserGetObjectInformation(HANDLE, int, void *, DWORD, DWORD *);
__declspec(dllimport) UINT64 SvNtUserCreateWindowStation(const WCHAR *, DWORD, DWORD, void *);
__declspec(dllimport) UINT64 SvNtUserCreateDesktop(const WCHAR *, const WCHAR *, void *, DWORD, DWORD, void *);
__declspec(dllimport) UINT64 SvNtUserEnumDesktops(HANDLE, WCHAR *, DWORD, DWORD *);
__declspec(dllimport) UINT64 SvNtUserEnumWindowStations(HANDLE, WCHAR *, DWORD, DWORD *);
__declspec(dllimport) UINT64 SvNtUserSwitchDesktop(HANDLE);
__declspec(dllimport) UINT64 SvNtUserSetObjectInformation(HANDLE, int, void *, DWORD);
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(HANDLE, DWORD, SIZE_T);
__declspec(dllimport) BOOL HeapFree(HANDLE, DWORD, void *);
__declspec(dllimport) int MultiByteToWideChar(UINT, DWORD, const char *, int, WCHAR *, int);
__declspec(dllimport) int WideCharToMultiByte(UINT, DWORD, const WCHAR *, int, char *, int, const char *, BOOL *);
__declspec(dllimport) DWORD GetLastError(void);
__declspec(dllimport) LONG NtQuerySecurityObject(HANDLE,DWORD,void *,DWORD,DWORD *);
__declspec(dllimport) LONG NtSetSecurityObject(HANDLE,DWORD,const void *);
__declspec(dllimport) ULONG RtlNtStatusToDosError(UINT32);

static WCHAR *sv_session_ansi_name(const char *source)
{
    if (!source) return NULL;
    int chars = MultiByteToWideChar(0, 0, source, -1, NULL, 0);
    if (!chars) return NULL;
    WCHAR *result = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)chars * sizeof(WCHAR));
    if (!result) { SetLastError(8); return NULL; }
    if (!MultiByteToWideChar(0, 0, source, -1, result, chars)) {
        HeapFree(GetProcessHeap(), 0, result); return NULL;
    }
    return result;
}

USER32_API HANDLE CreateWindowStationW(const WCHAR *name, DWORD flags, DWORD access, void *security)
{ return (HANDLE)(UINT_PTR)SvNtUserCreateWindowStation(name, flags, access, security); }
USER32_API HANDLE CreateWindowStationA(const char *name, DWORD flags, DWORD access, void *security)
{
    WCHAR *wide = sv_session_ansi_name(name);
    if (name && !wide) return NULL;
    HANDLE result = CreateWindowStationW(wide, flags, access, security);
    if (wide) HeapFree(GetProcessHeap(), 0, wide);
    return result;
}
USER32_API HANDLE OpenWindowStationW(const WCHAR *name, BOOL inherit, DWORD access)
{ return (HANDLE)(UINT_PTR)NtUserOpenWindowStation(name, inherit, access); }
USER32_API HANDLE OpenWindowStationA(const char *name, BOOL inherit, DWORD access)
{
    WCHAR *wide = sv_session_ansi_name(name); if (!wide) return NULL;
    HANDLE result = OpenWindowStationW(wide, inherit, access);
    HeapFree(GetProcessHeap(), 0, wide); return result;
}
USER32_API BOOL CloseWindowStation(HANDLE station)
{ return NtUserCloseWindowStation(station) != 0; }
USER32_API HANDLE GetProcessWindowStation(void)
{ return (HANDLE)(UINT_PTR)NtUserGetProcessWindowStation(); }
USER32_API BOOL SetProcessWindowStation(HANDLE station)
{ return NtUserSetProcessWindowStation(station) != 0; }
USER32_API HANDLE CreateDesktopW(const WCHAR *name, const WCHAR *device, void *mode,
    DWORD flags, DWORD access, void *security)
{ return (HANDLE)(UINT_PTR)SvNtUserCreateDesktop(name, device, mode, flags, access, security); }
USER32_API HANDLE CreateDesktopA(const char *name, const char *device, void *mode,
    DWORD flags, DWORD access, void *security)
{
    WCHAR *wide = sv_session_ansi_name(name); if (!wide) return NULL;
    WCHAR *wide_device = sv_session_ansi_name(device);
    if (device && !wide_device) { HeapFree(GetProcessHeap(), 0, wide); return NULL; }
    HANDLE result = CreateDesktopW(wide, wide_device, mode, flags, access, security);
    HeapFree(GetProcessHeap(), 0, wide);
    if (wide_device) HeapFree(GetProcessHeap(), 0, wide_device);
    return result;
}
USER32_API HANDLE OpenDesktopW(const WCHAR *name, DWORD flags, BOOL inherit, DWORD access)
{ return (HANDLE)(UINT_PTR)NtUserOpenDesktop(name, flags, inherit, access); }
USER32_API HANDLE OpenDesktopA(const char *name, DWORD flags, BOOL inherit, DWORD access)
{
    WCHAR *wide = sv_session_ansi_name(name); if (!wide) return NULL;
    HANDLE result = OpenDesktopW(wide, flags, inherit, access);
    HeapFree(GetProcessHeap(), 0, wide); return result;
}
USER32_API HANDLE OpenInputDesktop(DWORD flags, BOOL inherit, DWORD access)
{ return (HANDLE)(UINT_PTR)NtUserOpenInputDesktop(flags, inherit, access); }
USER32_API BOOL CloseDesktop(HANDLE desktop)
{ return NtUserCloseDesktop(desktop) != 0; }
USER32_API HANDLE GetThreadDesktop(DWORD thread)
{
    if (!thread) { SetLastError(87); return NULL; }
    return (HANDLE)(UINT_PTR)NtUserGetThreadDesktop(thread);
}
USER32_API BOOL SetThreadDesktop(HANDLE desktop)
{ return NtUserSetThreadDesktop(desktop) != 0; }
USER32_API BOOL SwitchDesktop(HANDLE desktop)
{ return SvNtUserSwitchDesktop(desktop) != 0; }
USER32_API BOOL GetUserObjectInformationW(HANDLE object, int index, void *info,
    DWORD bytes, DWORD *needed)
{ return NtUserGetObjectInformation(object, index, info, bytes, needed) != 0; }

USER32_API BOOL GetUserObjectInformationA(HANDLE object, int index, void *info,
    DWORD bytes, DWORD *needed)
{
    if (index != 2 && index != 3) return GetUserObjectInformationW(object, index, info, bytes, needed);
    DWORD wide_bytes = 0;
    GetUserObjectInformationW(object, index, NULL, 0, &wide_bytes);
    if (!wide_bytes || GetLastError() != 122) return FALSE;
    WCHAR *wide = HeapAlloc(GetProcessHeap(), 0, wide_bytes);
    if (!wide) { SetLastError(8); return FALSE; }
    BOOL result = GetUserObjectInformationW(object, index, wide, wide_bytes, &wide_bytes);
    if (result) {
        int required = WideCharToMultiByte(0, 0, wide, -1, NULL, 0, NULL, NULL);
        if (needed) *needed = required > 0 ? (DWORD)required : 0;
        if (!required) result = FALSE;
        else if (!info || bytes < (DWORD)required) { SetLastError(122); result = FALSE; }
        else result = WideCharToMultiByte(0, 0, wide, -1, info, (int)bytes, NULL, NULL) != 0;
    }
    HeapFree(GetProcessHeap(), 0, wide); return result;
}
USER32_API BOOL SetUserObjectInformationW(HANDLE object, int index, void *info, DWORD bytes)
{ return SvNtUserSetObjectInformation(object, index, info, bytes) != 0; }
USER32_API BOOL SetUserObjectInformationA(HANDLE object, int index, void *info, DWORD bytes)
{ return SetUserObjectInformationW(object, index, info, bytes); }

USER32_API BOOL GetUserObjectSecurity(HANDLE object,DWORD *information,
    void *descriptor,DWORD bytes,DWORD *needed)
{
    if(!information||!needed){SetLastError(87);return FALSE;}
    LONG status=NtQuerySecurityObject(object,*information,descriptor,bytes,needed);
    if(status<0){SetLastError(RtlNtStatusToDosError(status));return FALSE;}
    return TRUE;
}
USER32_API BOOL SetUserObjectSecurity(HANDLE object,DWORD *information,void *descriptor)
{
    if(!information||!descriptor){SetLastError(87);return FALSE;}
    LONG status=NtSetSecurityObject(object,*information,descriptor);
    if(status<0){SetLastError(RtlNtStatusToDosError(status));return FALSE;}
    return TRUE;
}

typedef BOOL (*SV_SESSION_ENUM_NATIVE)(HANDLE, WCHAR *, DWORD, DWORD *);
static BOOL sv_session_enum_desktops(HANDLE station, WCHAR *names, DWORD chars, DWORD *needed)
{ return SvNtUserEnumDesktops(station, names, chars, needed) != 0; }
static BOOL sv_session_enum_stations(HANDLE unused, WCHAR *names, DWORD chars, DWORD *needed)
{ return SvNtUserEnumWindowStations(unused, names, chars, needed) != 0; }

static WCHAR *sv_session_names(HANDLE station, SV_SESSION_ENUM_NATIVE enumerate)
{
    DWORD needed = 0;
    enumerate(station, NULL, 0, &needed);
    if (!needed || GetLastError() != 122) return NULL;
    for (;;) {
        WCHAR *names = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)needed * sizeof(WCHAR));
        if (!names) { SetLastError(8); return NULL; }
        DWORD capacity = needed;
        if (enumerate(station, names, capacity, &needed)) return names;
        DWORD error = GetLastError(); HeapFree(GetProcessHeap(), 0, names);
        if (error != 122 || needed <= capacity) { SetLastError(error); return NULL; }
    }
}

static BOOL sv_session_enum_names(HANDLE station, SV_SESSION_ENUM_NATIVE enumerate,
    BOOL (*wide_callback)(WCHAR *, LONG_PTR), BOOL (*ansi_callback)(char *, LONG_PTR), LONG_PTR data)
{
    if (!wide_callback && !ansi_callback) { SetLastError(87); return FALSE; }
    WCHAR *names = sv_session_names(station, enumerate); if (!names) return FALSE;
    BOOL result = TRUE;
    for (WCHAR *name = names; *name && result; ) {
        if (wide_callback) result = wide_callback(name, data);
        else {
            int bytes = WideCharToMultiByte(0, 0, name, -1, NULL, 0, NULL, NULL);
            char *ansi = bytes ? HeapAlloc(GetProcessHeap(), 0, (SIZE_T)bytes) : NULL;
            if (!ansi) { SetLastError(bytes ? 8 : GetLastError()); result = FALSE; }
            else {
                if (!WideCharToMultiByte(0, 0, name, -1, ansi, bytes, NULL, NULL)) result = FALSE;
                else result = ansi_callback(ansi, data);
                HeapFree(GetProcessHeap(), 0, ansi);
            }
        }
        while (*name) name++; name++;
    }
    HeapFree(GetProcessHeap(), 0, names); return result;
}
USER32_API BOOL EnumDesktopsW(HANDLE station, BOOL (*callback)(WCHAR *, LONG_PTR), LONG_PTR data)
{ return sv_session_enum_names(station, sv_session_enum_desktops, callback, NULL, data); }
USER32_API BOOL EnumDesktopsA(HANDLE station, BOOL (*callback)(char *, LONG_PTR), LONG_PTR data)
{ return sv_session_enum_names(station, sv_session_enum_desktops, NULL, callback, data); }
USER32_API BOOL EnumWindowStationsW(BOOL (*callback)(WCHAR *, LONG_PTR), LONG_PTR data)
{ return sv_session_enum_names(NULL, sv_session_enum_stations, callback, NULL, data); }
USER32_API BOOL EnumWindowStationsA(BOOL (*callback)(char *, LONG_PTR), LONG_PTR data)
{ return sv_session_enum_names(NULL, sv_session_enum_stations, NULL, callback, data); }
