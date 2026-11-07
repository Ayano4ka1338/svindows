#!/usr/bin/env python3
"""Exercise the real DOS X11 backend against a deterministic wire peer."""
from pathlib import Path
import os
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
harness=r'''
#include <assert.h>
#include <stdlib.h>
void *calloc(size_t count,size_t size);
void free(void *p);
#include <string.h>
#include "kernel/dos/X11.c"
static U8 incoming[4096], outgoing[200000];
static U32 length, offset, written, closed, block_write, yields;
static U32 write_limit, write_budget;
static BOOLEAN budget_enabled;
static LINUX_SOCKET endpoint;
MOUSE_STATE MouseState;
PVOID ExAllocatePoolWithTag(POOL_TYPE type,SIZE_T size,ULONG tag) { (void)type;(void)tag;return calloc(1,size); }
void ExFreePool(PVOID p) { free(p); }
void KeYield(void) { yields++; }
int KeLog(const char *fmt,...) { (void)fmt;return 0; }
BOOLEAN LinuxInputKeyboardActive(void) { return TRUE; }
LINUX_SOCKET *LinuxLocalConnect(const char *name,UINT16 size) { assert(size==18 && name[0]==0);return &endpoint; }
void LinuxSocketClose(LINUX_SOCKET *s) { assert(s==&endpoint);closed++; }
UINT64 LinuxLocalTransfer(LINUX_SOCKET *s,BOOLEAN write,void *data,UINT64 size,UINT64 opts) {
  assert(s==&endpoint);
  if(write && block_write) { block_write=0;return (UINT64)-11; }
  if(write && budget_enabled && !write_budget) return (UINT64)-11;
  if(write && budget_enabled) write_budget--;
  if(write && write_limit && size>write_limit) size=write_limit;
  if(write) { assert(written+size<sizeof(outgoing));memcpy(outgoing+written,data,size);written+=size;return size; }
  if(offset==length) return opts==0x40?(UINT64)-11:0;
  if(size>length-offset)size=length-offset;
  /* Force short stream reads, including partial X events. */
  if(size>7)size=7;
  memcpy(data,incoming+offset,size);offset+=size;return size;
}
BOOLEAN KbdReadMirroredInputEvent(UINT32 *event) { (void)event;return FALSE; }
BOOLEAN KbdMirroredInputEventAvailable(void) { return FALSE; }
static void add_event(U8 type,U32 win,U8 detail) {
  U8 *e=incoming+length;memset(e,0,32);e[0]=type;e[1]=detail;
  put32(e+((type>=2&&type<=8)?12:type==22?8:4),win);length+=32;
}
int main(void) {
  DOS_VM *vm=calloc(1,sizeof(*vm)); vm->Running=TRUE;vm->Memory=calloc(1,MEM_SIZE);
  /* X11 setup: 24-bit TrueColor root with a 32-bit ZPixmap format. */
  incoming[0]=1;put16(incoming+6,28);U8 *s=incoming+8;
  put32(s+4,0x200000);put32(s+8,0x1fffff);s[20]=1;s[21]=1;
  s[32]=24;s[33]=32;s[34]=32;
  put32(s+40,1);put32(s+72,2);s[78]=24;s[79]=1;
  s[80]=24;put16(s+82,1);put32(s+88,2);
  put32(s+96,0xff0000);put32(s+100,0xff00);put32(s+104,0xff);
  length=120;
  incoming[length]=1;put32(incoming+length+8,100);length+=32;
  incoming[length]=1;put32(incoming+length+8,101);length+=32;
  DosXOpen(vm);assert(vm->XSession && vm->XDisplay && vm->Running);
  DOS_X11 *x=vm->XDisplay;assert(!x->Failed && x->Depth==24);
  X_PACKET p={.Op=1};DosXAgent(vm,&p);
  p=(X_PACKET){.Op=2,.Hwnd=10,.X=2,.Y=3,.Width=16,.Height=10};strcpy(p.Title,"Calculator");DosXAgent(vm,&p);
  p.Hwnd=11;p.X=20;strcpy(p.Title,"Notepad");DosXAgent(vm,&p);
  U32 a=x->Windows[0].Id,b=x->Windows[1].Id;assert(a && b && a!=b);
  p.Op=3;DosXAgent(vm,&p);assert(vm->VideoDirty);
  add_event(2,a,38);add_event(3,a,38);block_write=1;request(x,8,a);assert(yields==1);DosXPoll(vm);
  U32 event;assert(DosReadKey(vm,&event)&&event==30);assert(DosReadKey(vm,&event)&&event==(30|256));
  assert(!DosReadKey(vm,&event));
  add_event(9,a,0);add_event(2,a,38);add_event(6,a,0);
  put16(incoming+length-8,4);put16(incoming+length-6,5);DosXPoll(vm);
  assert(x->ActivationsPending==1 && !DosReadKey(vm,&event));
  assert(DosHostMouse(vm)->X==0);
  p.Op=4;DosXAgent(vm,&p);assert(p.Op==1 && p.Hwnd==10);
  p.Op=4;DosXAgent(vm,&p);assert(!p.Op && !x->ActivationsPending);DosXUpdateInput(vm);
  assert(DosReadKey(vm,&event) && event==30 && DosHostMouse(vm)->X==6);
  p.Op=8;p.Hwnd=1;DosXAgent(vm,&p);
  add_event(2,a,39);DosXPoll(vm);assert(!DosReadKey(vm,&event));
  p.Op=8;p.Hwnd=0;DosXAgent(vm,&p);assert(DosReadKey(vm,&event) && event==31);
  add_event(10,a,0);DosXPoll(vm);
  while(DosReadKey(vm,&event)) assert(event&256);
  add_event(22,a,0);incoming[length-32]|=128;
  put16(incoming+length-16,900);put16(incoming+length-14,600);
  put16(incoming+length-12,16);put16(incoming+length-10,10);
  DosXPoll(vm);p.Op=4;DosXAgent(vm,&p);assert(!p.Op); /* X-only movement */
  while(x->MouseHead!=x->MouseTail)DosXUpdateInput(vm);
  p.Op=8;p.Hwnd=1;DosXAgent(vm,&p);
  add_event(4,a,1);add_event(5,a,1);DosXPoll(vm);
  DosXUpdateInput(vm);assert(!DosHostMouse(vm)->Buttons);
  p.Op=4;DosXAgent(vm,&p);assert(p.Op==1);
  p.Op=4;DosXAgent(vm,&p);assert(!p.Op && !x->ActivationsPending);
  p.Op=8;p.Hwnd=0;DosXAgent(vm,&p);
  DosXUpdateInput(vm);assert(DosHostMouse(vm)->Buttons==1);
  DosXUpdateInput(vm);assert(DosHostMouse(vm)->Buttons==0);
  add_event(33,b,32);put32(incoming+length-24,100);put32(incoming+length-20,101);DosXPoll(vm);
  p.Op=4;DosXAgent(vm,&p);assert(p.Op==2 && p.Hwnd==11 && vm->Running);
  vm->VideoMode=0x13;vm->VgaSequencer[4]=8;vm->VgaDac[3]=63;
  vm->Memory[0xa0000]=1;assert(DosVideoRgbPixel(vm,0,0)==0xff0000);
  assert(DosVideoRgbPixel(vm,-1,0)==0 && DosVideoRgbPixel(vm,320,0)==0);
  vm->VideoMode=0x12;vm->VgaAttribute[1]=1;vm->VgaDacMask=255;
  vm->VgaPlanes[0][0]=128;assert(DosVideoRgbPixel(vm,0,0)==0xff0000);
  vm->VideoMode=0x13;vm->VgaSequencer[4]=8;
  vm->Memory[0xa0000+3*320+2]=1;
  p=(X_PACKET){.Op=10,.Hwnd=10};DosXAgent(vm,&p);
  p.Op=7;DosXAgent(vm,&p);assert(x->Windows[0].Surface[0]==0xff0000);
  vm->VgaDac[7]=63;vm->Memory[0xa0000+3*320+2]=2;
  vm->Memory[0xa0000+3*320+3]=2;
  p=(X_PACKET){.Op=9,.Hwnd=10,.X=2,.Y=3,.Width=1,.Height=1};DosXAgent(vm,&p);
  p.Op=7;DosXAgent(vm,&p);
  assert(x->Windows[0].Surface[0]==0xff0000 && x->Windows[0].Surface[1]==0x00ff00);
  vm->Memory[0xa0000+3*320+3]=0; /* Host exposes must use the independent snapshot. */
  assert(DosXPresent(vm));assert(!vm->VideoDirty && !x->Failed);
  assert(x->Windows[0].Surface[1]==0x00ff00);
  U32 stable=written;assert(DosXPresent(vm));assert(written==stable);
  /* A saturated server must stop presentation without yielding the guest.
   * Retain a partially written image before subsequent control requests. */
  x->Windows[0].Surface[0] ^= 0x123456;
  vm->VideoDirty=TRUE;write_limit=7;budget_enabled=TRUE;write_budget=1;
  U32 old_yields=yields;
  assert(DosXPresent(vm) && vm->VideoDirty && !x->Failed);
  assert(yields==old_yields && x->PendingOffset==7 && x->PendingBytes>7);
  DosXPoll(vm);assert(yields==old_yields && x->PendingOffset==7);
  budget_enabled=FALSE;
  request(x,8,a); /* must follow, never split, the image request */
  assert(!x->PendingBytes && !x->Failed);
  write_limit=0;assert(DosXPresent(vm) && !vm->VideoDirty);
  stable=written;assert(DosXPresent(vm) && written==stable);
  add_event(12,a,0);DosXPoll(vm);assert(vm->VideoDirty);
  assert(DosXPresent(vm));assert(written>stable);
  add_event(2,a,38);DosXPoll(vm);assert(DosReadKey(vm,&event) && event==30);
  p.Op=1;DosXAgent(vm,&p);p=(X_PACKET){.Op=2,.Hwnd=11,.Width=16,.Height=10};DosXAgent(vm,&p);p.Op=3;DosXAgent(vm,&p);
  assert(!x->Windows[0].Id && x->Windows[1].Id==b);
  add_event(10,a,0);DosXPoll(vm);assert(DosReadKey(vm,&event) && event==(30|256));
  DOS_VM *child=calloc(1,sizeof(*child));DosXInherit(child,vm);DosXInherit(vm,child);assert(x->References==2);
  DosXClose(child);assert(!closed && x->References==1);free(child);
  /* No ConfigureWindow sibling may reference clients reparented by TWM. */
  for(U32 at=12;at<written;) {
    U32 bytes=(U32)get16(outgoing+at+2)*4;assert(bytes>=4 && at+bytes<=written);
    if(outgoing[at]==12)assert(!(get16(outgoing+at+8)&32));
    at+=bytes;
  }
  DosXClose(vm);assert(closed==1 && !vm->XDisplay);free(vm->Memory);free(vm);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-x11-') as tmp:
    path=Path(tmp);(path/'check.c').write_text(harness)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-O2',
                    '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                    '-Wall','-Wextra','-Werror','-Wno-multichar','-Wno-unused-parameter',
                    '-I.','-Ikernel/include','-Ikernel/net/lwip','-Ibuild/lwip/src/include',
                    str(path/'check.c'),'kernel/dos/Video.c','-o',str(path/'check')],cwd=root,check=True)
    subprocess.run([str(path/'check')],check=True)
print('PASS DOS X11 captures, popup clipping, focus, ordered buttons, keyboard, close and lifecycle')
