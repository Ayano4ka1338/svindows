/* Process-local, read-only /proc nodes. They are never stored on the FAT
 * volume, even if a native program creates C:\proc. */
#include "Internal.h"
#include <svindows/Rtl.h>
#include <svindows/Mm.h>
#include <svindows/Ke.h>

#define PROC_O_CREAT 0x40u
#define PROC_O_TRUNC 0x200u
#define PROC_O_DIRECTORY 0x10000u
#define PROC_O_CLOEXEC 0x80000u
#define PROC_DT_DIR 4u
#define PROC_DT_REG 8u
#define PROC_DT_LNK 10u
#define PROC_CONTENT_CAPACITY 8192u

typedef struct _PROC_DIRENT64 {
    UINT64 Inode;
    INT64 NextOffset;
    UINT16 RecordLength;
    UINT8 Type;
    char Name[];
} PROC_DIRENT64;

static char proc_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static BOOLEAN proc_equal(const char *a, const char *b)
{
    while (*a && *b && proc_lower(*a) == proc_lower(*b)) { a++; b++; }
    return !*a && !*b;
}

UINT8 LinuxProcPathKind(const char *path)
{
    const char *root = "C:\\proc";
    UINT32 i = 0;
    while (root[i] && path[i] && proc_lower(root[i]) == proc_lower(path[i]))
        i++;
    if (root[i] || (path[i] && path[i] != '\\')) return LINUX_PROC_NONE;
    if (!path[i]) return LINUX_PROC_ROOT;
    if (proc_equal(path, "C:\\proc\\self")) return LINUX_PROC_SELF;
    if (proc_equal(path, "C:\\proc\\self\\status"))
        return LINUX_PROC_STATUS;
    if (proc_equal(path, "C:\\proc\\self\\cmdline"))
        return LINUX_PROC_CMDLINE;
    if (proc_equal(path, "C:\\proc\\self\\exe")) return LINUX_PROC_EXE;
    if (proc_equal(path, "C:\\proc\\self\\cwd")) return LINUX_PROC_CWD;
    if (proc_equal(path, "C:\\proc\\version")) return LINUX_PROC_VERSION;
    if (proc_equal(path, "C:\\proc\\meminfo")) return LINUX_PROC_MEMINFO;
    if (proc_equal(path, "C:\\proc\\cpuinfo")) return LINUX_PROC_CPUINFO;
    return LINUX_PROC_UNKNOWN;
}

UINT64 LinuxProcOpen(PKPROCESS process, UINT64 slot, const char *path,
                     UINT64 flags)
{
    UINT8 node = LinuxProcPathKind(path);
    if (node == LINUX_PROC_NONE || node == LINUX_PROC_UNKNOWN)
        return LinuxError(LINUX_ENOENT);
    if (flags & (PROC_O_CREAT | PROC_O_TRUNC)) return LinuxError(LINUX_EROFS);
    if (node == LINUX_PROC_ROOT || node == LINUX_PROC_SELF)
        return LinuxOpenDirectory(process, slot, path, flags);
    if (node == LINUX_PROC_EXE || node == LINUX_PROC_CWD)
        return LinuxError(LINUX_EOPNOTSUPP);
    if (flags & PROC_O_DIRECTORY) return LinuxError(LINUX_ENOTDIR);
    if (flags & 3u) return LinuxError(LINUX_EROFS);
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[slot];
    memset(entry, 0, sizeof(*entry));
    entry->Used = TRUE;
    entry->Kind = LINUX_FD_PROC;
    entry->ProcNode = node;
    entry->CloseOnExec = !!(flags & PROC_O_CLOEXEC);
    return slot;
}

