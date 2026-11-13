#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../../user/w/user32/User32IconDib.inc"
#include "../../user/w/user32/User32CursorDib.inc"
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
    unsigned char cursor_dib[64]={0},mask[16]; unsigned cursor_pixels[16],flags,w,h;
    put32(cursor_dib,40); put32(cursor_dib+4,2); put32(cursor_dib+8,4);
    put16(cursor_dib+12,1); put16(cursor_dib+14,1); put32(cursor_dib+44,0xffffff);
    cursor_dib[48]=cursor_dib[52]=0x40; cursor_dib[56]=0xc0;
    assert(sv_cursor_dib(cursor_dib,sizeof(cursor_dib),2,2,cursor_pixels,mask,&flags,&w,&h));
    assert(flags==4&&w==2&&h==2&&cursor_pixels[0]==0&&cursor_pixels[1]==0xffffff&&
           cursor_pixels[2]==0&&cursor_pixels[3]==0xffffff&&mask[0]==0&&mask[1]==0&&mask[2]==1&&mask[3]==1);
    assert(sv_cursor_dib(cursor_dib,sizeof(cursor_dib),4,4,cursor_pixels,mask,&flags,&w,&h));
    assert(mask[8]==1&&mask[15]==1);
    assert(!sv_cursor_dib(cursor_dib,63,2,2,cursor_pixels,mask,&flags,&w,&h));
    put32(cursor_dib+8,3); assert(!sv_cursor_dib(cursor_dib,64,2,2,cursor_pixels,mask,&flags,&w,&h));
    put32(cursor_dib+8,2); put16(cursor_dib+14,32);
    put32(cursor_dib+40,0x80010203); put32(cursor_dib+44,0x00ff0000);
    assert(sv_cursor_dib(cursor_dib,52,2,1,cursor_pixels,mask,&flags,&w,&h));
    assert(flags==1&&cursor_pixels[0]==0x80010203&&cursor_pixels[1]==0x00ff0000);
    puts("PASS cursor DIB black/white/transparent/invert masks, alpha, top-down scaling and invalid bounds");
}
