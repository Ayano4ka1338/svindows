#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../../user/w/user32/User32IconDib.inc"
static void put16(unsigned char *out, unsigned value)
{ out[0] = value; out[1] = value >> 8; }
static void put32(unsigned char *out, unsigned value)
{ for (unsigned i = 0; i < 4; ++i) out[i] = value >> (i * 8); }
int main(void)
{
    unsigned char dib[56] = {0};
    put32(dib, 40); put32(dib + 4, 2); put32(dib + 8, 2);
    put16(dib + 12, 1); put16(dib + 14, 32);
    put32(dib + 40, 0x800000ff); put32(dib + 44, 0xff00ff00);
    dib[48] = 0x80;
    unsigned out[8];
    assert(sv_icon_dib(dib, 52, 2, 1, 0x0000ff, out));
    assert(out[0] == 0xff7f0080 && out[1] == 0xff00ff00);
    dib[43] = dib[47] = 0;
    assert(sv_icon_dib(dib, 52, 2, 1, 0x0000ff, out));
    assert(out[0] == 0xffff0000 && out[1] == 0xff00ff00);
    assert(sv_icon_dib(dib, 52, 4, 2, 0x0000ff, out));
    assert(out[0] == out[1] && out[2] == out[3] && out[0] == out[4]);
    assert(!sv_icon_dib(dib, 43, 2, 1, 0, out));
    assert(!sv_icon_dib(dib, 52, 257, 1, 0, out));
    memset(dib + 40, 0, 16);
    put16(dib + 14, 4); put32(dib + 32, 2);
    put32(dib + 44, 0xffffff); dib[48] = 0x01;
    assert(sv_icon_dib(dib, 56, 2, 1, 0, out));
    assert(out[0] == 0xff000000 && out[1] == 0xffffffff);
    dib[48] = 0x02;
    assert(!sv_icon_dib(dib, 56, 2, 1, 0, out));
    puts("PASS icon DIB alpha, classic masks, palettes, scaling and malformed bounds");
}
