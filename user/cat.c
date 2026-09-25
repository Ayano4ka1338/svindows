/* Minimal stdin -> stdout filter used by shell pipeline regressions. */
typedef void *HANDLE;
typedef void *LPVOID;
typedef unsigned long DWORD;
typedef int BOOL;
#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
__declspec(dllimport) HANDLE GetStdHandle(DWORD);
__declspec(dllimport) BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, LPVOID);

int main(void)
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    char buffer[512];
    DWORD got, written;
    while (ReadFile(input, buffer, sizeof(buffer), &got, 0) && got) {
        if (!WriteFile(output, buffer, got, &written, 0) || written != got)
            return 1;
    }
    return 0;
}
