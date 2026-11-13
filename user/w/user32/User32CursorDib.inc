/* Cursor resource DIBs retain alpha and classic AND/XOR semantics. Output is
 * top-down ARGB plus one AND bit per pixel, independent of display format. */
static unsigned cursor_u16(const unsigned char *p)
{ return p[0] | (unsigned)p[1] << 8; }
static unsigned cursor_u32(const unsigned char *p)
{ return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }
static int sv_cursor_dib(const unsigned char *data, unsigned bytes,
                         unsigned width, unsigned height, unsigned *pixels,
                         unsigned char *and_mask, unsigned *flags,
                         unsigned *source_width_out, unsigned *source_height_out)
{
    if (!data || bytes < 40 || !pixels || !and_mask || !width || !height ||
        width > 256 || height > 256) return 0;
    unsigned header=cursor_u32(data), source_width=cursor_u32(data+4);
    unsigned doubled_height=cursor_u32(data+8), bits=cursor_u16(data+14);
    if(header<40||header>bytes||!source_width||source_width>256||
       !doubled_height||doubled_height>512||(doubled_height&1)||
       cursor_u16(data+12)!=1||cursor_u32(data+16)||
       (bits!=1&&bits!=4&&bits!=8&&bits!=24&&bits!=32)) return 0;
    unsigned source_height=doubled_height/2;
    unsigned colors=bits<=8?cursor_u32(data+32):0;
    if(bits<=8&&!colors) colors=1u<<bits;
    if(colors>256||colors*4>bytes-header) return 0;
    unsigned start=header+colors*4, stride=((source_width*bits+31)/32)*4;
    unsigned raster_bytes=stride*source_height;
    if(raster_bytes>bytes-start) return 0;
    unsigned mask_start=start+raster_bytes, mask_stride=((source_width+31)/32)*4;
    int mask_present=mask_stride*source_height<=bytes-mask_start;
    int alpha=0;
    if(bits==32) for(unsigned i=0;i<raster_bytes;i+=4) if(data[start+i+3]) alpha=1;
    if(!alpha&&!mask_present) return 0;
    for(unsigned y=0;y<height;++y) {
        unsigned sy=source_height-1-y*source_height/height;
        const unsigned char *line=data+start+sy*stride;
        for(unsigned x=0;x<width;++x) {
            unsigned sx=x*source_width/width, color=0;
            if(bits<=8) {
                unsigned index=bits==8?line[sx]:bits==4?
                    (line[sx/2]>>(sx&1?0:4))&15:(line[sx/8]>>(7-sx%8))&1;
                if(index>=colors) return 0;
                color=cursor_u32(data+header+index*4)&0xffffffu;
            } else {
                const unsigned char *p=line+sx*(bits/8);
                color=p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16;
                if(alpha) color|=(unsigned)p[3]<<24;
            }
            unsigned target=y*width+x;
            pixels[target]=color;
            and_mask[target]=mask_present&&
                (data[mask_start+sy*mask_stride+sx/8]&(0x80>>(sx%8)))?1:0;
        }
    }
    *flags=(alpha?1u:0u)|(bits==1?4u:0u);
    *source_width_out=source_width; *source_height_out=source_height;
    return 1;
}
