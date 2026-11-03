/*
 * svindows/Console.h - character console subsystem.
 *
 * The console owns terminal state.  HAL only renders cells to concrete
 * devices, while the File object layer supplies process-visible handles.
 */
#ifndef _SVINDOWS_CONSOLE_H_
#define _SVINDOWS_CONSOLE_H_

#include <nt/Ntdef.h>

#define CON_INPUT_PROCESSED 0x0001u
#define CON_INPUT_LINE      0x0002u
#define CON_INPUT_ECHO      0x0004u

#define CON_OUTPUT_PROCESSED 0x0001u
#define CON_OUTPUT_WRAP      0x0002u

typedef struct _CONSOLE_CELL {
    UINT8 Character;
    UINT8 Attribute;
} CONSOLE_CELL;

typedef struct _CONSOLE_SCREEN_INFO {
    INT16 Width;
    INT16 Height;
    INT16 CursorX;
    INT16 CursorY;
    UINT16 Attribute;
    INT16 WindowLeft;
    INT16 WindowTop;
    INT16 WindowRight;
    INT16 WindowBottom;
    INT16 MaximumWidth;
    INT16 MaximumHeight;
} CONSOLE_SCREEN_INFO;

typedef struct _CONSOLE_CURSOR_INFO {
    UINT32 Size;
    UINT32 Visible;
} CONSOLE_CURSOR_INFO;

enum {
    ConsoleControlGetMode = 1,
    ConsoleControlSetMode,
    ConsoleControlGetScreenInfo,
    ConsoleControlSetAttribute,
    ConsoleControlSetCursorPosition,
    ConsoleControlGetCursorInfo,
    ConsoleControlSetCursorInfo,
    ConsoleControlFillCharacter,
    ConsoleControlFillAttribute,
    ConsoleControlSetInputMode,
    ConsoleControlSetForegroundProcess,
    ConsoleControlRedraw,
    ConsoleControlStreamControl,
};

void ConInitialize(void);
void ConRedraw(void);
void ConQueueInputCharacter(char character);
void ConFlushInput(void);
SIZE_T ConRead(void *buffer, SIZE_T length);
SIZE_T ConTryRead(void *buffer, SIZE_T length);
BOOLEAN ConInputAvailable(void);
SIZE_T ConWrite(const void *buffer, SIZE_T length);
/* Append kernel boot text to the process-visible screen buffer without
 * sending it to the serial stream a second time. */
void ConAppendKernelText(const char *text, SIZE_T length);
void ConAppendKernelTextWithAttribute(const char *text, SIZE_T length,
                                      UINT8 attribute);
NTSTATUS ConControl(UINT32 operation, UINT64 argument1, UINT64 argument2,
                    UINT64 argument3, UINT64 *result);
void ConUpdateCursor(UINT64 tick);

#endif /* _SVINDOWS_CONSOLE_H_ */
