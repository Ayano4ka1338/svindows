/* Validate file-controlled layout before reading tables or mapping an image.
 * Resource-only PE32 and executable PE32+ share these header offsets. */
static BOOLEAN LdrpValidateImageFile(const void *file, SIZE_T file_size)
{
    if (!file || file_size < sizeof(IMAGE_DOS_HEADER)) return FALSE;
    const UINT8 *bytes = file;
    const IMAGE_DOS_HEADER *dos = file;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) || (dos->e_lfanew & 3) ||
        dos->e_lfanew > file_size - 24) return FALSE;
    const UINT8 *headers = bytes + dos->e_lfanew;
    if (*(const UINT32 *)headers != IMAGE_NT_SIGNATURE) return FALSE;
    const IMAGE_FILE_HEADER *coff = (const IMAGE_FILE_HEADER *)(headers + 4);
    UINT64 optional_offset = (UINT64)dos->e_lfanew + 24;
    if (coff->SizeOfOptionalHeader > file_size - optional_offset ||
        coff->SizeOfOptionalHeader < 64) return FALSE;
    const UINT8 *opt = bytes + optional_offset;
    UINT16 magic = *(const UINT16 *)opt;
    UINT32 directory_offset;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) directory_offset = 112;
    else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
             *(const UINT32 *)(opt + 16) == 0) directory_offset = 96;
    else return FALSE;
    if (coff->SizeOfOptionalHeader < directory_offset) return FALSE;
    UINT32 directories = *(const UINT32 *)(opt + directory_offset - 4);
    if (directories > 16 || directories * sizeof(IMAGE_DATA_DIRECTORY) >
        coff->SizeOfOptionalHeader - directory_offset) return FALSE;
    /* Executable loader helpers read all standard directory slots. */
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
        (directories != 16 ||
         coff->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))) return FALSE;
    UINT32 image_size = *(const UINT32 *)(opt + 56);
    UINT32 header_size = *(const UINT32 *)(opt + 60);
    if (!image_size || image_size > 0x10000000 ||
        header_size > image_size || header_size > file_size) return FALSE;
    UINT32 entry = *(const UINT32 *)(opt + 16);
    if (entry >= image_size) return FALSE;
    UINT64 section_offset = optional_offset + coff->SizeOfOptionalHeader;
    UINT64 section_bytes = (UINT64)coff->NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (section_bytes > file_size - section_offset ||
        section_offset + section_bytes > header_size) return FALSE;
    const IMAGE_SECTION_HEADER *sections = (const void *)(bytes + section_offset);
    for (UINT16 i = 0; i < coff->NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER *s = &sections[i];
        UINT64 span = s->VirtualSize > s->SizeOfRawData ?
                      s->VirtualSize : s->SizeOfRawData;
        if (s->VirtualAddress > image_size || span > image_size - s->VirtualAddress ||
            (s->SizeOfRawData &&
             (s->PointerToRawData > file_size ||
              s->SizeOfRawData > file_size - s->PointerToRawData))) return FALSE;
    }
    const IMAGE_DATA_DIRECTORY *dirs = (const void *)(opt + directory_offset);
    for (UINT32 i = 0; i < directories; i++) {
        UINT64 limit = i == 4 ? file_size : image_size; /* certificate is a file offset */
        if (dirs[i].VirtualAddress > limit ||
            dirs[i].Size > limit - dirs[i].VirtualAddress) return FALSE;
    }
    return TRUE;
}
