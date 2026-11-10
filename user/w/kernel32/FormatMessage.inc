/* Format a caller-supplied message using Win64's eight-byte argument slots.
 * Resource/system message tables are handled separately when available. */
#define FORMAT_ALLOCATE 0x100u
#define FORMAT_IGNORE_INSERTS 0x200u
#define FORMAT_FROM_STRING 0x400u
#define FORMAT_ARGUMENT_ARRAY 0x2000u
#define MESSAGE_MAX_CHARS 65535u

typedef struct {
    WCHAR *Text;
    DWORD Length;
} MESSAGE_OUTPUT;

static BOOL message_append(MESSAGE_OUTPUT *out, WCHAR ch)
{
    if (out->Length >= MESSAGE_MAX_CHARS) return 0;
    if (out->Text) out->Text[out->Length] = ch;
    out->Length++;
    return 1;
}

static BOOL message_insert(MESSAGE_OUTPUT *out, ULONGLONG value,
                            const WCHAR *format, DWORD length)
{
    DWORD at = 0, width = 0;
    WCHAR padding = length && format[0] == '0' ? '0' : ' ';
    while (at < length && format[at] >= '0' && format[at] <= '9') {
        width = width * 10 + format[at++] - '0';
        if (width > MESSAGE_MAX_CHARS) return 0;
    }
    if (at < length && (format[at] == 'l' || format[at] == 'w')) at++;
    WCHAR kind = at < length ? format[at++] : 's';
    if (at != length) return 0;

    WCHAR digits[32];
    const WCHAR *text = 0;
    DWORD count = 0;
    BOOL negative = 0;
    if (kind == 's') {
        text = (const WCHAR *)value;
        if (!text) text = L"(null)";
        while (count < MESSAGE_MAX_CHARS && text[count]) count++;
    } else if (kind == 'd' || kind == 'i' || kind == 'u' || kind == 'x' || kind == 'X') {
        unsigned number = (unsigned)value;
        if ((kind == 'd' || kind == 'i') && (int)number < 0) {
            negative = 1;
            number = 0u - number;
        }
        unsigned base = kind == 'x' || kind == 'X' ? 16 : 10;
        do {
            unsigned digit = number % base;
            digits[count++] = (WCHAR)(digit < 10 ? '0' + digit :
                                      (kind == 'X' ? 'A' : 'a') + digit - 10);
            number /= base;
        } while (number);
    } else {
        return 0;
    }
    DWORD occupied = count + negative;
    if (negative && padding == '0') {
        if (!message_append(out, '-')) return 0;
        negative = 0;
    }
    while (width > occupied) {
        if (!message_append(out, padding)) return 0;
        width--;
    }
    if (negative && !message_append(out, '-')) return 0;
    if (text) {
        for (DWORD i = 0; i < count; i++)
            if (!message_append(out, text[i])) return 0;
    } else {
        while (count)
            if (!message_append(out, digits[--count])) return 0;
    }
    return 1;
}

static BOOL format_message_pass(DWORD flags, const WCHAR *source, void *arguments,
                                  MESSAGE_OUTPUT *out)
{
    const ULONGLONG *args = 0;
    if (arguments && !(flags & FORMAT_IGNORE_INSERTS))
        args = flags & FORMAT_ARGUMENT_ARRAY ? arguments : *(const ULONGLONG **)arguments;
    for (const WCHAR *p = source; *p; p++) {
        if (*p != '%' || (flags & FORMAT_IGNORE_INSERTS)) {
            if (!message_append(out, *p)) return 0;
            continue;
        }
        p++;
        if (!*p) return 0;
        if (*p == '0') return 1;
        if (*p >= '1' && *p <= '9') {
            unsigned index = *p - '0';
            if (p[1] >= '0' && p[1] <= '9') index = index * 10 + *++p - '0';
            const WCHAR *format = L"s";
            DWORD count = 1;
            if (p[1] == '!') {
                format = p + 2;
                p = format;
                while (*p && *p != '!') p++;
                if (!*p) return 0;
                count = (DWORD)(p - format);
            }
            if (!args || !message_insert(out, args[index - 1], format, count)) return 0;
            continue;
        }
        WCHAR ch = *p;
        if (ch == 'n') {
            if (!message_append(out, '\r')) return 0;
            ch = '\n';
        } else if (ch == 'r') ch = '\r';
        else if (ch == 't') ch = '\t';
        else if (ch == 'b') ch = ' ';
        else if (ch != '%') return 0;
        if (!message_append(out, ch)) return 0;
    }
    return 1;
}

__declspec(dllexport) DWORD FormatMessageW(DWORD flags, const void *source,
    DWORD message, DWORD language, WCHAR *buffer, DWORD size, void *arguments)
{
    (void)message;
    (void)language;
    if (flags & FORMAT_ALLOCATE) {
        if (buffer) *(WCHAR **)buffer = 0;
    } else if (buffer && size) buffer[0] = 0;
    if (!(flags & FORMAT_FROM_STRING)) {
        SetLastError(317); /* ERROR_MR_MID_NOT_FOUND */
        return 0;
    }
    if (!source || !buffer || (flags & 0xff) || (flags & 0x1800)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    MESSAGE_OUTPUT output = {0};
    if (!format_message_pass(flags, source, arguments, &output)) {
        SetLastError(87);
        return 0;
    }
    DWORD required = output.Length + 1;
    WCHAR *text = buffer;
    if (flags & FORMAT_ALLOCATE) {
        DWORD capacity = size > required ? size : required;
        if (capacity > MESSAGE_MAX_CHARS) {
            SetLastError(8);
            return 0;
        }
        text = LocalAlloc(0, (SIZE_T)capacity * sizeof(WCHAR));
        if (!text) return 0;
    } else if (size < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    output.Text = text;
    output.Length = 0;
    if (!format_message_pass(flags, source, arguments, &output)) {
        if (flags & FORMAT_ALLOCATE) LocalFree(text);
        SetLastError(87);
        return 0;
    }
    text[output.Length] = 0;
    if (flags & FORMAT_ALLOCATE) *(WCHAR **)buffer = text;
    return output.Length;
}
