/* WIC memory-stream PNG decoding used by Win32 resource image loaders. */
#include <stdlib.h>
#include <string.h>
typedef long HRESULT; typedef unsigned int UINT; typedef unsigned long DWORD;
typedef unsigned long long U64;
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(void*,DWORD,U64);
__declspec(dllimport) int HeapFree(void*,DWORD,void*);
__declspec(dllimport) void *HeapReAlloc(void*,DWORD,void*,U64);
void *malloc(size_t n){return HeapAlloc(GetProcessHeap(),8,n?n:1);}
void free(void*p){if(p)HeapFree(GetProcessHeap(),0,p);}
void *realloc(void*p,size_t n){return p?HeapReAlloc(GetProcessHeap(),0,p,n):malloc(n);}
int abs(int n){return n<0?-n:n;}
void *memcpy(void*d,const void*s,size_t n){unsigned char *a=d;const unsigned char*b=s;while(n--)*a++=*b++;return d;}
void *memset(void*d,int c,size_t n){volatile unsigned char*a=d;while(n--)*a++=(unsigned char)c;return d;}
int memcmp(const void*a,const void*b,size_t n){const unsigned char*x=a,*y=b;while(n--){if(*x!=*y)return *x-*y;x++;y++;}return 0;}
size_t strlen(const char*s){size_t n=0;while(s[n])n++;return n;}
int _fltused=0;
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 8192
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"
#define E_FAIL ((HRESULT)0x80004005u)
#define E_NOTIMPL ((HRESULT)0x80004001u)
#define E_INVALIDARG ((HRESULT)0x80070057u)
#define E_OUTOFMEMORY ((HRESULT)0x8007000eu)
typedef struct {DWORD A;unsigned short B,C;unsigned char D[8];} GUID;
static const GUID bgra={0x6fddc324,0x4e03,0x4bfe,{0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9,0x0f}};
typedef struct {int X,Y,Width,Height;} WIC_RECT;
typedef struct OBJ {void **Vtable;UINT Refs,Kind;const unsigned char *Data;UINT Bytes,Position,Width,Height;unsigned char *Pixels;GUID Format;} OBJ;
enum {FACTORY=1,STREAM,DECODER,SOURCE,CONVERTER,SCALER};
static void *factory_vt[28],*stream_vt[18],*decoder_vt[14],*source_vt[11],*converter_vt[11],*scaler_vt[11];
static OBJ factory;
static UINT addref(OBJ*o){return ++o->Refs;}
static UINT release(OBJ*o){if(o==&factory)return o->Refs>1?--o->Refs:1;UINT n=--o->Refs;if(!n){free(o->Pixels);free(o);}return n;}
static HRESULT query(OBJ*o,const GUID*iid,void**out){(void)iid;if(!out)return E_INVALIDARG;*out=o;addref(o);return 0;}
static HRESULT unsupported(void){return E_NOTIMPL;}
static OBJ *newobj(UINT kind,void**vt){OBJ*o=malloc(sizeof(*o));if(o){o->Vtable=vt;o->Refs=1;o->Kind=kind;o->Format=bgra;}return o;}
static HRESULT getsize(OBJ*o,UINT*w,UINT*h){if(!w||!h)return E_INVALIDARG;*w=o->Width;*h=o->Height;return 0;}
static HRESULT getformat(OBJ*o,GUID*f){if(!f)return E_INVALIDARG;*f=o->Format;return 0;}
static HRESULT resolution(OBJ*o,double*x,double*y){(void)o;if(!x||!y)return E_INVALIDARG;*x=*y=96;return 0;}
static HRESULT copy_pixels(OBJ*o,const WIC_RECT*r,UINT stride,UINT size,unsigned char*out){
 int x=r?r->X:0,y=r?r->Y:0,w=r?r->Width:(int)o->Width,h=r?r->Height:(int)o->Height;
 if(!o->Pixels||!out||x<0||y<0||w<=0||h<=0||(U64)x+w>o->Width||(U64)y+h>o->Height||stride<(UINT)w*4||(U64)stride*(h-1)+(UINT)w*4>size)return E_INVALIDARG;
 for(int j=0;j<h;j++)memcpy(out+(U64)j*stride,o->Pixels+((U64)(y+j)*o->Width+x)*4,(UINT)w*4);return 0;
}
static HRESULT stream_memory(OBJ*o,unsigned char*data,UINT bytes){if(!data||!bytes)return E_INVALIDARG;o->Data=data;o->Bytes=bytes;o->Position=0;return 0;}
static HRESULT stream_read(OBJ*o,void*out,UINT bytes,UINT*read){if(!out)return E_INVALIDARG;UINT n=o->Bytes-o->Position;if(bytes<n)n=bytes;memcpy(out,o->Data+o->Position,n);o->Position+=n;if(read)*read=n;return n==bytes?0:1;}
static HRESULT stream_seek(OBJ*o,long long offset,UINT origin,U64*position){long long p=origin==0?offset:origin==1?(long long)o->Position+offset:(long long)o->Bytes+offset;if(p<0||(U64)p>o->Bytes)return E_INVALIDARG;o->Position=(UINT)p;if(position)*position=p;return 0;}
static HRESULT create_stream(OBJ*f,void**out){(void)f;if(!out)return E_INVALIDARG;*out=newobj(STREAM,stream_vt);return *out?0:E_OUTOFMEMORY;}
static HRESULT decode_stream(OBJ*f,OBJ*stream,const GUID*vendor,UINT options,void**out){
 (void)f;(void)vendor;(void)options;if(!stream||!out)return E_INVALIDARG;*out=0;
 int w,h,n;unsigned char*p=stbi_load_from_memory(stream->Data,(int)stream->Bytes,&w,&h,&n,4);if(!p)return E_FAIL;
 OBJ*d=newobj(DECODER,decoder_vt);if(!d){free(p);return E_OUTOFMEMORY;}d->Width=w;d->Height=h;d->Pixels=p;
 for(U64 i=0;i<(U64)w*h;i++){unsigned char v=p[i*4];p[i*4]=p[i*4+2];p[i*4+2]=v;}*out=d;return 0;
}
static HRESULT frame_count(OBJ*o,UINT*n){(void)o;if(!n)return E_INVALIDARG;*n=1;return 0;}
static HRESULT get_frame(OBJ*d,UINT index,void**out){if(index||!out)return E_INVALIDARG;OBJ*s=newobj(SOURCE,source_vt);if(!s)return E_OUTOFMEMORY;s->Width=d->Width;s->Height=d->Height;U64 n=(U64)s->Width*s->Height*4;s->Pixels=malloc(n);if(!s->Pixels){release(s);return E_OUTOFMEMORY;}memcpy(s->Pixels,d->Pixels,n);*out=s;return 0;}
static HRESULT create_converter(OBJ*f,void**out){(void)f;if(!out)return E_INVALIDARG;*out=newobj(CONVERTER,converter_vt);return *out?0:E_OUTOFMEMORY;}
static HRESULT create_scaler(OBJ*f,void**out){(void)f;if(!out)return E_INVALIDARG;*out=newobj(SCALER,scaler_vt);return *out?0:E_OUTOFMEMORY;}
static HRESULT converter_init(OBJ*d,OBJ*s,const GUID*format,UINT dither,void*palette,double alpha,UINT paltype){
 (void)dither;(void)palette;(void)alpha;(void)paltype;if(!s||!format)return E_INVALIDARG;
 UINT w,h;HRESULT hr=((HRESULT(*)(OBJ*,UINT*,UINT*))s->Vtable[3])(s,&w,&h);if(hr<0)return hr;
 d->Pixels=malloc((U64)w*h*4);if(!d->Pixels)return E_OUTOFMEMORY;d->Width=w;d->Height=h;d->Format=*format;
 hr=((HRESULT(*)(OBJ*,const WIC_RECT*,UINT,UINT,unsigned char*))s->Vtable[7])(s,0,w*4,w*h*4,d->Pixels);
 if(hr<0)return hr;
 if(format->D[7]==0x10)for(U64 i=0;i<(U64)w*h;i++){unsigned a=d->Pixels[i*4+3];for(int c=0;c<3;c++)d->Pixels[i*4+c]=(unsigned char)((d->Pixels[i*4+c]*a+127)/255);}
 return 0;
}
static HRESULT can_convert(OBJ*o,const GUID*a,const GUID*b,int*yes){(void)o;(void)a;(void)b;if(!yes)return E_INVALIDARG;*yes=1;return 0;}
static HRESULT scaler_init(OBJ*d,OBJ*s,UINT w,UINT h,UINT mode){(void)mode;if(!s||!w||!h||!s->Pixels)return E_INVALIDARG;d->Width=w;d->Height=h;d->Format=s->Format;d->Pixels=malloc((U64)w*h*4);if(!d->Pixels)return E_OUTOFMEMORY;for(UINT y=0;y<h;y++)for(UINT x=0;x<w;x++)memcpy(d->Pixels+((U64)y*w+x)*4,s->Pixels+(((U64)y*s->Height/h)*s->Width+(U64)x*s->Width/w)*4,4);return 0;}
static void init_tables(void){
 static int initialized;if(initialized)return;initialized=1;
 void **tables[]={factory_vt,stream_vt,decoder_vt,source_vt,converter_vt,scaler_vt};UINT sizes[]={28,18,14,11,11,11};
 for(UINT t=0;t<6;t++){for(UINT i=0;i<sizes[t];i++)tables[t][i]=(void*)unsupported;tables[t][0]=(void*)query;tables[t][1]=(void*)addref;tables[t][2]=(void*)release;}
 factory_vt[4]=(void*)decode_stream;factory_vt[10]=(void*)create_converter;factory_vt[11]=(void*)create_scaler;factory_vt[14]=(void*)create_stream;
 stream_vt[3]=(void*)stream_read;stream_vt[5]=(void*)stream_seek;stream_vt[16]=(void*)stream_memory;
 decoder_vt[12]=(void*)frame_count;decoder_vt[13]=(void*)get_frame;
 for(UINT t=3;t<6;t++){tables[t][3]=(void*)getsize;tables[t][4]=(void*)getformat;tables[t][5]=(void*)resolution;tables[t][7]=(void*)copy_pixels;}
 converter_vt[8]=(void*)converter_init;converter_vt[9]=(void*)can_convert;scaler_vt[8]=(void*)scaler_init;
 factory.Vtable=factory_vt;factory.Refs=1;factory.Kind=FACTORY;
}
__declspec(dllexport) HRESULT WICCreateImagingFactory_Proxy(UINT version,void**out){(void)version;if(!out)return E_INVALIDARG;init_tables();*out=&factory;addref(&factory);return 0;}