static void proc_cpuid(UINT32 leaf, UINT32 subleaf,
                       UINT32 *a, UINT32 *b, UINT32 *c, UINT32 *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

static UINT64 proc_meminfo(char *out, SIZE_T capacity)
{
    UINT64 total = MmUsablePageCount() << PAGE_SHIFT;
    UINT64 free = MmFreePageCount() << PAGE_SHIFT;
    UINT64 total_kb = total >> 10;
    UINT64 free_kb = free >> 10;
    int n = RtlFormat(out, capacity,
        "MemTotal: %llu kB\nMemFree: %llu kB\n"
        "MemAvailable: %llu kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n",
        (unsigned long long)total_kb, (unsigned long long)free_kb,
        (unsigned long long)free_kb);
    return n < 0 ? 0 : ((UINT64)n < capacity ? (UINT64)n : capacity - 1);
}

static UINT64 proc_cpuinfo(char *out, SIZE_T capacity)
{
    UINT32 a, b, c, d;
    proc_cpuid(0, 0, &a, &b, &c, &d);
    UINT32 max_basic = a;
    char vendor[13];
    memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4); vendor[12] = 0;

    proc_cpuid(1, 0, &a, &b, &c, &d);
    UINT32 signature = a;
    UINT32 feature_ecx = c, feature_edx = d, cache_line = (b >> 8) & 0xFF;
    UINT32 logical_count = (b >> 16) & 0xFF;
    if (!logical_count) logical_count = 1;
    UINT32 core_count = 1;
    if (max_basic >= 4) {
        proc_cpuid(4, 0, &a, &b, &c, &d);
        if ((a & 0x1Fu) != 0) core_count = ((a >> 26) & 0x3Fu) + 1;
    }
    UINT32 stepping = signature & 0xF;
    UINT32 model = (signature >> 4) & 0xF;
    UINT32 family = (signature >> 8) & 0xF;
    UINT32 ext_model = (signature >> 16) & 0xF;
    UINT32 ext_family = (signature >> 20) & 0xFF;
    if (family == 0xF) family += ext_family;
    if (family == 6 || family == 0xF) model |= ext_model << 4;

    char brand[49]; memset(brand, 0, sizeof(brand));
    proc_cpuid(0x80000000u, 0, &a, &b, &c, &d);
    UINT32 max_extended = a;
    if (max_extended >= 0x80000004u) {
        UINT32 *words = (UINT32 *)brand;
        for (UINT32 leaf = 0; leaf < 3; leaf++) {
            proc_cpuid(0x80000002u + leaf, 0,
                       &words[leaf * 4], &words[leaf * 4 + 1],
                       &words[leaf * 4 + 2], &words[leaf * 4 + 3]);
        }
        for (int i = 47; i >= 0 && brand[i] == ' '; i--) brand[i] = 0;
    }
    if (!brand[0]) strcpy(brand, "Unknown x86_64 CPU");

    int n = RtlFormat(out, capacity,
        "processor\t: 0\nvendor_id\t: %s\ncpu family\t: %u\n"
        "model\t\t: %u\nmodel name\t: %s\nstepping\t: %u\n",
        vendor, family, model, brand, stepping);
    if (n < 0 || (SIZE_T)n >= capacity) return n < 0 ? 0 : capacity - 1;
    SIZE_T used = (SIZE_T)n;
    n = RtlFormat(out + used, capacity - used,
                  "cpu cores\t: %u\nsiblings\t: %u\n"
                  "fpu\t\t: %s\nwp\t\t: yes\n",
                  core_count, logical_count,
                  (feature_edx & (1u << 0)) ? "yes" : "no");
    if (n < 0 || (SIZE_T)n >= capacity - used) return n < 0 ? used : capacity - 1;
    used += (SIZE_T)n;
    n = RtlFormat(out + used, capacity - used,
                  "clflush size\t: %u\ncache_alignment\t: %u\n",
                  cache_line * 8, cache_line * 8);
    if (n < 0 || (SIZE_T)n >= capacity - used) return n < 0 ? used : capacity - 1;
    used += (SIZE_T)n;

    n = RtlFormat(out + used, capacity - used, "flags\t\t:");
    if (n < 0 || (SIZE_T)n >= capacity - used) return used;
    used += (SIZE_T)n;
#define CPU_FLAG(bit, name) do { if (feature_edx & (1u << (bit))) { n = RtlFormat(out + used, capacity - used, " %s", name); if (n < 0 || (SIZE_T)n >= capacity - used) return used; used += (SIZE_T)n; } } while (0)
    CPU_FLAG(0, "fpu"); CPU_FLAG(1, "vme"); CPU_FLAG(2, "de");
    CPU_FLAG(3, "pse"); CPU_FLAG(4, "tsc"); CPU_FLAG(5, "msr");
    CPU_FLAG(6, "pae"); CPU_FLAG(8, "cx8"); CPU_FLAG(9, "apic");
    CPU_FLAG(11, "sep"); CPU_FLAG(12, "mtrr"); CPU_FLAG(13, "pge");
    CPU_FLAG(14, "mca"); CPU_FLAG(15, "cmov"); CPU_FLAG(16, "pat");
    CPU_FLAG(19, "clflush"); CPU_FLAG(23, "mmx"); CPU_FLAG(24, "fxsr");
    CPU_FLAG(25, "sse"); CPU_FLAG(26, "sse2");
#undef CPU_FLAG
#define CPU_FLAG_C(bit, name) do { if (feature_ecx & (1u << (bit))) { n = RtlFormat(out + used, capacity - used, " %s", name); if (n < 0 || (SIZE_T)n >= capacity - used) return used; used += (SIZE_T)n; } } while (0)
    CPU_FLAG_C(0, "sse3"); CPU_FLAG_C(9, "ssse3");
    CPU_FLAG_C(19, "sse4_1"); CPU_FLAG_C(20, "sse4_2");
    CPU_FLAG_C(23, "popcnt"); CPU_FLAG_C(25, "aes");
    CPU_FLAG_C(26, "xsave"); CPU_FLAG_C(31, "hypervisor");
#undef CPU_FLAG_C
    BOOLEAN avx_state = FALSE;
    if ((feature_ecx & (1u << 27)) && (feature_ecx & (1u << 28))) {
        UINT32 xcr0_low, xcr0_high;
        __asm__ volatile("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0));
        avx_state = (xcr0_low & 0x6u) == 0x6u;
    }
    /* AVX is reported only when the kernel enabled its extended state. */
    if (avx_state) {
        n = RtlFormat(out + used, capacity - used, " avx");
        if (n > 0 && (SIZE_T)n < capacity - used) used += (SIZE_T)n;
    }
    if (max_basic >= 7) {
        proc_cpuid(7, 0, &a, &b, &c, &d);
#define CPU_FLAG_B(bit, name) do { if (b & (1u << (bit))) { n = RtlFormat(out + used, capacity - used, " %s", name); if (n < 0 || (SIZE_T)n >= capacity - used) return used; used += (SIZE_T)n; } } while (0)
        CPU_FLAG_B(3, "bmi1");
        if (avx_state) CPU_FLAG_B(5, "avx2");
        CPU_FLAG_B(8, "bmi2");
#undef CPU_FLAG_B
    }
    proc_cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000001u) {
        proc_cpuid(0x80000001u, 0, &a, &b, &c, &d);
        if (d & (1u << 20)) {
            n = RtlFormat(out + used, capacity - used, " nx");
            if (n > 0 && (SIZE_T)n < capacity - used) used += (SIZE_T)n;
        }
        if (d & (1u << 29)) {
            n = RtlFormat(out + used, capacity - used, " lm");
            if (n > 0 && (SIZE_T)n < capacity - used) used += (SIZE_T)n;
        }
        if (d & (1u << 11)) {
            n = RtlFormat(out + used, capacity - used, " syscall");
            if (n > 0 && (SIZE_T)n < capacity - used) used += (SIZE_T)n;
        }
    }
    n = RtlFormat(out + used, capacity - used, "\n");
    if (n > 0) used += (SIZE_T)n;
    return used;
}

