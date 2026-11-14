/*
 * mm/Multiboot.c - turn the Multiboot2 information block into an MM_MEMORY_MAP.
 *
 * The memory-map parser runs while the low 4 GiB is still identity-mapped.
 * Limine modules are found later through the direct map after VMM startup.
 */
#include <svindows/Mm.h>
#include <svindows/Multiboot2.h>
#include <svindows/Gfx.h>
#include <string.h>

BOOLEAN MmFindMultibootModule(UINT64 mb_info_phys, const char *name,
                              UINT64 *base_out, UINT64 *size_out)
{
    if (!mb_info_phys || !name || !base_out || !size_out ||
        (mb_info_phys & 7))
        return FALSE;
    MB_INFO_HEADER *hdr = (MB_INFO_HEADER *)MmPhysToVirt(mb_info_phys);
    if (hdr->total_size < sizeof(*hdr) + sizeof(MB_TAG) ||
        hdr->total_size > ~(UINT64)0 - mb_info_phys)
        return FALSE;
    UINT64 end = mb_info_phys + hdr->total_size;
    SIZE_T name_size = strlen(name);
    for (UINT64 cursor = mb_info_phys + sizeof(*hdr);
         cursor + sizeof(MB_TAG) <= end;) {
        MB_TAG *tag = (MB_TAG *)MmPhysToVirt(cursor);
        if (tag->size < sizeof(*tag) || tag->size > end - cursor)
            return FALSE;
        if (tag->type == MB_TAG_TYPE_END)
            return FALSE;
        if (tag->type == MB_TAG_TYPE_MODULE &&
            tag->size >= __builtin_offsetof(MB_TAG_MODULE, command_line)) {
            MB_TAG_MODULE *module = (MB_TAG_MODULE *)tag;
            if (module->end > module->start) {
                const char *cmd = module->command_line;
                UINT32 cmd_size = tag->size -
                    (UINT32)__builtin_offsetof(MB_TAG_MODULE, command_line);
                BOOLEAN matches = FALSE;
                for (UINT32 i = 0; name_size && i + name_size <= cmd_size; i++) {
                    SIZE_T j = 0;
                    while (j < name_size && cmd[i + j] == name[j]) j++;
                    if (j == name_size) {
                        matches = TRUE;
                        break;
                    }
                }
                if (matches || (cmd_size && !*cmd)) {
                    *base_out = module->start;
                    *size_out = (UINT64)module->end - module->start;
                    return TRUE;
                }
            }
        }
        cursor += (tag->size + 7u) & ~7u;
    }
    return FALSE;
}

const char *MmRegionTypeName(MM_REGION_TYPE type)
{
    switch (type) {
    case MmRegionAvailable:       return "available";
    case MmRegionReserved:        return "reserved";
    case MmRegionAcpiReclaimable: return "ACPI-reclaim";
    case MmRegionAcpiNvs:         return "ACPI-NVS";
    case MmRegionBad:             return "bad";
    default:                      return "unknown";
    }
}

static MM_REGION_TYPE translate_type(UINT32 mb_type)
{
    switch (mb_type) {
    case MB_MEMORY_AVAILABLE:        return MmRegionAvailable;
    case MB_MEMORY_ACPI_RECLAIMABLE: return MmRegionAcpiReclaimable;
    case MB_MEMORY_NVS:              return MmRegionAcpiNvs;
    case MB_MEMORY_BADRAM:           return MmRegionBad;
    default:                         return MmRegionReserved;
    }
}

BOOLEAN MmParseMultibootMemoryMap(UINT64 mb_info_phys, MM_MEMORY_MAP *out)
{
    if (!out)
        return FALSE;
    out->count = 0;
    out->highest_address = 0;
    out->total_available = 0;

    if (mb_info_phys == 0 || (mb_info_phys & 7))
        return FALSE;

    MB_INFO_HEADER *hdr = (MB_INFO_HEADER *)MmPhysToVirt(mb_info_phys);
    if (hdr->total_size < sizeof(*hdr) + sizeof(MB_TAG) ||
        hdr->total_size > ~(UINT64)0 - mb_info_phys)
        return FALSE;
    UINT64 end = mb_info_phys + hdr->total_size;

    /* Walk the tag list. Tags are 8-byte aligned; the list ends at MB_TAG_END. */
    UINT64 cursor = mb_info_phys + sizeof(MB_INFO_HEADER);
    while (cursor + sizeof(MB_TAG) <= end) {
        MB_TAG *tag = (MB_TAG *)MmPhysToVirt(cursor);
        if (tag->size < sizeof(MB_TAG) || tag->size > end - cursor)
            return FALSE;
        if (tag->type == MB_TAG_TYPE_END)
            return tag->size == sizeof(MB_TAG) && out->count > 0;

        if (tag->type == MB_TAG_TYPE_FRAMEBUFFER) {
            if (tag->size < __builtin_offsetof(MB_TAG_FRAMEBUFFER, red_field_position))
                return FALSE;
            MB_TAG_FRAMEBUFFER *fb = (MB_TAG_FRAMEBUFFER *)tag;
            if (fb->fb_type == MB_FRAMEBUFFER_TYPE_RGB &&
                tag->size < sizeof(MB_TAG_FRAMEBUFFER))
                return FALSE;
            GfxFramebuffer.PhysAddr = fb->addr;
            GfxFramebuffer.Pitch = fb->pitch;
            GfxFramebuffer.Width = fb->width;
            GfxFramebuffer.Height = fb->height;
            GfxFramebuffer.Bpp = fb->bpp;
            if (fb->fb_type == MB_FRAMEBUFFER_TYPE_RGB) {
                GfxFramebuffer.RedShift = fb->red_field_position;
                GfxFramebuffer.GreenShift = fb->green_field_position;
                GfxFramebuffer.BlueShift = fb->blue_field_position;
            } else {
                GfxFramebuffer.RedShift = 16;
                GfxFramebuffer.GreenShift = 8;
                GfxFramebuffer.BlueShift = 0;
            }
            GfxFramebuffer.Present = TRUE;
        }

        if (tag->type == MB_TAG_TYPE_MMAP) {
            if (tag->size < sizeof(MB_TAG_MMAP))
                return FALSE;
            MB_TAG_MMAP *mmap = (MB_TAG_MMAP *)tag;
            UINT32 entry_size = mmap->entry_size;
            if (entry_size < sizeof(MB_MMAP_ENTRY) || mmap->entry_version != 0 ||
                (tag->size - sizeof(MB_TAG_MMAP)) % entry_size)
                return FALSE;
            UINT64 entries_end = cursor + tag->size;

            for (UINT64 e = cursor + 16;
                 e + entry_size <= entries_end && out->count < MM_MAX_REGIONS;
                 e += entry_size) {
                MB_MMAP_ENTRY *me = (MB_MMAP_ENTRY *)MmPhysToVirt(e);
                if (me->len > ~(UINT64)0 - me->addr ||
                    (me->type == MB_MEMORY_AVAILABLE &&
                     me->len > ~(UINT64)0 - out->total_available))
                    return FALSE;

                MM_MEMORY_REGION *r = &out->regions[out->count++];
                r->base = me->addr;
                r->length = me->len;
                r->type = translate_type(me->type);

                if (r->type == MmRegionAvailable) {
                    out->total_available += r->length;
                    UINT64 region_end = r->base + r->length;
                    if (region_end > out->highest_address)
                        out->highest_address = region_end;
                }
            }
        }

        /* Advance to the next 8-byte-aligned tag. */
        cursor += (tag->size + 7) & ~7ULL;
    }

    return FALSE; /* An end tag is mandatory; never accept a truncated list. */
}
