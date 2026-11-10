#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t BYTE;
typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;
#include "../../user/w/ntdll/Rtl/ResourceRanges.inc"
static void put32(BYTE *p,ULONG n){for(unsigned i=0;i<4;i++)p[i]=(BYTE)(n>>(i*8));}
int main(void)
{
    BYTE storage[33]={0};BYTE *cm=storage+1; /* packed, deliberately unaligned */
    BYTE io[32]={0};
    ULONGLONG start=0,alignment=0,minimum=0,maximum=0;
    put32(cm+4,0x12345678);put32(cm+8,0x90abcdef);put32(cm+12,0xfedcba98);
    put32(io+8,0xfedcba98);put32(io+12,0x12);
    put32(io+16,0x12345678);put32(io+20,0x90abcdef);
    put32(io+24,0x87654321);put32(io+28,0xfedcba09);
    unsigned shifts[]={0,8,16,32},flags[]={0,0x200,0x400,0x800};
    for(unsigned i=0;i<4;i++){
        cm[0]=io[1]=i?7:3;
        cm[2]=io[4]=(BYTE)flags[i];cm[3]=io[5]=(BYTE)(flags[i]>>8);
        ULONGLONG length=(ULONGLONG)0xfedcba98u<<shifts[i];
        assert(RtlCmDecodeMemIoResource(cm,&start)==length&&start==0x90abcdef12345678ULL);
        assert(RtlIoDecodeMemIoResource(io,&alignment,&minimum,&maximum)==length);
        assert(alignment==((ULONGLONG)0x12<<shifts[i])&&minimum==start&&maximum==0xfedcba0987654321ULL);
        assert(RtlIoDecodeMemIoResource(io,0,0,0)==length);
    }
    io[4]=0;io[5]=6;minimum=42;
    assert(!RtlIoDecodeMemIoResource(io,0,&minimum,0)&&minimum==42);
    cm[0]=2;assert(!RtlCmDecodeMemIoResource(cm,0));
    puts("PASS packed resource addresses and 32/40/48/64-bit memory lengths and alignments");
}
