/* CM descriptors are packed to four-byte boundaries, including their 64-bit
 * physical addresses. Decode bytes rather than making unaligned typed loads. */
static ULONG resource_u32(const BYTE *p)
{ return (ULONG)p[0]|((ULONG)p[1]<<8)|((ULONG)p[2]<<16)|((ULONG)p[3]<<24); }
static ULONGLONG resource_u64(const BYTE *p)
{ return resource_u32(p)|((ULONGLONG)resource_u32(p+4)<<32); }
static int resource_shift(BYTE type, unsigned flags)
{
    if (type==1 || type==3) return 0; /* port or ordinary memory */
    if (type!=7) return -1;
    switch (flags&0xe00) {
    case 0x200: return 8;
    case 0x400: return 16;
    case 0x800: return 32;
    default: return -1;
    }
}
ULONGLONG RtlCmDecodeMemIoResource(const void *descriptor, ULONGLONG *start)
{
    if (!descriptor) return 0;
    const BYTE *p=descriptor;
    int shift=resource_shift(p[0],p[2]|((unsigned)p[3]<<8));
    if (shift<0) return 0;
    if (start) *start=resource_u64(p+4);
    return (ULONGLONG)resource_u32(p+12)<<shift;
}
ULONGLONG RtlIoDecodeMemIoResource(const void *descriptor, ULONGLONG *alignment,
    ULONGLONG *minimum, ULONGLONG *maximum)
{
    if (!descriptor) return 0;
    const BYTE *p=descriptor;
    int shift=resource_shift(p[1],p[4]|((unsigned)p[5]<<8));
    if (shift<0) return 0;
    if (alignment) *alignment=(ULONGLONG)resource_u32(p+12)<<shift;
    if (minimum) *minimum=resource_u64(p+16);
    if (maximum) *maximum=resource_u64(p+24);
    return (ULONGLONG)resource_u32(p+8)<<shift;
}
