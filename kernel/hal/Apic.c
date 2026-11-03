/* ACPI processor topology discovery. APIC programming and AP startup are
 * separate phases so firmware discovery can be validated before any AP runs. */
#include <svindows/Hal.h>
#include <svindows/Mm.h>
#include <svindows/Multiboot2.h>
#include <svindows/Ke.h>
#include <svindows/Rtl.h>

#define MB_TAG_TYPE_ACPI_OLD 14u
#define MB_TAG_TYPE_ACPI_NEW 15u

typedef struct PACKED _ACPI_RSDP {
    char Signature[8];
    UINT8 Checksum;
    char OemId[6];
    UINT8 Revision;
    UINT32 RsdtAddress;
} ACPI_RSDP;

typedef struct PACKED _ACPI_RSDP2 {
    ACPI_RSDP First;
    UINT32 Length;
    UINT64 XsdtAddress;
    UINT8 ExtendedChecksum;
    UINT8 Reserved[3];
} ACPI_RSDP2;

typedef struct PACKED _ACPI_SDT_HEADER {
    char Signature[4];
    UINT32 Length;
    UINT8 Revision;
    UINT8 Checksum;
    char OemId[6];
    char OemTableId[8];
    UINT32 OemRevision;
    UINT32 CreatorId;
    UINT32 CreatorRevision;
} ACPI_SDT_HEADER;

typedef struct PACKED _ACPI_MADT {
    ACPI_SDT_HEADER Header;
    UINT32 LocalApicAddress;
    UINT32 Flags;
    UINT8 Entries[];
} ACPI_MADT;

typedef struct PACKED _ACPI_MADT_ENTRY {
    UINT8 Type;
    UINT8 Length;
} ACPI_MADT_ENTRY;

typedef struct PACKED _ACPI_MADT_LOCAL_APIC {
    UINT8 Type, Length, AcpiProcessorId, ApicId;
    UINT32 Flags;
} ACPI_MADT_LOCAL_APIC;

typedef struct PACKED _ACPI_MADT_LOCAL_X2APIC {
    UINT8 Type, Length;
    UINT16 Reserved;
    UINT32 X2ApicId;
    UINT32 Flags;
    UINT32 AcpiProcessorId;
} ACPI_MADT_LOCAL_X2APIC;

static HAL_PROCESSOR_INFO g_processors[HAL_MAXIMUM_PROCESSORS];
static UINT32 g_processor_count = 1;
static UINT64 g_local_apic_phys;
static volatile UINT32 *g_local_apic;
static BOOLEAN g_x2apic;

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_X2APIC_EOI    0x80B
#define IA32_X2APIC_ICR    0x830
#define APIC_BASE_ENABLE   (1ULL << 11)
#define APIC_BASE_X2       (1ULL << 10)
#define APIC_MMIO_VA       0xFFFF900000000000ULL

static UINT64 apic_rdmsr(UINT32 msr)
{
    UINT32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UINT64)hi << 32) | lo;
}

static void apic_wrmsr(UINT32 msr, UINT64 value)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((UINT32)value),
                     "d"((UINT32)(value >> 32)));
}

static void apic_wait_delivery(void)
{
    if (g_x2apic) return;
    while (g_local_apic[0x300 / 4] & (1u << 12))
        __asm__ volatile("pause");
}

static UINT8 acpi_checksum(const void *memory, UINT32 length)
{
    const UINT8 *bytes = memory;
    UINT8 sum = 0;
    for (UINT32 i = 0; i < length; i++) sum = (UINT8)(sum + bytes[i]);
    return sum;
}

static BOOLEAN rsdp_valid(const ACPI_RSDP *rsdp, UINT32 available)
{
    static const char signature[8] = {'R','S','D',' ','P','T','R',' '};
    if (!rsdp || available < sizeof(*rsdp) ||
        memcmp(rsdp->Signature, signature, sizeof(signature)) ||
        acpi_checksum(rsdp, sizeof(*rsdp)))
        return FALSE;
    if (rsdp->Revision >= 2) {
        if (available < sizeof(ACPI_RSDP2)) return FALSE;
        const ACPI_RSDP2 *extended = (const ACPI_RSDP2 *)rsdp;
        if (extended->Length < sizeof(*extended) || extended->Length > 4096 ||
            extended->Length > available ||
            acpi_checksum(extended, extended->Length))
            return FALSE;
    }
    return TRUE;
}

