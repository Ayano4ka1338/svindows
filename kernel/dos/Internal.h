#ifndef SVINDOWS_DOS_INTERNAL_H
#define SVINDOWS_DOS_INTERNAL_H

#include <svindows/Dos.h>
#include <svindows/Ex.h>
#include <svindows/Io.h>
#include <svindows/Ke.h>
#include <svindows/Rtl.h>
#include <svindows/Input.h>

typedef UINT8 U8;
typedef UINT16 U16;
typedef UINT32 U32;
typedef UINT64 U64;

#define DOS_REAL_MEMORY_SIZE 0x100000u
#define MEM_SIZE 0x08000000u /* 128 MiB flat-memory space for DOS extenders */
#define DOS_DPMI_MAX_BLOCKS 128u
#define DOS_MAX_BLOCKS 128u
#define DOS_VM_INSTRUCTION_LIMIT 100000000u
#define PSP 0x1000u
#define DOS_ENV_SEGMENT 0x0f00u
#define DOS_ENV_SIZE 0x1000u
#define CF 1u
#define AF 0x10u
#define ZF 0x40u
#define SF 0x80u
#define OF 0x800u
#define DF 0x400u
#define AX 0
#define CX 1
#define DX 2
#define BX 3
#define SP 4
#define BP 5
#define SI 6
#define DI 7
#define ES 0
#define CS 1
#define SS 2
#define DS 3
#define FS 4
#define GS 5

typedef struct _DOS_FILE {
  U8 *Data;
  SIZE_T Size, Position;
  BOOLEAN Writable;
  BOOLEAN Readable;
  U16 References;
  char Path[260];
} DOS_FILE;

typedef struct _DOS_BLOCK {
  U16 Segment, Paragraphs;
  BOOLEAN Used;
} DOS_BLOCK;

typedef struct _DOS_DESCRIPTOR {
  U32 Base;
  U32 Limit;
  U8 Access;
  U8 Flags;
  BOOLEAN Valid;
} DOS_DESCRIPTOR;

typedef struct _DOS_DPMI_BLOCK {
  U32 Base, Size;
  BOOLEAN Used;
} DOS_DPMI_BLOCK;

typedef struct _DOS_TABLE_REGISTER {
  U32 Base;
  U16 Limit;
} DOS_TABLE_REGISTER;

typedef struct _DOS_VM {
  U8 *Memory;
  U16 Registers[8], Segments[6], Ip, Flags;
  U32 Registers32[8];
  U32 Eip, Eflags, Cr0;
  U16 FpuControl, FpuStatus;
  DOS_TABLE_REGISTER Gdtr, Idtr;
  U16 Ldtr, Tr;
  U16 DpmiHostSelector;
  U16 DpmiPmVectorSelector[256];
  U32 DpmiPmVectorOffset[256];
  U16 DpmiPmExceptionSelector[32];
  U32 DpmiPmExceptionOffset[32];
  DOS_DESCRIPTOR LdtCache, SegmentCache[6];
  DOS_DESCRIPTOR DpmiDescriptors[128];
  BOOLEAN DpmiDescriptorUsed[128];
  BOOLEAN DpmiDosDescriptor[128];
  DOS_DPMI_BLOCK DpmiBlocks[DOS_DPMI_MAX_BLOCKS];
  struct {
    U16 Selector;
    U32 Offset, RegistersOffset;
    U16 RegistersSelector;
    BOOLEAN Used;
  } DpmiCallbacks[16];
  U32 DpmiNextLinear;
  BOOLEAN DpmiClient32;
  BOOLEAN ProtectedMode;
  DOS_FILE *Files[20];
  DOS_BLOCK Blocks[DOS_MAX_BLOCKS];
  U16 InitialParagraphs;
  U16 CountryCode;
  U8 CtrlBreak;
  U8 VideoMode;
  U8 VgaDac[768];
  U8 VgaDacWriteIndex, VgaDacReadIndex, VgaDacComponent;
  U8 VgaAttributeIndex, VgaSequencerIndex, VgaGraphicsIndex, VgaCrtcIndex;
  U8 VgaDacMask, VgaSpeakerControl, VgaMiscOutput;
  U8 VgaAttribute[0x15], VgaSequencer[5], VgaGraphics[9], VgaCrtc[0x19];
  U8 VgaAttributeDataPhase;
  U8 KeyboardData, KeyboardDataReady;
  U8 PicMasterMask, PicSlaveMask;
  U8 PicMasterVectorBase, PicSlaveVectorBase;
  U8 PicMasterInitStep, PicSlaveInitStep;
  U8 PitControl, PitReadPhase, PitWritePhase, PitLowByte;
  U16 PitReload, PitLatch;
  U8 PitLatched;
  U32 LastVideoTick, LastTimerTick, DosTimerPhase;
  U32 PortTraceCount;
  BOOLEAN VideoActive;
  BOOLEAN KeyboardMirrorActive;
  U16 DtaSegment, DtaOffset;
  char SearchDirectory[260], SearchPattern[11];
  U32 SearchIndex;
  U16 SearchAttributes, SearchDtaSegment, SearchDtaOffset;
  BOOLEAN SearchActive;
  U8 ExecDepth, LastChildExitCode, LastChildTerminationType;
  BOOLEAN ChildStatusAvailable;
  int Running, ExitCode, Fault;
  U32 Steps;
  U32 CurrentInstructionIp;
  U8 CurrentOpcode;
} DOS_VM;

