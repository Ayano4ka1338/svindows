/* Bounded integer formatting and secure wide scanning (integer/text fields). */
__declspec(dllexport) int _ui64tow_s(unsigned long long value, WCHAR *out,
    SIZE_T capacity, int radix)
{
    if (!out || !capacity || radix < 2 || radix > 36) {
        if (out && capacity) out[0] = 0;
        g_errno = 22; return 22;
    }
    WCHAR digits[64];
    SIZE_T count = 0;
    do {
        unsigned digit = (unsigned)(value % (unsigned)radix);
        digits[count++] = (WCHAR)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value /= (unsigned)radix;
    } while (value);
    if (count >= capacity) { out[0] = 0; g_errno = 34; return 34; }
    for (SIZE_T i = 0; i < count; ++i) out[i] = digits[count - i - 1];
    out[count] = 0;
    return 0;
}

static int scan_wide_space(WCHAR c)
{ return c == ' ' || (c >= '\t' && c <= '\r'); }
static int scan_wide_digit(WCHAR c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

__declspec(dllexport) int swscanf_s(const WCHAR *input, const WCHAR *format, ...)
{
    if (!input || !format) { g_errno = 22; return -1; }
    __builtin_va_list args;
    __builtin_va_start(args, format);
    int assigned = 0, consumed = 0, input_failure = 0;
    while (*format) {
        if (scan_wide_space(*format)) {
            do { ++format; } while (scan_wide_space(*format));
            while (scan_wide_space(*input)) ++input;
            continue;
        }
        if (*format != '%' || format[1] == '%') {
            if (*format == '%') ++format;
            if (*input != *format) { input_failure = !*input; break; }
            ++format; ++input; consumed = 1; continue;
        }
        ++format;
        int suppress = *format == '*';
        if (suppress) ++format;
        unsigned width = 0;
        while (*format >= '0' && *format <= '9') {
            if (width > 10000000) { g_errno = 22; goto done; }
            width = width * 10 + *format++ - '0';
        }
        int length = 0; /* 0/int, 1/char, 2/short, 3/long32, 4/int64 */
        if (*format == 'h') { ++format; length = 2; if (*format == 'h') { ++format; length = 1; } }
        else if (*format == 'l') { ++format; length = 3; if (*format == 'l') { ++format; length = 4; } }
        else if (*format == 'I' && format[1] == '6' && format[2] == '4') { format += 3; length = 4; }
        else if (*format == 'z') { ++format; length = 4; }
        WCHAR spec = *format;
        if (!spec) { g_errno = 22; break; }
        ++format;
        if (spec != 'c') while (scan_wide_space(*input)) ++input;
        if (!*input) { input_failure = 1; break; }
        if (spec == 's' || spec == 'c') {
            if (length == 1 || length == 2) { g_errno = 22; break; }
            unsigned count = 0, limit = width ? width : spec == 'c' ? 1 : ~0u;
            while (count < limit && input[count] &&
                   (spec == 'c' || !scan_wide_space(input[count]))) ++count;
            if (!count || (spec == 'c' && count != limit)) { input_failure = 1; break; }
            if (!suppress) {
                WCHAR *out = __builtin_va_arg(args, WCHAR *);
                unsigned capacity = __builtin_va_arg(args, unsigned);
                if (!out || !capacity) { g_errno = 22; break; }
                if (count + (spec == 's') > capacity) {
                    out[0] = 0; g_errno = 34; break;
                }
                for (unsigned i = 0; i < count; ++i) out[i] = input[i];
                if (spec == 's') out[count] = 0;
                ++assigned;
            }
            input += count; consumed = 1; continue;
        }
        unsigned base = spec == 'o' ? 8 : spec == 'x' || spec == 'X' || spec == 'p' ? 16 : 10;
        if (spec != 'u' && spec != 'd' && spec != 'i' && spec != 'o' &&
            spec != 'x' && spec != 'X' && spec != 'p') {
            g_errno = 22; break; /* Unsupported conversions never consume arguments. */
        }
        const WCHAR *p = input;
        unsigned left = width ? width : ~0u;
        int negative = 0;
        if (left && (*p == '-' || *p == '+')) { negative = *p == '-'; ++p; --left; }
        if (spec == 'i') base = *p == '0' ? 8 : 10;
        if (left >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
            (spec == 'i' || base == 16) && left > 2 &&
            scan_wide_digit(p[2]) >= 0 && scan_wide_digit(p[2]) < 16) {
            base = 16; p += 2; left -= 2;
        }
        unsigned long long value = 0;
        unsigned digits = 0;
        for (; left; --left) {
            int digit = scan_wide_digit(*p);
            if (digit < 0 || (unsigned)digit >= base) break;
            if (value > (~0ull - (unsigned)digit) / base) { g_errno = 34; goto done; }
            value = value * base + (unsigned)digit;
            ++p; ++digits;
        }
        if (!digits) { input_failure = !*p; break; }
        if (negative) value = 0ull - value;
        if (!suppress) {
            void *out = __builtin_va_arg(args, void *);
            if (!out) { g_errno = 22; break; }
            if (spec == 'p' || length == 4) *(unsigned long long *)out = value;
            else if (length == 1) *(unsigned char *)out = (unsigned char)value;
            else if (length == 2) *(unsigned short *)out = (unsigned short)value;
            else *(unsigned int *)out = (unsigned int)value;
            ++assigned;
        }
        input = p; consumed = 1;
    }
done:
    __builtin_va_end(args);
    return !assigned && !consumed && input_failure ? -1 : assigned;
}
