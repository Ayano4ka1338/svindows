/* Native console renderer. Applications use executive screen/input objects;
 * this GUI process owns only presentation, selection and hardware input. */
#include "../../../kernel/include/abi/ConsoleHost.h"
#include "../explorer/Win32.h"

__declspec(dllimport) LONG SvNtConsoleManage(UINT, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR *);
__declspec(dllimport) LONG SvNtConsoleHost(UINT, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR *);
__declspec(dllimport) HMODULE GetModuleHandleW(const WCHAR *);
__declspec(dllimport) BOOL AttachConsole(DWORD);
__declspec(dllimport) int ShowCursor(BOOL);

static HWND host, scrollbar;
static HINSTANCE instance;
static HFONT font;
static int cell_width = 12, cell_height = 20;
static SV_CONSOLE_HOST_VIEW view;
static SV_CONSOLE_CELL *cells;
static SIZE_T cell_capacity, title_capacity;
static WCHAR *title;
static WCHAR *row_text;
static int *row_spacing;
static SIZE_T row_text_capacity, row_spacing_capacity;
static BOOL selecting, selected, closing;
static BOOL focused = TRUE;
static int selection_start, selection_end;
static DWORD close_started;
static BOOL cursor_phase;
static WORD pending_key, pending_scan;
static DWORD last_status;

