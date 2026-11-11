/* Decode uncompressed Windows icon DIBs into an opaque BGRA bitmap.
 * Alpha and the classic AND mask are composed over the requested background. */
static unsigned icon_u16(const unsigned char *p)
{ return p[0] | (unsigned)p[1] << 8; }
static unsigned icon_u32(const unsigned char *p)
{ return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }

static int sv_icon_dib(const unsigned char *data, unsigned size, int width,
    int height, unsigned background, unsigned *pixels)
{
    if (!data || size < 40 || !pixels || width <= 0 || height <= 0 ||
        width > 256 || height > 256) return 0;
    unsigned header = icon_u32(data), source_width = icon_u32(data + 4);
    unsigned double_height = icon_u32(data + 8), bits = icon_u16(data + 14);
    if (header < 40 || header > size || !source_width || source_width > 256 ||
        !double_height || double_height > 512 || (double_height & 1) ||
        icon_u16(data + 12) != 1 || icon_u32(data + 16) ||
        (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32)) return 0;
    unsigned source_height = double_height / 2;
    unsigned colors = bits <= 8 ? icon_u32(data + 32) : 0;
    if (bits <= 8 && !colors) colors = 1u << bits;
    if (colors > 256 || colors * 4 > size - header) return 0;
    unsigned start = header + colors * 4;
    unsigned stride = ((source_width * bits + 31) / 32) * 4;
    unsigned mask_stride = ((source_width + 31) / 32) * 4;
    unsigned raster_size = stride * source_height;
    if (raster_size > size - start) return 0;
    unsigned mask_start = start + raster_size;
    int mask_present = mask_stride * source_height <= size - mask_start;
    int has_alpha = 0;
    if (bits == 32)
        for (unsigned y = 0; y < source_height; ++y)
            for (unsigned x = 0; x < source_width; ++x)
                if (data[start + y * stride + x * 4 + 3]) has_alpha = 1;
    for (int y = 0; y < height; ++y) {
        unsigned source_y = source_height - 1 - (unsigned)y * source_height / height;
        const unsigned char *line = data + start + source_y * stride;
        for (int x = 0; x < width; ++x) {
            unsigned source_x = (unsigned)x * source_width / width;
            unsigned color = 0, alpha = 255;
            if (bits <= 8) {
                unsigned index = bits == 8 ? line[source_x] : bits == 4 ?
                    (line[source_x / 2] >> (source_x & 1 ? 0 : 4)) & 15 :
                    (line[source_x / 8] >> (7 - source_x % 8)) & 1;
                if (index >= colors) return 0;
                color = icon_u32(data + header + index * 4) & 0xffffff;
            } else {
                const unsigned char *p = line + source_x * (bits / 8);
                color = p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16;
                if (bits == 32 && has_alpha) alpha = p[3];
            }
            if (!has_alpha && mask_present &&
                (data[mask_start + source_y * mask_stride + source_x / 8] &
                 (0x80 >> (source_x % 8)))) alpha = 0;
            unsigned result = 0xff000000u;
            for (unsigned shift = 0; shift < 24; shift += 8) {
                unsigned foreground = (color >> shift) & 255;
                /* COLORREF background is RGB-order; DIB pixels are BGR-order. */
                unsigned back = (background >> (16 - shift)) & 255;
                result |= ((foreground * alpha + back * (255 - alpha) + 127) / 255) << shift;
            }
            pixels[(unsigned)y * width + x] = result;
        }
    }
    return 1;
}
