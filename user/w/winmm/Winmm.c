typedef unsigned long DWORD;
__declspec(dllimport) DWORD GetTickCount(void);
__declspec(dllexport) DWORD timeGetTime(void) { return GetTickCount(); }