static UINT64 proc_contents(PKPROCESS process, UINT8 node, char *out,
                            SIZE_T capacity)
{
    if (node == LINUX_PROC_VERSION) {
        static const char version[] = "Svindows version 0.6.0 x86_64\n";
        memcpy(out, version, sizeof(version) - 1);
        return sizeof(version) - 1;
    }
    if (node == LINUX_PROC_MEMINFO) return proc_meminfo(out, capacity);
    if (node == LINUX_PROC_CPUINFO) return proc_cpuinfo(out, capacity);
    if (node == LINUX_PROC_CMDLINE) {
        const char *path = process->ImagePath;
        if ((path[0] == 'C' || path[0] == 'c') && path[1] == ':')
            path += 2;
        SIZE_T n = 0;
        while (path[n] && n + 1 < 512) {
            out[n] = path[n] == '\\' ? '/' : path[n];
            n++;
        }
        out[n] = 0;
        return n + 1;
    }
    if (node == LINUX_PROC_STATUS) {
        const char *name = process->Name ? process->Name : "?";
        for (const char *p = name; *p; p++)
            if (*p == '\\' || *p == '/') name = p + 1;
        int n = RtlFormat(out, capacity,
            "Name:\t%s\nState:\tR (running)\nPid:\t%u\nPPid:\t%u\n"
            "Uid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\n",
            name, process->ProcessId, process->ParentProcessId);
        return n < 0 ? 0 : (UINT64)((SIZE_T)n < capacity ? n : (int)capacity - 1);
    }
    return 0;
}

