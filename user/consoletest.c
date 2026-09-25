/* End-to-end regression for the Kernel32 console surface. */
typedef void *HANDLE;
typedef void *LPVOID;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef int BOOL;

typedef struct _COORD { short X, Y; } COORD;
typedef struct _SMALL_RECT { short Left, Top, Right, Bottom; } SMALL_RECT;
typedef struct _CONSOLE_SCREEN_BUFFER_INFO {
    COORD Size;
    COORD CursorPosition;
    WORD Attributes;
    SMALL_RECT Window;
    COORD MaximumWindowSize;
} CONSOLE_SCREEN_BUFFER_INFO;
typedef struct _CONSOLE_CURSOR_INFO { DWORD Size; BOOL Visible; }
    CONSOLE_CURSOR_INFO;

#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)

__declspec(dllimport) HANDLE GetStdHandle(DWORD);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) BOOL GetConsoleMode(HANDLE, DWORD *);
__declspec(dllimport) BOOL SetConsoleMode(HANDLE, DWORD);
__declspec(dllimport) BOOL GetConsoleScreenBufferInfo(
    HANDLE, CONSOLE_SCREEN_BUFFER_INFO *);
__declspec(dllimport) BOOL SetConsoleTextAttribute(HANDLE, WORD);
__declspec(dllimport) BOOL GetConsoleCursorInfo(HANDLE, CONSOLE_CURSOR_INFO *);
__declspec(dllimport) BOOL SetConsoleCursorInfo(HANDLE,
                                                const CONSOLE_CURSOR_INFO *);

static DWORD text_length(const char *text)
{
    DWORD length = 0;
    while (text[length]) length++;
    return length;
}

static BOOL say(HANDLE output, const char *text)
{
    DWORD written = 0;
    DWORD length = text_length(text);
    return WriteFile(output, text, length, &written, 0) && written == length;
}

int main(void)
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    CONSOLE_SCREEN_BUFFER_INFO screen;
    CONSOLE_CURSOR_INFO cursor;
    if (!GetConsoleMode(input, &mode) || !SetConsoleMode(input, mode) ||
        !GetConsoleScreenBufferInfo(output, &screen) ||
        !GetConsoleCursorInfo(output, &cursor)) {
        say(output, "[console] API regression failed\r\n");
        return 1;
    }
    CONSOLE_CURSOR_INFO changed = cursor;
    changed.Size = 15;
    changed.Visible = 1;
    if (!SetConsoleCursorInfo(output, &changed) ||
        !SetConsoleTextAttribute(output, 0x0a) ||
        !say(output, "[console] green text, modes and cursor API OK\r\n") ||
        !SetConsoleTextAttribute(output, screen.Attributes) ||
        !SetConsoleCursorInfo(output, &cursor))
        return 2;
    say(output, "CONSOLE_TEST: PASS\r\n");
    return 0;
}