static const ACPI_RSDP *scan_rsdp(UINT64 begin, UINT64 end)
{
    for (UINT64 address = (begin + 15u) & ~15ULL;
         address <= end && end - address >= sizeof(ACPI_RSDP);
         address += 16u) {
        const ACPI_RSDP *rsdp = (const ACPI_RSDP *)MmPhysToVirt(address);
        if (rsdp_valid(rsdp, (UINT32)(end - address))) return rsdp;
    }
    return NULL;
}

static const ACPI_RSDP *find_rsdp(UINT64 mbi_phys)
{
    if (mbi_phys) {
        const MB_INFO_HEADER *info = MmPhysToVirt(mbi_phys);
        if (info->total_size >= sizeof(*info) + sizeof(MB_TAG) &&
            info->total_size <= 16u * 1024u * 1024u) {
            UINT64 end = mbi_phys + info->total_size;
            for (UINT64 address = mbi_phys + sizeof(*info);
                 address + sizeof(MB_TAG) <= end; ) {
                const MB_TAG *tag = MmPhysToVirt(address);
                if (tag->size < sizeof(*tag) || tag->size > end - address)
                    break;
                UINT32 rsdp_size = tag->size - sizeof(*tag);
                if (tag->type == MB_TAG_TYPE_ACPI_NEW &&
                    rsdp_valid((const ACPI_RSDP *)((const UINT8 *)tag +
                               sizeof(*tag)), rsdp_size))
                    return (const ACPI_RSDP *)((const UINT8 *)tag + sizeof(*tag));
                if (tag->type == MB_TAG_TYPE_ACPI_OLD &&
                    rsdp_valid((const ACPI_RSDP *)((const UINT8 *)tag +
                               sizeof(*tag)), rsdp_size))
                    return (const ACPI_RSDP *)((const UINT8 *)tag + sizeof(*tag));
                UINT64 next = address + ((tag->size + 7u) & ~7u);
                if (next <= address) break;
                address = next;
            }
        }
    }

    /* Firmware fallback: first scan the EBDA, then the standard BIOS range. */
    UINT16 ebda_segment = *(volatile UINT16 *)MmPhysToVirt(0x40eu);
    UINT64 ebda = (UINT64)ebda_segment << 4;
    const ACPI_RSDP *rsdp = NULL;
    if (ebda >= 0x400u && ebda < 0xa0000u)
        rsdp = scan_rsdp(ebda, ebda + 1024u);
    if (!rsdp) rsdp = scan_rsdp(0xe0000u, 0x100000u);
    return rsdp;
}

static const ACPI_SDT_HEADER *acpi_table(UINT64 physical,
                                         const char signature[4])
{
    if (!physical || physical > ~(UINT64)0 - sizeof(ACPI_SDT_HEADER))
        return NULL;
    const ACPI_SDT_HEADER *header = MmPhysToVirt(physical);
    if (memcmp(header->Signature, signature, 4) ||
        header->Length < sizeof(*header) || header->Length > 1024u * 1024u ||
        physical > ~(UINT64)0 - header->Length ||
        acpi_checksum(header, header->Length))
        return NULL;
    return header;
}

static UINT32 bootstrap_apic_id(void)
{
    UINT32 eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    return ebx >> 24;
}

static void append_processor(UINT32 apic_id, UINT32 acpi_id, UINT32 flags)
{
    if (!(flags & 1u)) return; /* online-capable but disabled entries are not BSPs */
    for (UINT32 i = 0; i < g_processor_count; i++)
        if (g_processors[i].ApicId == apic_id) return;
    if (g_processor_count >= HAL_MAXIMUM_PROCESSORS) return;
    HAL_PROCESSOR_INFO *processor = &g_processors[g_processor_count++];
    processor->ApicId = apic_id;
    processor->AcpiProcessorId = acpi_id;
}

UINT32 HalGetProcessorCount(void) { return g_processor_count; }

const HAL_PROCESSOR_INFO *HalGetProcessorInfo(UINT32 index)
{
    return index < g_processor_count ? &g_processors[index] : NULL;
}

