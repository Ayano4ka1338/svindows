typedef unsigned long DWORD;
typedef unsigned short WCHAR;
typedef long RPC_STATUS;
typedef struct { unsigned long Data1; unsigned short Data2, Data3; unsigned char Data4[8]; } UUID;
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(void *, DWORD, unsigned long long);
__declspec(dllimport) int HeapFree(void *, DWORD, void *);
static unsigned long g_uuid_seed = 0x53475631;
static WCHAR sv_hex[] = L"0123456789abcdef";
static void sv_put_hex(WCHAR *s, unsigned long v, int digits)
{ for (int i=digits-1;i>=0;i--) { s[i]=sv_hex[v&15]; v>>=4; } }
__declspec(dllexport) RPC_STATUS UuidCreate(UUID *u)
{
    if (!u) return 87;
    unsigned long n = ++g_uuid_seed;
    u->Data1 = 0x53560000u | (n & 0xffffu); u->Data2 = (unsigned short)(n >> 16);
    u->Data3 = 0x4000u | (unsigned short)(n & 0x0fffu);
    u->Data4[0]=0x80; u->Data4[1]=0x00;
    for (int i=2;i<8;i++) u->Data4[i]=(unsigned char)(n >> ((i-2)*4));
    return 0;
}
__declspec(dllexport) RPC_STATUS UuidToStringW(const UUID *u, WCHAR **out)
{
    if (!u || !out) return 87;
    WCHAR *s=(WCHAR*)HeapAlloc(GetProcessHeap(),0, sizeof(WCHAR)*37);
    if (!s) return 14;
    sv_put_hex(s,u->Data1,8); s[8]='-'; sv_put_hex(s+9,u->Data2,4);
    s[13]='-'; sv_put_hex(s+14,u->Data3,4); s[18]='-';
    sv_put_hex(s+19,u->Data4[0],2); sv_put_hex(s+21,u->Data4[1],2); s[23]='-';
    for (int i=2;i<8;i++) sv_put_hex(s+24+(i-2)*2,u->Data4[i],2);
    s[36]=0; *out=s; return 0;
}
__declspec(dllexport) RPC_STATUS RpcStringFreeW(WCHAR **s)
{
    if (!s) return 87;
    if (*s) HeapFree(GetProcessHeap(),0,*s);
    *s=0; return 0;
}