UINT64 LinuxProcSize(PKPROCESS process, UINT8 node)
{
    char contents[PROC_CONTENT_CAPACITY];
    return proc_contents(process, node, contents, sizeof(contents));
}

UINT64 LinuxProcRead(PKPROCESS process, LINUX_FD_ENTRY *entry,
                     UINT64 buffer, UINT64 length, UINT64 offset,
                     BOOLEAN advance)
{
    if (length > LINUX_IO_MAX) return LinuxError(LINUX_EINVAL);
    if (!length) return 0;
    char contents[PROC_CONTENT_CAPACITY];
    UINT64 size = proc_contents(process, entry->ProcNode, contents, sizeof(contents));
    if (offset >= size) return 0;
    UINT64 copied = length < size - offset ? length : size - offset;
    if (!MmProbeForWrite(buffer, copied)) return LinuxError(LINUX_EFAULT);
    memcpy((void *)(ULONG_PTR)buffer, contents + offset, (SIZE_T)copied);
    if (advance) entry->Offset = offset + copied;
    return copied;
}

NTSTATUS LinuxProcSnapshot(const char *path, char *buffer, SIZE_T capacity,
                           SIZE_T *length)
{
    if (!path || !buffer || !capacity || !length)
        return STATUS_INVALID_PARAMETER;
    UINT8 node = LinuxProcPathKind(path);
    if (node != LINUX_PROC_MEMINFO && node != LINUX_PROC_CPUINFO)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    UINT64 size = proc_contents(NULL, node, buffer, capacity);
    if (size >= capacity) return STATUS_BUFFER_TOO_SMALL;
    *length = (SIZE_T)size;
    return STATUS_SUCCESS;
}


/* Linux x86-64 struct sysinfo. Values are derived from the physical page
 * allocator and the kernel's 100 Hz monotonic clock, not from fixed constants. */
typedef struct _LINUX_SYSINFO {
    INT64 Uptime;
    UINT64 Loads[3];
    UINT64 TotalRam;
    UINT64 FreeRam;
    UINT64 SharedRam;
    UINT64 BufferRam;
    UINT64 TotalSwap;
    UINT64 FreeSwap;
    UINT16 Processes;
    UINT16 Padding;
    UINT64 TotalHigh;
    UINT64 FreeHigh;
    UINT32 MemoryUnit;
    UINT8 Reserved[sizeof(INT64) - sizeof(UINT32)];
} LINUX_SYSINFO;
_Static_assert(sizeof(LINUX_SYSINFO) == 112, "x86-64 sysinfo ABI size");

