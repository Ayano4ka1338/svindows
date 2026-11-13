/* Native console transport shared by conhost, kernel32 and the I/O manager. */
#ifndef SVINDOWS_CONSOLE_HOST_ABI_H
#define SVINDOWS_CONSOLE_HOST_ABI_H
#define SV_CONSOLE_ALLOCATE 0u
#define SV_CONSOLE_FREE 1u
#define SV_CONSOLE_ATTACH 2u
#define SV_CONSOLE_OPEN_INPUT 3u
#define SV_CONSOLE_OPEN_OUTPUT 4u
#define SV_CONSOLE_CONNECT_HOST 5u
#define SV_CONSOLE_GET_WINDOW 6u
#define SV_CONSOLE_GET_PROCESS_LIST 7u
#define SV_CONSOLE_REGISTER_CONTROL 8u
#define SV_CONSOLE_GENERATE_CONTROL 9u
#define SV_CONSOLE_CREATE_BUFFER 10u
#define SV_CONSOLE_ACTIVATE_BUFFER 11u
#define SV_CONSOLE_HOST_CLOSE 12u
#define SV_CONSOLE_HOST_STATE 13u
#define SV_CONSOLE_HOST_INPUT 14u
#define SV_CONSOLE_HOST_RESIZE 15u
#define SV_CONSOLE_HOST_SCROLL 16u
#define SV_CONSOLE_SET_STANDARD_HANDLE 17u
#define SV_CONSOLE_GET_CONTROL_STATE 18u

#define SV_CONSOLE_CONTROL_READ_WIDE 14u
#define SV_CONSOLE_CONTROL_WRITE_WIDE 15u
#define SV_CONSOLE_CONTROL_RESIZE_BUFFER 16u
#define SV_CONSOLE_CONTROL_SET_WINDOW 17u
#define SV_CONSOLE_CONTROL_FLUSH_INPUT 18u
#define SV_CONSOLE_CONTROL_READ_INPUT 19u
#define SV_CONSOLE_CONTROL_PEEK_INPUT 20u
#define SV_CONSOLE_CONTROL_COUNT_INPUT 21u
#define SV_CONSOLE_CONTROL_READ_OUTPUT 22u
#define SV_CONSOLE_CONTROL_WRITE_OUTPUT 23u
#define SV_CONSOLE_CONTROL_GET_INPUT_CP 24u
#define SV_CONSOLE_CONTROL_SET_INPUT_CP 25u
#define SV_CONSOLE_CONTROL_GET_OUTPUT_CP 26u
#define SV_CONSOLE_CONTROL_SET_OUTPUT_CP 27u
#define SV_CONSOLE_CONTROL_GET_TITLE 28u
#define SV_CONSOLE_CONTROL_SET_TITLE 29u
#define SV_CONSOLE_CONTROL_WRITE_INPUT 30u
#define SV_CONSOLE_CONTROL_SCROLL_BUFFER 31u
#define SV_CONSOLE_CONTROL_GET_INFO_EX 32u
#define SV_CONSOLE_CONTROL_SET_INFO_EX 33u
#define SV_CONSOLE_CONTROL_FILL_WIDE 34u

typedef struct _SV_CONSOLE_CELL {
    unsigned short Character, Attribute;
} SV_CONSOLE_CELL;
typedef struct _SV_CONSOLE_RECT {
    short Left, Top, Right, Bottom;
} SV_CONSOLE_RECT;
typedef struct _SV_CONSOLE_INPUT_RECORD {
    unsigned short EventType, Reserved;
    union {
        struct {
            unsigned int KeyDown;
            unsigned short Repeat, VirtualKey, ScanCode, Character;
            unsigned int ControlState;
        } Key;
        struct {
            short X, Y;
            unsigned int Buttons, ControlState, Flags;
        } Mouse;
        struct {
            short Width, Height;
        } Size;
        unsigned int Focus;
        unsigned char Bytes[16];
    } Event;
} SV_CONSOLE_INPUT_RECORD;
typedef struct _SV_CONSOLE_BUFFER_TRANSFER {
    unsigned long long Cells;
    short Width, Height, X, Y;
    SV_CONSOLE_RECT Region;
} SV_CONSOLE_BUFFER_TRANSFER;
typedef struct _SV_CONSOLE_HOST_VIEW {
    unsigned int Size, Width, Height, CursorX, CursorY, CursorSize, CursorVisible;
    unsigned int InputMode, OutputMode, InputCodepage, OutputCodepage, ProcessCount;
    SV_CONSOLE_RECT Window;
    unsigned short Attribute, Reserved;
    unsigned long long Revision;
    unsigned int TitleLength, HostAttached;
    unsigned int ColorTable[16];
} SV_CONSOLE_HOST_VIEW;
typedef struct _SV_CONSOLE_HOST_SNAPSHOT {
    unsigned long long View, Cells, Title;
    unsigned long long CellCapacity, TitleCapacity;
} SV_CONSOLE_HOST_SNAPSHOT;
typedef struct _SV_CONSOLE_SCREEN_INFO_EX {
    unsigned int Size;
    short Width, Height, CursorX, CursorY;
    unsigned short Attribute;
    SV_CONSOLE_RECT Window;
    short MaximumWidth, MaximumHeight;
    unsigned short PopupAttribute;
    unsigned int FullscreenSupported;
    unsigned int ColorTable[16];
} SV_CONSOLE_SCREEN_INFO_EX;
typedef struct _SV_CONSOLE_READ_CONTROL {
    unsigned int Size, InitialChars, WakeupMask, ControlKeyState;
} SV_CONSOLE_READ_CONTROL;
_Static_assert(sizeof(SV_CONSOLE_CELL) == 4, "CHAR_INFO console cell ABI");
_Static_assert(sizeof(SV_CONSOLE_INPUT_RECORD) == 20, "INPUT_RECORD console ABI");
#endif