static BOOL status_ok(LONG status) {
    if (status < 0) {
        last_status = (DWORD)status;
        return FALSE;
    }
    return TRUE;
}
static BOOL manage(UINT operation, UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR *result) {
    return status_ok(SvNtConsoleManage(operation, a, b, c, result));
}
static BOOL reserve(void **memory, SIZE_T *capacity, SIZE_T count, SIZE_T size) {
    if (count <= *capacity)
        return TRUE;
    if (size && count > ((SIZE_T)-1) / size)
        return FALSE;
    void *replacement = *memory ? HeapReAlloc(GetProcessHeap(), 0, *memory, count * size)
                                : HeapAlloc(GetProcessHeap(), 0, count * size);
    if (!replacement)
        return FALSE;
    *memory = replacement;
    *capacity = count;
    return TRUE;
}
static BOOL refresh(void) {
    SV_CONSOLE_HOST_SNAPSHOT request = {.View = (UINT_PTR)&view};
    LONG status = SvNtConsoleHost(SV_CONSOLE_HOST_STATE, (UINT_PTR)&request, 0, 0, 0);
    if (!status_ok(status))
        return FALSE;
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        SIZE_T cols = (SIZE_T)(view.Window.Right - view.Window.Left + 1),
               rows = (SIZE_T)(view.Window.Bottom - view.Window.Top + 1);
        if (!cols || !rows || cols > ((SIZE_T)-1) / rows)
            return FALSE;
        if (!reserve((void **)&cells, &cell_capacity, cols * rows, sizeof(*cells)) ||
            !reserve((void **)&title, &title_capacity, (SIZE_T)view.TitleLength + 1,
                     sizeof(*title)) ||
            !reserve((void **)&row_text, &row_text_capacity, cols, sizeof(*row_text)) ||
            !reserve((void **)&row_spacing, &row_spacing_capacity, cols, sizeof(*row_spacing)))
            return FALSE;
        request.Cells = (UINT_PTR)cells;
        request.CellCapacity = cell_capacity;
        request.Title = (UINT_PTR)title;
        request.TitleCapacity = title_capacity;
        status = SvNtConsoleHost(SV_CONSOLE_HOST_STATE, (UINT_PTR)&request, 0, 0, 0);
        if (status == (LONG)0xc0000023u)
            continue;
        if (!status_ok(status))
            return FALSE;
        SetWindowTextW(host, title);
        SHELL_SCROLL_INFO info = {.size = sizeof(info),
                                  .mask = 7,
                                  .minimum = 0,
                                  .maximum = (int)view.Height - 1,
                                  .page = (UINT)(view.Window.Bottom - view.Window.Top + 1),
                                  .position = view.Window.Top};
        SetScrollInfo(scrollbar, 2, &info, TRUE);
        return TRUE;
    }
    return FALSE;
}
static void fill(HDC dc, const RECT *rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    if (brush) {
        FillRect(dc, rect, brush);
        DeleteObject(brush);
    }
}
static BOOL selected_cell(int index) {
    int first = selection_start < selection_end ? selection_start : selection_end;
    int last = selection_start > selection_end ? selection_start : selection_end;
    return selected && index >= first && index <= last;
}
static void paint(void) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(host, &paint);
    if (!dc)
        return;
    RECT client;
    GetClientRect(host, &client);
    fill(dc, &client, 0);
    HGDIOBJ previous = font ? SelectObject(dc, font) : 0;
    SetBkMode(dc, TRANSPARENT);
    int cols = view.Window.Right - view.Window.Left + 1,
        rows = view.Window.Bottom - view.Window.Top + 1;
    if (cells && cols > 0 && rows > 0 && (SIZE_T)cols * rows <= cell_capacity) {
        for (int row = 0; row < rows; row++) {
            for (int col = 0; col < cols;) {
                int index = row * cols + col, first = col;
                WORD attribute = cells[index].Attribute;
                BOOL highlighted = selected_cell(index);
                while (col < cols && cells[row * cols + col].Attribute == attribute &&
                       selected_cell(row * cols + col) == highlighted) {
                    WCHAR character = cells[row * cols + col].Character;
                    row_text[col - first] = character ? character : ' ';
                    row_spacing[col - first] = cell_width;
                    col++;
                }
                COLORREF foreground = view.ColorTable[attribute & 15],
                         background = view.ColorTable[(attribute >> 4) & 15];
                if ((attribute & 0x4000) || highlighted) {
                    COLORREF swap = foreground;
                    foreground = background;
                    background = swap;
                }
                RECT rect = {first * cell_width, row * cell_height, col * cell_width,
                             (row + 1) * cell_height};
                if (background)
                    fill(dc, &rect, background);
                SetTextColor(dc, foreground);
                ExtTextOutW(dc, rect.left, rect.top + 2, 0, 0, row_text, (UINT)(col - first),
                            row_spacing);
            }
        }
        for (int row = 0; row < rows; row++)
            for (int col = 0; col < cols; col++) {
                int index = row * cols + col;
                SV_CONSOLE_CELL cell = cells[index];
                COLORREF foreground = view.ColorTable[cell.Attribute & 15],
                         background = view.ColorTable[(cell.Attribute >> 4) & 15];
                if ((cell.Attribute & 0x4000) || selected_cell(index)) {
                    COLORREF swap = foreground;
                    foreground = background;
                    background = swap;
                }
                RECT rect = {col * cell_width, row * cell_height, (col + 1) * cell_width,
                             (row + 1) * cell_height};
                if (cell.Attribute & 0x8000) {
                    RECT line = {rect.left, rect.bottom - 2, rect.right, rect.bottom - 1};
                    fill(dc, &line, foreground);
                }
                if (cell.Attribute & 0x400) {
                    RECT line = {rect.left, rect.top + cell_height / 2, rect.right,
                                 rect.top + cell_height / 2 + 1};
                    fill(dc, &line, foreground);
                }
                if (cell.Attribute & 0x800) {
                    RECT line = {rect.left, rect.top, rect.left + 1, rect.bottom};
                    fill(dc, &line, foreground);
                }
                if (cell.Attribute & 0x1000) {
                    RECT line = {rect.right - 1, rect.top, rect.right, rect.bottom};
                    fill(dc, &line, foreground);
                }
            }
        if (cursor_phase && view.CursorVisible && view.CursorX >= (UINT)view.Window.Left &&
            view.CursorX <= (UINT)view.Window.Right && view.CursorY >= (UINT)view.Window.Top &&
            view.CursorY <= (UINT)view.Window.Bottom && !selected) {
            int x = ((int)view.CursorX - view.Window.Left) * cell_width,
                y = ((int)view.CursorY - view.Window.Top) * cell_height;
            int height = (int)(cell_height * view.CursorSize / 100);
            if (height < 1)
                height = 1;
            RECT caret = {x, y + cell_height - height, x + cell_width, y + cell_height};
            fill(dc, &caret, view.ColorTable[view.Attribute & 15]);
        }
    }
    if (previous)
        SelectObject(dc, previous);
    EndPaint(host, &paint);
}
static DWORD control_state(void) {
    DWORD state = 0;
    if (GetKeyState(0x11) & 0x8000)
        state |= 8;
    if (GetKeyState(0x12) & 0x8000)
        state |= 2;
    if (GetKeyState(0x10) & 0x8000)
        state |= 16;
    if (GetKeyState(0x90) & 1)
        state |= 32;
    if (GetKeyState(0x91) & 1)
        state |= 64;
    if (GetKeyState(0x14) & 1)
        state |= 128;
    return state;
}
static BOOL input_record(const SV_CONSOLE_INPUT_RECORD *record) {
    return status_ok(SvNtConsoleHost(SV_CONSOLE_HOST_INPUT, (UINT_PTR)record, 1, 0, 0));
}
static void character(WCHAR character, WORD key, WORD scan, DWORD state) {
    SV_CONSOLE_INPUT_RECORD record = {.EventType = 1};
    record.Event.Key.KeyDown = TRUE;
    record.Event.Key.Repeat = 1;
    record.Event.Key.VirtualKey = key;
    record.Event.Key.ScanCode = scan;
    record.Event.Key.Character = character;
    record.Event.Key.ControlState = state;
    input_record(&record);
}
static int pointer_cell(LPARAM position) {
    int cols = view.Window.Right - view.Window.Left + 1,
        rows = view.Window.Bottom - view.Window.Top + 1;
    int x = (short)LOWORD(position) / cell_width, y = (short)HIWORD(position) / cell_height;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= cols)
        x = cols - 1;
    if (y >= rows)
        y = rows - 1;
    return y * cols + x;
}
static void copy_selection(void) {
    if (!selected || !cells)
        return;
    int cols = view.Window.Right - view.Window.Left + 1;
    int first = selection_start < selection_end ? selection_start : selection_end,
        last = selection_start > selection_end ? selection_start : selection_end;
    if (first < 0 || last < first || (SIZE_T)last >= cell_capacity || cols <= 0)
        return;
    SIZE_T capacity = (SIZE_T)(last - first + 1) + (SIZE_T)(last / cols - first / cols) * 2 + 1;
    HANDLE memory = GlobalAlloc(GMEM_MOVEABLE, capacity * sizeof(WCHAR));
    if (!memory)
        return;
    WCHAR *text = GlobalLock(memory);
    if (!text) {
        GlobalFree(memory);
        return;
    }
    SIZE_T used = 0;
    for (int index = first; index <= last; index++) {
        if (index > first && index % cols == 0) {
            while (used && text[used - 1] == ' ')
                used--;
            text[used++] = '\r';
            text[used++] = '\n';
        }
        text[used++] = cells[index].Character ? cells[index].Character : ' ';
    }
    text[used] = 0;
    GlobalUnlock(memory);
    if (!OpenClipboard(host)) {
        GlobalFree(memory);
        return;
    }
    BOOL transferred = EmptyClipboard() && SetClipboardData(13, memory) != 0;
    CloseClipboard();
    if (!transferred)
        GlobalFree(memory);
    selected = FALSE;
    InvalidateRect(host, 0, FALSE);
}
static void paste(void) {
    if (!OpenClipboard(host))
        return;
    HANDLE memory = GetClipboardData(13);
    WCHAR *text = memory ? GlobalLock(memory) : 0;
    SIZE_T count = memory ? GlobalSize(memory) / sizeof(WCHAR) : 0;
    if (text) {
        for (SIZE_T i = 0; i < count && text[i]; i++) {
            WCHAR ch = text[i];
            if (ch == '\r' && i + 1 < count && text[i + 1] == '\n')
                i++;
            if (ch == '\n')
                ch = '\r';
            character(ch, ch == '\r' ? 0x0d : 0, 0, 0);
        }
        GlobalUnlock(memory);
    }
    CloseClipboard();
}
static void scroll_to(int top) {
    if (status_ok(SvNtConsoleHost(SV_CONSOLE_HOST_SCROLL, (UINT_PTR)top, 0, 0, 0))) {
        selected = FALSE;
        refresh();
        InvalidateRect(host, 0, FALSE);
    }
}
static void resize(void) {
    RECT rect;
    GetClientRect(host, &rect);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    int bar_width = GetSystemMetrics(SM_CXVSCROLL);
    if (bar_width < 12)
        bar_width = 16;
    MoveWindow(scrollbar, width - bar_width, 0, bar_width, height, TRUE);
    int cols = (width - bar_width) / cell_width, rows = height / cell_height;
    if (cols < 1 || rows < 1)
        return;
    if (status_ok(
            SvNtConsoleHost(SV_CONSOLE_HOST_RESIZE, (UINT)(WORD)cols | ((UINT)(WORD)rows << 16),
                            (UINT)(WORD)cell_width | ((UINT)(WORD)cell_height << 16), 0, 0))) {
        selected = FALSE;
        refresh();
        InvalidateRect(host, 0, FALSE);
    }
}
static void initialize_font(void) {
    LOGFONTW info = {0};
    info.lfHeight = 16;
    info.lfWeight = 400;
    info.lfPitchAndFamily = 1;
    const WCHAR face[] = L"DejaVu Sans Mono";
    for (UINT i = 0; i < sizeof(face) / sizeof(*face) && i < 32; i++)
        info.lfFaceName[i] = face[i];
    font = CreateFontIndirectW(&info);
    HDC dc = GetDC(host);
    if (!dc)
        return;
    HGDIOBJ old = font ? SelectObject(dc, font) : 0;
    TEXTMETRICW metrics;
    if (GetTextMetricsW(dc, &metrics)) {
        if (metrics.tmMaxCharWidth > 0)
            cell_width = (int)metrics.tmMaxCharWidth;
        if (metrics.tmHeight > 0)
            cell_height = (int)metrics.tmHeight + 4;
    }
    if (cell_width < 6)
        cell_width = 6;
    if (cell_height < 10)
        cell_height = 10;
    if (old)
        SelectObject(dc, old);
    ReleaseDC(host, dc);
}
static LRESULT window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        host = window;
        scrollbar = CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | WS_VISIBLE | 1, 0, 0, 16, 100,
                                    window, (HMENU)(UINT_PTR)100, instance, 0);
        if (!scrollbar)
            return -1;
        initialize_font();
        if (!manage(SV_CONSOLE_CONNECT_HOST, (UINT_PTR)window,
                    (UINT)(WORD)cell_width | ((UINT)(WORD)cell_height << 16), 0, 0))
            return -1;
        SetTimer(window, 1, 30, 0);
        SetFocus(window);
        refresh();
        return 0;
    case WM_SIZE:
        if (scrollbar)
            resize();
        return 0;
    case WM_PAINT:
        paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER: {
        UINT_PTR old = view.Revision;
        BOOL old_phase = cursor_phase;
        cursor_phase = focused && ((GetTickCount() / 500) & 1) == 0;
        if (!refresh()) {
            if (!closing)
                SetWindowTextW(window, L"Console connection failed");
            return 0;
        }
        if (!view.ProcessCount) {
            DestroyWindow(window);
            return 0;
        }
        if (closing && GetTickCount() - close_started > 5000) {
            if (manage(SV_CONSOLE_HOST_CLOSE, 1, 0, 0, 0))
                DestroyWindow(window);
            return 0;
        }
        if (old != view.Revision || old_phase != cursor_phase)
            InvalidateRect(window, 0, FALSE);
        return 0;
    }
    case WM_KEYDOWN: {
        DWORD state = control_state();
        WORD key = (WORD)wp, scan = (WORD)((UINT_PTR)lp >> 16);
        if ((state & 8) && key == 'C' && selected) {
            copy_selection();
            return 0;
        }
        if (((state & 24) == 24 && key == 'V') || ((state & 16) && key == 0x2d)) {
            paste();
            return 0;
        }
        if ((state & 16) && (key == 0x21 || key == 0x22)) {
            int rows = view.Window.Bottom - view.Window.Top + 1;
            scroll_to(view.Window.Top + (key == 0x21 ? -rows : rows));
            return 0;
        }
        selected = FALSE;
        pending_key = key;
        pending_scan = scan;
        if ((key >= 0x21 && key <= 0x2f) || (key >= 0x70 && key <= 0x87))
            character(0, key, scan, state);
        InvalidateRect(window, 0, FALSE);
        return 0;
    }
    case WM_CHAR:
        character((WCHAR)wp, pending_key, pending_scan, control_state());
        return 0;
    case WM_KEYUP: {
        SV_CONSOLE_INPUT_RECORD record = {.EventType = 1};
        record.Event.Key.KeyDown = FALSE;
        record.Event.Key.Repeat = 1;
        record.Event.Key.VirtualKey = (WORD)wp;
        record.Event.Key.ScanCode = (WORD)((UINT_PTR)lp >> 16);
        record.Event.Key.ControlState = control_state();
        input_record(&record);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (view.InputMode & 0x40u) {
            selected = selecting = TRUE;
            selection_start = selection_end = pointer_cell(lp);
            SetCapture(window);
            InvalidateRect(window, 0, FALSE);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (selecting) {
            selection_end = pointer_cell(lp);
            InvalidateRect(window, 0, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (selecting) {
            selecting = FALSE;
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_RBUTTONDOWN:
        if (view.InputMode & 0x40u) {
            if (selected)
                copy_selection();
            else
                paste();
            return 0;
        }
        break;
    case WM_VSCROLL: {
        SetFocus(window);
        SHELL_SCROLL_INFO info = {.size = sizeof(info), .mask = 0x10};
        GetScrollInfo(scrollbar, 2, &info);
        int top = view.Window.Top, page = view.Window.Bottom - view.Window.Top + 1;
        switch (LOWORD(wp)) {
        case 0:
            top--;
            break;
        case 1:
            top++;
            break;
        case 2:
            top -= page;
            break;
        case 3:
            top += page;
            break;
        case 4:
        case 5:
            top = info.track;
            break;
        case 6:
            top = 0;
            break;
        case 7:
            top = (int)view.Height - page;
            break;
        default:
            return 0;
        }
        scroll_to(top);
        return 0;
    }
    case WM_MOUSEWHEEL:
        scroll_to(view.Window.Top - (short)HIWORD(wp) / 120 * 3);
        return 0;
    case WM_SETFOCUS:
        focused = TRUE;
        cursor_phase = TRUE;
        InvalidateRect(window, 0, FALSE);
        return 0;
    case WM_KILLFOCUS:
        focused = FALSE;
        cursor_phase = FALSE;
        InvalidateRect(window, 0, FALSE);
        return 0;
    case WM_CLOSE:
        if (!closing) {
            closing = TRUE;
            close_started = GetTickCount();
            manage(SV_CONSOLE_HOST_CLOSE, 0, 0, 0, 0);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        PostQuitMessage(0);
        return 0;
    }
    if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_RBUTTONDOWN ||
         message == WM_RBUTTONUP || message == WM_MOUSEMOVE) &&
        (view.InputMode & 0x10u)) {
        SV_CONSOLE_INPUT_RECORD record = {.EventType = 2};
        int index = pointer_cell(lp), cols = view.Window.Right - view.Window.Left + 1;
        record.Event.Mouse.X = (short)(view.Window.Left + index % cols);
        record.Event.Mouse.Y = (short)(view.Window.Top + index / cols);
        record.Event.Mouse.Buttons = (UINT)wp & 3u;
        record.Event.Mouse.ControlState = control_state();
        record.Event.Mouse.Flags = message == WM_MOUSEMOVE ? 1 : 0;
        input_record(&record);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
static int run(void) {
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    DWORD target = 0;
    if (argc == 3 && argv && CompareStringOrdinal(argv[1], -1, L"--attach", -1, FALSE) == 2) {
        for (SIZE_T i = 0; argv[2][i]; i++) {
            if (argv[2][i] < '0' || argv[2][i] > '9' || target > 0xffffffffu / 10) {
                target = 0;
                break;
            }
            target = target * 10 + argv[2][i] - '0';
        }
    }
    if (argv)
        LocalFree(argv);
    if (!target)
        return 87;
    UINT_PTR count = 0;
    LONG status = SvNtConsoleManage(SV_CONSOLE_GET_PROCESS_LIST, 0, 0, 0, &count);
    if (status < 0 && !AttachConsole(target))
        return 6;
    instance = GetModuleHandleW(0);
    WNDCLASSEXW cls = {.cbSize = sizeof(cls),
                       .lpfnWndProc = window_proc,
                       .hInstance = instance,
                       .hCursor = LoadCursorW(0, IDC_IBEAM),
                       .lpszClassName = L"ConsoleWindowClass"};
    if (!RegisterClassExW(&cls))
        return 8;
    HWND window =
        CreateWindowExW(0, cls.lpszClassName, L"Console", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        CW_USEDEFAULT, CW_USEDEFAULT, 1010, 570, 0, 0, instance, 0);
    if (!window)
        return 8;
    int columns = view.Window.Right - view.Window.Left + 1,
        rows = view.Window.Bottom - view.Window.Top + 1;
    int width =
        columns * cell_width + GetSystemMetrics(SM_CXVSCROLL) + GetSystemMetrics(SM_CXFRAME) * 2;
    int height =
        rows * cell_height + GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME) * 2;
    SetWindowPos(window, 0, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message;
    while (GetMessageW(&message, 0, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (font)
        DeleteObject(font);
    if (cells)
        HeapFree(GetProcessHeap(), 0, cells);
    if (title)
        HeapFree(GetProcessHeap(), 0, title);
    if (row_text)
        HeapFree(GetProcessHeap(), 0, row_text);
    if (row_spacing)
        HeapFree(GetProcessHeap(), 0, row_spacing);
    return (int)message.wParam;
}
void WinMainCRTStartup(void) { ExitProcess((DWORD)run()); }