BOOLEAN HalDiscoverProcessors(UINT64 mbi_phys)
{
    memset(g_processors, 0, sizeof(g_processors));
    g_processor_count = 0;
    UINT32 bsp_id = bootstrap_apic_id();
    const ACPI_RSDP *rsdp = find_rsdp(mbi_phys);
    if (!rsdp) goto fallback;

    UINT64 root_phys = rsdp->RsdtAddress;
    const char *root_signature = "RSDT";
    if (rsdp->Revision >= 2) {
        UINT64 xsdt = ((const ACPI_RSDP2 *)rsdp)->XsdtAddress;
        if (xsdt) { root_phys = xsdt; root_signature = "XSDT"; }
    }
    const ACPI_SDT_HEADER *root = acpi_table(root_phys, root_signature);
    if (!root) goto fallback;
    UINT32 entry_size = root_signature[0] == 'X' ? 8u : 4u;
    UINT32 payload_size = root->Length - sizeof(*root);
    if (payload_size % entry_size) goto fallback;
    const UINT8 *entries = (const UINT8 *)root + sizeof(*root);
    const ACPI_MADT *madt = NULL;
    for (UINT32 offset = 0; offset < payload_size; offset += entry_size) {
        UINT64 table_phys = entry_size == 8
            ? *(const UINT64 *)(entries + offset)
            : *(const UINT32 *)(entries + offset);
        const ACPI_SDT_HEADER *table = acpi_table(table_phys, "APIC");
        if (table) { madt = (const ACPI_MADT *)table; break; }
    }
    if (!madt || madt->Header.Length < sizeof(*madt)) goto fallback;
    g_local_apic_phys = madt->LocalApicAddress;

    const UINT8 *cursor = madt->Entries;
    const UINT8 *end = (const UINT8 *)madt + madt->Header.Length;
    while ((SIZE_T)(end - cursor) >= sizeof(ACPI_MADT_ENTRY)) {
        const ACPI_MADT_ENTRY *entry = (const ACPI_MADT_ENTRY *)cursor;
        if (entry->Length < sizeof(*entry) ||
            (SIZE_T)(end - cursor) < entry->Length)
            goto fallback;
        if (entry->Type == 0 && entry->Length >= sizeof(ACPI_MADT_LOCAL_APIC)) {
            const ACPI_MADT_LOCAL_APIC *local =
                (const ACPI_MADT_LOCAL_APIC *)entry;
            append_processor(local->ApicId, local->AcpiProcessorId,
                             local->Flags);
        } else if (entry->Type == 9 &&
                   entry->Length >= sizeof(ACPI_MADT_LOCAL_X2APIC)) {
            const ACPI_MADT_LOCAL_X2APIC *local =
                (const ACPI_MADT_LOCAL_X2APIC *)entry;
            append_processor(local->X2ApicId, local->AcpiProcessorId,
                             local->Flags);
        }
        cursor += entry->Length;
    }

    /* Keep the running CPU at slot zero for the bootstrap and scheduler paths. */
    UINT32 bsp_index = g_processor_count;
    for (UINT32 i = 0; i < g_processor_count; i++)
        if (g_processors[i].ApicId == bsp_id) { bsp_index = i; break; }
    if (bsp_index == g_processor_count) {
        if (g_processor_count >= HAL_MAXIMUM_PROCESSORS) goto fallback;
        bsp_index = g_processor_count++;
        g_processors[bsp_index].ApicId = bsp_id;
        g_processors[bsp_index].AcpiProcessorId = 0;
    }
    if (bsp_index) {
        HAL_PROCESSOR_INFO temp = g_processors[0];
        g_processors[0] = g_processors[bsp_index];
        g_processors[bsp_index] = temp;
    }
    for (UINT32 i = 0; i < g_processor_count; i++) {
        g_processors[i].BootstrapProcessor = i == 0;
        KPRCB *processor = KeGetProcessorBlock(i);
        processor->Self = processor;
        processor->ProcessorNumber = i;
        processor->ApicId = g_processors[i].ApicId;
    }
    KeLog("[smp]  ACPI MADT: %u enabled processor(s), BSP APIC ID %u\n",
          g_processor_count, bsp_id);
    return TRUE;

fallback:
    memset(g_processors, 0, sizeof(g_processors));
    g_processor_count = 1;
    g_processors[0].ApicId = bsp_id;
    g_processors[0].BootstrapProcessor = TRUE;
    KPRCB *processor = KeGetProcessorBlock(0);
    processor->Self = processor;
    processor->ProcessorNumber = 0;
    processor->ApicId = bsp_id;
    KeLog("[smp]  ACPI MADT unavailable; using bootstrap processor APIC ID %u\n",
          bsp_id);
    return FALSE;
}

