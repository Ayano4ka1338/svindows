#ifndef SVINDOWS_EXPLORER_FILE_MANAGER_H
#define SVINDOWS_EXPLORER_FILE_MANAGER_H
#include "Common.h"
BOOL ShellRegisterBrowserClass(void);
BOOL ShellSelectFolderItems(HWND window,const WCHAR *const *paths,SIZE_T count,BOOL edit);
#endif