static inline DOS_VM *current(void) {
  return (DOS_VM *)KeGetCurrentThread()->DosContext;
}
#define mem (current()->Memory)
#define r (current()->Registers)
#define r32 (current()->Registers32)
#define seg (current()->Segments)
#define ip (current()->Ip)
#define flags (current()->Flags)
#define files (current()->Files)
#define running (current()->Running)
#define exit_code (current()->ExitCode)
#define fault (current()->Fault)
#define steps (current()->Steps)

static inline U32 addr(U16 s, U16 o) {
  return (((U32)s << 4) + o) & 0xfffffu;
}
static inline U8 rb(U16 s, U16 o) { return mem[addr(s, o)]; }
static inline U16 rw(U16 s, U16 o) {
  return rb(s, o) | ((U16)rb(s, (U16)(o + 1)) << 8);
}
static inline void wb(U16 s, U16 o, U8 v) { mem[addr(s, o)] = v; }
static inline void ww(U16 s, U16 o, U16 v) {
  wb(s, o, (U8)v);
  wb(s, (U16)(o + 1), (U8)(v >> 8));
}
static inline U8 get8(unsigned n) {
  return n < 4 ? (U8)r[n] : (U8)(r[n - 4] >> 8);
}
static inline void set8(unsigned n, U8 v) {
  unsigned i = n & 3;
  r[i] = n < 4 ? (r[i] & 0xff00u) | v : (r[i] & 0xffu) | ((U16)v << 8);
}
static inline U16 word(const U8 *p) {
  return (U16)p[0] | ((U16)p[1] << 8);
}

U16 free_paragraphs_from(U16 start);
U16 dos_alloc(U16 paragraphs);
void dos_file_release(DOS_FILE *file);
BOOLEAN guest_path(U16 s, U16 o, char out[260]);
HANDLE standard_handle(U16 slot);
BOOLEAN transfer(U16 slot, void *buffer, U32 length, U32 *done, BOOLEAN write);
void output(U16 slot, const void *p, U32 n);
void say(const char *s);
void dos_name_mask(const char *source, char mask[11]);
BOOLEAN dos_find_next(void);
void fail(const char *s);
void dos_int(U8 n);
U32 DosPortIn(DOS_VM *vm, U16 port, unsigned width);
void DosPortOut(DOS_VM *vm, U16 port, U32 value, unsigned width);
void DosVideoSetMode(DOS_VM *vm, U8 mode);
void DosVideoPresent(DOS_VM *vm);
void DosVideoStop(DOS_VM *vm);
BOOLEAN KbdInputEventAvailable(void);
BOOLEAN KbdMirroredInputEventAvailable(void);
void step(void);
U8 run_vm(DOS_VM *vm);

#endif
