/* fetch.exe - a small fastfetch-style system summary for Svindows. */
typedef void *HANDLE;
typedef void *LPVOID;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
typedef long NTSTATUS;
typedef int BOOL;
typedef unsigned short WCHAR;

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STATUS_SUCCESS 0

typedef struct _SYSTEM_BASIC_INFORMATION_LOCAL {
    DWORD Reserved;
    DWORD TimerResolution;
    DWORD PageSize;
    DWORD NumberOfPhysicalPages;
    DWORD LowestPhysicalPageNumber;
    DWORD HighestPhysicalPageNumber;
    DWORD AllocationGranularity;
    DWORD Padding;
    ULONGLONG MinimumUserModeAddress;
    ULONGLONG MaximumUserModeAddress;
    ULONGLONG ActiveProcessorsAffinityMask;
    BYTE NumberOfProcessors;
    BYTE Reserved2[7];
} SYSTEM_BASIC_INFORMATION_LOCAL;

typedef struct _OSVERSIONINFO_LOCAL {
    DWORD Size;
    DWORD Major;
    DWORD Minor;
    DWORD Build;
    DWORD Platform;
    WCHAR ServicePack[128];
} OSVERSIONINFO_LOCAL;

__declspec(dllimport) HANDLE GetStdHandle(DWORD);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) BOOL SetConsoleTextAttribute(HANDLE, WORD);
__declspec(dllimport) ULONGLONG GetTickCount64(void);
__declspec(dllimport) BOOL GetVersionExW(void *);
__declspec(dllimport) BOOL GetComputerNameW(WCHAR *, DWORD *);
__declspec(dllimport) DWORD GetCurrentDirectoryW(DWORD, WCHAR *);
__declspec(dllimport) NTSTATUS NtQuerySystemInformation(
    DWORD, void *, DWORD, DWORD *);
void FetchCpuInfo(char vendor[13], char brand[49]);

static HANDLE output;

static DWORD length(const char *s)
{
    DWORD n = 0;
    while (s[n]) n++;
    return n;
}

static void write_text(const char *s)
{
    DWORD ignored;
    WriteFile(output, s, length(s), &ignored, 0);
}

static char *append(char *out, const char *s)
{
    while (*s) *out++ = *s++;
    *out = 0;
    return out;
}

static char *append_u64(char *out, ULONGLONG value)
{
    char digits[24];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count) *out++ = digits[--count];
    *out = 0;
    return out;
}

static void wide_to_ascii(char *out, DWORD capacity, const WCHAR *in)
{
    DWORD i = 0;
    if (!capacity) return;
    while (in[i] && i + 1 < capacity) {
        WCHAR ch = in[i];
        out[i] = ch < 128 ? (char)ch : '?';
        i++;
    }
    out[i] = 0;
}

static void trim_cpu_brand(char *brand)
{
    unsigned source = 0, target = 0;
    while (brand[source] == ' ') source++;
    for (; brand[source]; source++) {
        if (brand[source] == ' ' &&
            (target == 0 || brand[target - 1] == ' '))
            continue;
        brand[target++] = brand[source];
    }
    while (target && brand[target - 1] == ' ') target--;
    brand[target] = 0;
}

static void info_line(const char *logo, const char *label, const char *value)
{
    DWORD logo_length = length(logo);
    SetConsoleTextAttribute(output, 0x0b);
    write_text(logo);
    SetConsoleTextAttribute(output, 0x07);
    while (logo_length++ < 21) write_text(" ");
    write_text("  ");
    if (label && *label) {
        DWORD label_length = length(label);
        SetConsoleTextAttribute(output, 0x0e);
        write_text(label);
        SetConsoleTextAttribute(output, 0x07);
        while (label_length++ < 9) write_text(" ");
        write_text(": ");
        write_text(value);
    } else {
        write_text(value);
    }
    write_text("\r\n");
}

int main(void)
{
    char vendor[13], brand[49];
    char computer[32] = "SVINDOWS";
    char identity[48] = "user@SVINDOWS";
    char directory[260] = "C:\\";
    char version[64], uptime[64], memory[64], cpu[72];
    WCHAR wide[260];
    DWORD wide_size = 260;
    OSVERSIONINFO_LOCAL os;
    SYSTEM_BASIC_INFORMATION_LOCAL basic;
    DWORD returned = 0;

    output = GetStdHandle(STD_OUTPUT_HANDLE);
    FetchCpuInfo(vendor, brand);
    trim_cpu_brand(brand);
    if (!brand[0]) {
        char *p = cpu;
        p = append(p, vendor);
        append(p, " compatible CPU");
    } else {
        char *p = cpu;
        unsigned i = 0;
        while (brand[i] && i + 1 < sizeof(cpu)) *p++ = brand[i++];
        *p = 0;
    }

    if (GetComputerNameW(wide, &wide_size))
        wide_to_ascii(computer, sizeof(computer), wide);
    {
        char *p = identity;
        p = append(p, "user@");
        append(p, computer);
    }
    if (GetCurrentDirectoryW(260, wide))
        wide_to_ascii(directory, sizeof(directory), wide);

    os.Size = sizeof(os);
    if (GetVersionExW(&os)) {
        char *p = version;
        p = append(p, "NT ");
        p = append_u64(p, os.Major);
        *p++ = '.'; *p = 0;
        p = append_u64(p, os.Minor);
        p = append(p, " (build ");
        p = append_u64(p, os.Build);
        append(p, ")");
    } else {
        append(version, "NT-compatible x86_64");
    }

    {
        ULONGLONG seconds = GetTickCount64() / 1000;
        ULONGLONG days = seconds / 86400;
        ULONGLONG hours = (seconds / 3600) % 24;
        ULONGLONG minutes = (seconds / 60) % 60;
        char *p = uptime;
        if (days) { p = append_u64(p, days); p = append(p, "d "); }
        p = append_u64(p, hours); p = append(p, "h ");
        p = append_u64(p, minutes); append(p, "m");
    }

    if (NtQuerySystemInformation(0, &basic, sizeof(basic), &returned) ==
        STATUS_SUCCESS) {
        ULONGLONG mib =
            ((ULONGLONG)basic.NumberOfPhysicalPages * basic.PageSize) >> 20;
        char *p = memory;
        p = append_u64(p, mib);
        append(p, " MiB physical");
    } else {
        append(memory, "unknown");
    }

    write_text("\r\n");
    info_line("      /\\", "", identity);
    info_line("     /**\\", "OS", "Svindows 0.6.0 x86_64");
    info_line("    /****\\   /\\", "Kernel", version);
    info_line("   /      \\ /**\\", "Uptime", uptime);
    info_line("  /  /\\    /    \\", "Shell", "cmd.exe");
    info_line(" /  /  \\  /      \\", "CPU", cpu);
    info_line("/  /    \\/ /\\     \\", "Memory", memory);
    info_line("", "Disk", "FAT32 (C:)");
    info_line("", "Directory", directory);
    SetConsoleTextAttribute(output, 0x07);
    return 0;
}
