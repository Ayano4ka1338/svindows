#ifndef SVINDOWS_EXPLORER_COMMON_H
#define SVINDOWS_EXPLORER_COMMON_H
#include "Win32.h"

#define SHELL_PATH_CAPACITY 260u
extern HINSTANCE ShellInstance;
extern WCHAR ShellDesktopPath[SHELL_PATH_CAPACITY];
extern WCHAR ShellDocumentsPath[SHELL_PATH_CAPACITY];
extern WCHAR ShellExecutablePath[SHELL_PATH_CAPACITY];
extern BOOL ShellStandalone;

SIZE_T ShellLength(const WCHAR *text);
int ShellCompare(const WCHAR *first, const WCHAR *second);
BOOL ShellCopy(WCHAR *out, SIZE_T capacity, const WCHAR *text);
BOOL ShellJoin(WCHAR *out, SIZE_T capacity, const WCHAR *directory, const WCHAR *leaf);
BOOL ShellNormalizePath(const WCHAR *input, const WCHAR *directory,
    WCHAR *output, SIZE_T capacity);
BOOL ShellPathWithin(const WCHAR *path, const WCHAR *directory);
BOOL ShellValidName(const WCHAR *name);
const WCHAR *ShellLeaf(const WCHAR *path);
const WCHAR *ShellExtension(const WCHAR *path);
WCHAR *ShellDuplicate(const WCHAR *text);
BOOL ShellReserve(void **memory, SIZE_T *capacity, SIZE_T wanted, SIZE_T element_size);
void *ShellAlloc(SIZE_T bytes);
void ShellFree(void *memory);
void ShellZero(void *memory, SIZE_T bytes);
void ShellUnsigned(WCHAR *out, SIZE_T capacity, ULONGLONG value);
void ShellLog(const char *message);
void ShellLogPath(const char *message, const WCHAR *path);
void ShellSetStatus(HWND control, const WCHAR *text);
HWND ShellButton(HWND parent, UINT id, const WCHAR *caption);
HWND ShellEdit(HWND parent, UINT id, const WCHAR *text);
BOOL ShellLaunch(HWND owner, const WCHAR *path, const WCHAR *arguments);
BOOL ShellRunCommand(HWND owner, const WCHAR *command);
BOOL ShellOpenPath(HWND owner, const WCHAR *path);
void ShellError(HWND owner, const WCHAR *action, const WCHAR *path, DWORD error);
BOOL ShellConfirm(HWND owner, const WCHAR *caption, const WCHAR *text);
BOOL ShellPrompt(HWND owner, const WCHAR *caption, const WCHAR *label,
    WCHAR *value, SIZE_T capacity);
BOOL ShellProcessDialogMessage(MSG *message);
void ShellRegisterDialogClass(void);
BOOL ShellReadShortcut(const WCHAR *path, WCHAR *target, SIZE_T capacity);
void ShellPaintFill(HDC dc, const RECT *rect, COLORREF color);
void ShellPaintIcon(HDC dc, int x, int y, int size, BOOL folder, BOOL application, BOOL shortcut);

HWND ShellOpenFolder(const WCHAR *path);
BOOL ShellBrowserMessage(MSG *message);
void ShellRefreshDesktop(void);
#endif