BOOLEAN HalInitializeLocalApic(void)
{
    UINT32 eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (!(edx & (1u << 9))) return FALSE; /* no architectural local APIC */

    UINT64 base = apic_rdmsr(IA32_APIC_BASE_MSR);
    g_x2apic = (base & APIC_BASE_X2) != 0;
    if (!g_x2apic && (ecx & (1u << 21))) {
        /* Preserve firmware's x2APIC selection; do not switch modes at runtime. */
        g_x2apic = FALSE;
    }
    if (g_x2apic) {
        apic_wrmsr(IA32_APIC_BASE_MSR,
                   base | APIC_BASE_ENABLE | APIC_BASE_X2);
        apic_wrmsr(0x80F, 0x1FF); /* software enable, spurious vector 0xff */
        apic_wrmsr(0x808, 0);     /* task priority */
        apic_wrmsr(0x832, 1u << 16); /* mask timer LVT */
        apic_wrmsr(0x833, 1u << 16); /* mask thermal LVT */
        apic_wrmsr(0x834, 1u << 16); /* mask performance LVT */
        apic_wrmsr(0x835, 1u << 16); /* mask LINT0 */
        apic_wrmsr(0x836, 1u << 16); /* mask LINT1 */
        apic_wrmsr(0x837, 1u << 16); /* mask error LVT */
        if (KeGetCurrentProcessorBlock()->ProcessorNumber == 0)
            KeLog("[smp]  local APIC enabled in x2APIC mode\n");
        return TRUE;
    }

    UINT64 physical = (base & 0xFFFFF000ULL);
    if (g_local_apic_phys) physical = g_local_apic_phys;
    if (!physical || !MmMapPage(APIC_MMIO_VA, physical,
                                 PTE_WRITE | PTE_PCD | PTE_PWT))
        return FALSE;
    g_local_apic = (volatile UINT32 *)APIC_MMIO_VA;
    apic_wrmsr(IA32_APIC_BASE_MSR,
               (base & ~0xFFFFF000ULL) | physical | APIC_BASE_ENABLE);
    g_local_apic[0x80 / 4] = 0; /* task priority */
    g_local_apic[0xF0 / 4] = 0x1FF; /* software enable, vector 0xff */
    g_local_apic[0x320 / 4] = 1u << 16; /* mask timer LVT */
    g_local_apic[0x330 / 4] = 1u << 16; /* mask thermal LVT */
    g_local_apic[0x340 / 4] = 1u << 16; /* mask performance LVT */
    g_local_apic[0x350 / 4] = 1u << 16; /* mask LINT0 */
    g_local_apic[0x360 / 4] = 1u << 16; /* mask LINT1 */
    g_local_apic[0x370 / 4] = 1u << 16; /* mask error LVT */
    if (KeGetCurrentProcessorBlock()->ProcessorNumber == 0)
        KeLog("[smp]  local APIC enabled (MMIO 0x%lx)\n",
              (unsigned long)physical);
    return TRUE;
}

void HalLocalApicEoi(void)
{
    if (g_x2apic) apic_wrmsr(IA32_X2APIC_EOI, 0);
    else if (g_local_apic) g_local_apic[0xB0 / 4] = 0;
}

BOOLEAN HalSendIpi(UINT32 apic_id, UINT8 vector)
{
    UINT32 command = vector;
    if (g_x2apic) {
        apic_wait_delivery();
        apic_wrmsr(IA32_X2APIC_ICR, ((UINT64)apic_id << 32) | command);
        return TRUE;
    }
    if (!g_local_apic || apic_id > 255u) return FALSE;
    apic_wait_delivery();
    g_local_apic[0x310 / 4] = apic_id << 24;
    g_local_apic[0x300 / 4] = command;
    apic_wait_delivery();
    return TRUE;
}

static BOOLEAN hal_send_apic_command(UINT32 apic_id, UINT32 command)
{
    if (g_x2apic) {
        apic_wait_delivery();
        apic_wrmsr(IA32_X2APIC_ICR, ((UINT64)apic_id << 32) | command);
        return TRUE;
    }
    if (!g_local_apic || apic_id > 255u) return FALSE;
    apic_wait_delivery();
    g_local_apic[0x310 / 4] = apic_id << 24;
    g_local_apic[0x300 / 4] = command;
    apic_wait_delivery();
    return TRUE;
}

BOOLEAN HalSendInitIpi(UINT32 apic_id, BOOLEAN assert_init)
{
    /* INIT is level-triggered and must be explicitly deasserted. */
    UINT32 command = (5u << 8) | (1u << 15);
    if (assert_init) command |= 1u << 14;
    return hal_send_apic_command(apic_id, command);
}

BOOLEAN HalSendStartupIpi(UINT32 apic_id, UINT8 startup_page)
{
    if (!startup_page) return FALSE;
    return hal_send_apic_command(apic_id, (6u << 8) | startup_page);
}