UINT64 LinuxSysinfo(UINT64 destination)
{
    if (!MmProbeForWrite(destination, sizeof(LINUX_SYSINFO)))
        return LinuxError(LINUX_EFAULT);
    LINUX_SYSINFO info;
    memset(&info, 0, sizeof(info));
    info.Uptime = (INT64)(KeGetTickCount() / 100u);
    info.TotalRam = MmUsablePageCount() << PAGE_SHIFT;
    info.FreeRam = MmFreePageCount() << PAGE_SHIFT;
    info.MemoryUnit = 1;
    memcpy((void *)(ULONG_PTR)destination, &info, sizeof(info));
    return 0;
}

UINT64 LinuxProcReadlink(PKPROCESS process, UINT8 node,
                         UINT64 buffer, UINT64 size)
{
    if (node != LINUX_PROC_EXE && node != LINUX_PROC_CWD)
        return LinuxError(LINUX_EINVAL);
    if (!size) return LinuxError(LINUX_EINVAL);
    const char *path = node == LINUX_PROC_EXE ? process->ImagePath :
                       process->CurrentDirectory;
    if ((path[0] != 'C' && path[0] != 'c') || path[1] != ':' ||
        path[2] != '\\') return LinuxError(LINUX_ENOENT);
    path += 2;
    UINT64 length = strlen(path);
    UINT64 copied = length < size ? length : size;
    if (!MmProbeForWrite(buffer, copied)) return LinuxError(LINUX_EFAULT);
    for (UINT64 i = 0; i < copied; i++)
        *(char *)(ULONG_PTR)(buffer + i) = path[i] == '\\' ? '/' : path[i];
    return copied;
}

UINT64 LinuxProcGetdents(LINUX_FD_ENTRY *entry, UINT64 buffer, UINT64 length)
{
    static const char *const root_names[] = {".", "..", "self", "version", "meminfo", "cpuinfo"};
    static const UINT8 root_types[] =
        {PROC_DT_DIR, PROC_DT_DIR, PROC_DT_DIR, PROC_DT_REG, PROC_DT_REG, PROC_DT_REG};
    static const UINT8 root_nodes[] =
        {LINUX_PROC_ROOT, LINUX_PROC_ROOT, LINUX_PROC_SELF,
         LINUX_PROC_VERSION, LINUX_PROC_MEMINFO, LINUX_PROC_CPUINFO};
    static const char *const self_names[] =
        {".", "..", "status", "cmdline", "exe", "cwd"};
    static const UINT8 self_types[] =
        {PROC_DT_DIR, PROC_DT_DIR, PROC_DT_REG, PROC_DT_REG,
         PROC_DT_LNK, PROC_DT_LNK};
    static const UINT8 self_nodes[] =
        {LINUX_PROC_SELF, LINUX_PROC_ROOT, LINUX_PROC_STATUS,
         LINUX_PROC_CMDLINE, LINUX_PROC_EXE, LINUX_PROC_CWD};
    BOOLEAN self = LinuxProcPathKind(entry->DirectoryPath) == LINUX_PROC_SELF;
    const char *const *names = self ? self_names : root_names;
    const UINT8 *types = self ? self_types : root_types;
    const UINT8 *nodes = self ? self_nodes : root_nodes;
    UINT64 count = self ? 6 : 6;
    UINT64 written = 0;
    while (entry->Offset < count) {
        UINT64 index = entry->Offset;
        SIZE_T name_length = strlen(names[index]);
        UINT64 record_length = (19 + name_length + 1 + 7) & ~7ULL;
        if (record_length > length - written)
            return written ? written : LinuxError(LINUX_EINVAL);
        PROC_DIRENT64 *record = (PROC_DIRENT64 *)(ULONG_PTR)(buffer + written);
        memset(record, 0, (SIZE_T)record_length);
        record->Inode = 0x7000u + nodes[index];
        record->NextOffset = (INT64)(index + 1);
        record->RecordLength = (UINT16)record_length;
        record->Type = types[index];
        memcpy(record->Name, names[index], name_length + 1);
        written += record_length;
        entry->Offset++;
    }
    return written;
}
