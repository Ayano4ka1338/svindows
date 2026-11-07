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
#define DOS_XMS_CAPACITY (16u * 1024u * 1024u)
#define DOS_XMS_BASE 0x00110000u /* Above the HMA reserved by XMS. */
#define DOS_XMS_MAX_HANDLES 64u
#define DOS_HW_POLL_INTERVAL 128u
#define DOS_CPU_BATCH_SIZE 4096u
/* DOS 3.1-3.3 compatibility data used by INT 21h/34h, 52h and 5D06h. */
#define DOS_SYSTEM_DATA_SEGMENT 0x0600u
#define DOS_LISTS_OFFSET 0x0022u /* DOS 3.x AH=52h returns ES:BX with BX=0022h. */
#define DOS_SDA_OFFSET 0x0218u /* DOS 3.x: ListOfLists+202h minus 0Ch. */
#define DOS_SDA_SIZE 0x073cu
#define DOS_DPB_OFFSET 0x0a40u
#define DOS_SFT_OFFSET 0x0b00u
#define DOS_SFT_ENTRY_SIZE 0x35u
#define DOS_SFT_ENTRY_COUNT 20u
#define DOS_CDS_OFFSET 0x1200u
#define DOS_CDS_ENTRY_SIZE 0x45u
#define DOS_DEVICE_CLOCK_OFFSET 0x1600u
#define DOS_DEVICE_CON_OFFSET 0x1612u
#define DOS_DEVICE_AUX_OFFSET 0x1624u
#define DOS_DEVICE_PRN_OFFSET 0x1636u
#define DOS_MAX_BLOCKS 128u
#define PSP 0x1000u
#define DOS_BUILTIN_SEGMENT 0xf000u
#define DOS_BUILTIN_OFFSET(vector) (0x1000u + (U16)(vector) * 4u)
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
  U16 OwnerPsp;
} DOS_BLOCK;

typedef struct _DOS_DESCRIPTOR {
  U32 Base;
  U32 Limit;
  U8 Access;
  U8 Flags;
  BOOLEAN Valid;
} DOS_DESCRIPTOR;

#define DOS_JIT_CACHE_ENTRIES 256u
typedef struct _DOS_JIT_OP {
  U8 Kind, Opcode, Width, ModRm;
  U8 Reg, Reg2, Segment, Condition;
  U8 BaseReg, IndexReg, Scale, HasSib;
  U32 Displacement, Immediate, NextIp, Target;
} DOS_JIT_OP;
typedef struct _DOS_JIT_ENTRY {
  void *Storage;
  U8 *Code;
  U32 StartIp, CodeBase, CodeLimit, Signature;
  U32 SourceLength, InstructionCount;
  U16 Selector;
  BOOLEAN NoJit, BudgetLimited, Warmup;
  U8 WarmCount;
} DOS_JIT_ENTRY;
typedef struct _DOS_JIT_CACHE {
  DOS_JIT_ENTRY Entries[DOS_JIT_CACHE_ENTRIES];
} DOS_JIT_CACHE;

typedef struct _DOS_DPMI_BLOCK {
  U32 Base, Size;
  BOOLEAN Used;
} DOS_DPMI_BLOCK;

typedef struct _DOS_XMS_BLOCK {
  U32 Base, Size;
  BOOLEAN Used, Locked;
} DOS_XMS_BLOCK;

typedef struct _DOS_TABLE_REGISTER {
  U32 Base;
  U16 Limit;
} DOS_TABLE_REGISTER;

typedef struct _DOS_VM {
  U8 *Memory;
  U16 Registers[8], Segments[6], Ip, Flags;
  U32 Registers32[8];
  U64 MmxRegisters[8];
  U32 Eip, Eflags, Cr0;
  U16 FpuControl, FpuStatus;
  BOOLEAN FpuInitialized; /* host x87 state has been initialized for this DOS thread */
  BOOLEAN FpuTraceLogged; /* one-time guest SS:SP diagnostic for first x87 instruction */
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
  DOS_XMS_BLOCK XmsBlocks[DOS_XMS_MAX_HANDLES];
  U32 XmsUsed;
  U16 XmsA20Global, XmsA20Local;
  BOOLEAN XmsHmaOwned;
  struct {
    U16 Selector;
    U32 Offset, RegistersOffset;
    U16 RegistersSelector;
    BOOLEAN Used;
  } DpmiCallbacks[16];
  U32 DpmiNextLinear;
  BOOLEAN DpmiClient32;
  BOOLEAN ProtectedMode;
  U8 PendingException;
  U16 PendingExceptionError;
  U8 Cpl; /* Real-to-protected entry starts at ring zero regardless of CS bits. */
  DOS_FILE *Files[20];
  DOS_BLOCK Blocks[DOS_MAX_BLOCKS];
  U16 InitialParagraphs;
  U16 DosAllocationStrategy;
  U16 CurrentPsp; /* DOS current process, switchable by INT 21h/50h. */
  U16 CountryCode;
  U32 ImageSize, ImageHash;
  U16 DosVersion; /* INT 21h/AH=30 compatibility version (AL major, AH minor). */
  U8 InDosCount; /* DOS-visible SDA byte returned by INT 21h/AH=34h. */
  BOOLEAN Dos4gImage;
  U8 CtrlBreak;
  U8 VideoMode;
  U8 VgaDac[768];
  U8 VgaPlanes[4][65536];
  U8 VgaLatches[4];
  U8 VgaDacWriteIndex, VgaDacReadIndex, VgaDacComponent;
  U8 VgaAttributeIndex, VgaSequencerIndex, VgaGraphicsIndex, VgaCrtcIndex;
  U8 VgaDacMask, VgaSpeakerControl, VgaMiscOutput;
  U8 VgaAttribute[0x15], VgaSequencer[5], VgaGraphics[9], VgaCrtc[0x19];
  U8 VgaAttributeDataPhase;
  U8 KeyboardData, KeyboardDataReady;
  U16 MouseX, MouseY, MouseMinX, MouseMaxX, MouseMinY, MouseMaxY;
  U16 MouseLastX, MouseLastY;
  U16 MouseHostX, MouseHostY;
  U16 MouseMotionX, MouseMotionY;
  U16 MouseCallbackOffset, MouseCallbackSegment, MouseCallbackMask;
  struct { U16 Mask, Offset, Segment; } MouseAltCallbacks[3];
  U8 MouseAltPendingEvents[3];
  U16 MouseConditionalLeft, MouseConditionalTop;
  U16 MouseConditionalRight, MouseConditionalBottom;
  INT16 MouseHotX, MouseHotY;
  U16 MouseAndMask[64][8], MouseXorMask[64][8];
  U8 MouseCursorWidthWords, MouseCursorHeight;
  U16 MouseSensitivityX, MouseSensitivityY, MouseDoubleSpeedThreshold;
  U16 MouseRatioX, MouseRatioY;
  U16 MouseTextScreenMask, MouseTextCursorMask;
  U16 MouseSavePage;
  U16 MousePressX[3], MousePressY[3], MouseReleaseX[3], MouseReleaseY[3];
  U16 MousePressCount[3], MouseReleaseCount[3];
  U8 MouseButtons, MouseLastButtons, MouseHideCount, MousePendingEvents;
  U8 MousePage;
  U8 MouseTextCursorType, MouseTextCursorStart, MouseTextCursorEnd;
  BOOLEAN MouseConditionalOff, MouseDisabled;
  BOOLEAN MouseCallbackActive;
  U16 MouseCallbackReturnIp, MouseCallbackReturnCs, MouseCallbackSavedFlags;
  U16 MouseCallbackSavedRegisters[8], MouseCallbackSavedSegments[6];
  U32 MouseHostSeq;
  U16 MouseVideoWidth, MouseVideoHeight;
  BOOLEAN MouseInitialized;
  BOOLEAN MouseWindowsVersionProbe;
  U8 PicMasterMask, PicSlaveMask;
  U8 PicMasterVectorBase, PicSlaveVectorBase;
  U8 PicMasterInitStep, PicSlaveInitStep;
  U8 PitControl, PitReadPhase, PitWritePhase, PitLowByte;
  U16 PitReload, PitLatch;
  U8 PitLatched;
  U16 DmaRegisters[8];
  U8 DmaPageRegisters[16], DmaFlipFlop, DmaStatus;
  U8 DmaCommand, DmaMask, DmaMode;
  U8 SbDspCommand, SbDspArgsNeeded, SbDspArgCount;
  U8 SbDspResponse[8], SbDspResponseHead, SbDspResponseCount;
  U8 SbDspVersionMajor, SbDspVersionMinor;
  U8 SbDspArgs[4];
  UINT64 SbDmaAllocationPhysical;
  U8 *SbDmaBuffer;
  BOOLEAN SbDspReset, SbHardwarePresent;
  U32 LastVideoTick, LastTimerTick, DosTimerPhase, VideoPresentPhase;
  U32 VideoFpsLastTick, VideoFpsFrames;
  U32 PortTraceCount;
  U8 UartRegisters[2][8];
  U8 UartRx[2][64], UartRxHead[2], UartRxTail[2];
  U32 SerialMouseHostSeq;
  INT32 SerialMouseHostX, SerialMouseHostY;
  U8 SerialMouseButtons;
  U8 SerialMouseUart;
  BOOLEAN SerialMouseActive;
  BOOLEAN BiosFontsReady;
  void *XDisplay;
  BOOLEAN XSession;
  BOOLEAN VideoActive;
  BOOLEAN VideoDirty;
  BOOLEAN KeyboardMirrorActive;
  U16 DtaSegment, DtaOffset;
  char SearchDirectory[260], SearchPattern[11];
  U32 SearchIndex;
  U16 SearchAttributes, SearchDtaSegment, SearchDtaOffset;
  BOOLEAN SearchActive;
  U8 ExecDepth, LastChildExitCode, LastChildTerminationType;
  BOOLEAN ChildStatusAvailable;
  U16 LastDosError;
  int Running, ExitCode, Fault;
  U32 Steps;
  U32 CurrentInstructionIp;
  U8 CurrentOpcode;
  U8 InterruptShadow;
  U32 HardwarePollDeadline;
  U32 HardwarePollCountdown; /* compatibility mirror for native JIT probes */
  DOS_JIT_CACHE *JitCache;
  U32 JitLastExecuted;
} DOS_VM;

static inline DOS_VM *current(void) {
  PKTHREAD thread = KeGetCurrentThreadFast();
  return thread ? (DOS_VM *)thread->DosContext : NULL;
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

static inline U32 addr(U16 s, U32 o) {
  U32 linear = ((U32)s << 4) + o;
  return (current()->XmsA20Global || current()->XmsA20Local)
             ? linear : (linear & 0xfffffu);
}
U8 DosVideoReadMemory(DOS_VM *vm, U32 linear);
void DosVideoWriteMemory(DOS_VM *vm, U32 linear, U8 value);
static inline U8 rb(U16 s, U32 o) {
  U32 linear = addr(s, o);
  return linear >= 0xa0000u && linear < 0xb0000u ? DosVideoReadMemory(current(), linear) : mem[linear];
}
static inline U16 rw(U16 s, U32 o) {
  return rb(s, o) | ((U16)rb(s, o + 1u) << 8);
}
static inline void wb(U16 s, U32 o, U8 v) {
  U32 linear = addr(s, o);
  DOS_VM *vm = current();
  if (linear >= 0xa0000u && linear < 0xb0000u)
    DosVideoWriteMemory(vm, linear, v);
  else {
    mem[linear] = v;
    if (vm->VideoMode >= 4u && vm->VideoMode <= 6u &&
        linear >= 0xb8000u && linear < 0xbc000u)
      vm->VideoDirty = TRUE;
  }
}
static inline void ww(U16 s, U32 o, U16 v) {
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
U16 dos_largest_free(void);
void dos_file_release(DOS_FILE *file);
BOOLEAN guest_path(U16 s, U16 o, char out[260]);
HANDLE standard_handle(U16 slot);
BOOLEAN transfer(U16 slot, void *buffer, U32 length, U32 *done, BOOLEAN write);
void output(U16 slot, const void *p, U32 n);
void say(const char *s);
void dos_name_mask(const char *source, char mask[11]);
BOOLEAN dos_find_next(void);
BOOLEAN dos_find_fcb(U16 source_segment, U16 source_offset, BOOLEAN first);
void fail(const char *s);
void dos_int(U8 n);
void DosMousePoll(DOS_VM *vm);
void DosMouseSetDefaultCursor(DOS_VM *vm);
void DosMouseInitialize(DOS_VM *vm);
void DosSerialMousePoll(DOS_VM *vm);
BOOLEAN DosSerialMouseIrqPending(DOS_VM *vm, unsigned uart);
BOOLEAN DosMouseDispatchCallback(DOS_VM *vm);
BOOLEAN DosMouseCallbackReturn(DOS_VM *vm);
void DosInitializeSystemData(DOS_VM *vm, const char *current_directory);
void DosSyncSystemData(DOS_VM *vm, const char *current_directory);
void DosUpdateMemoryControlBlocks(DOS_VM *vm);
void DosUpdateSftEntry(DOS_VM *vm, U16 handle);
U32 DosPortIn(DOS_VM *vm, U16 port, unsigned width);
void DosPortOut(DOS_VM *vm, U16 port, U32 value, unsigned width);
void DosVideoSetMode(DOS_VM *vm, U8 mode);
void DosVideoInherit(DOS_VM *destination, const DOS_VM *source);
void DosVideoSetPaletteRegister(DOS_VM *vm, U8 index, U8 value);
U8 DosVideoGetPaletteRegister(DOS_VM *vm, U8 index);
void DosVideoWritePixel(DOS_VM *vm, U16 x, U16 y, U8 color);
void DosVideoInitializeFonts(DOS_VM *vm);
U8 DosVideoReadPixel(DOS_VM *vm, U16 x, U16 y);
void DosVideoPresent(DOS_VM *vm);
void DosVideoStop(DOS_VM *vm);
BOOLEAN KbdInputEventAvailable(void);
BOOLEAN KbdMirroredInputEventAvailable(void);
void step(void);
void step_vm(DOS_VM *vm);
U32 DosCpuRunBatch(DOS_VM *vm, U32 max_steps);
U32 DosJitRun(DOS_VM *vm, U32 instruction_budget);
U32 DosJitRunHot(DOS_VM *vm, U32 instruction_budget);
void DosJitDestroy(DOS_VM *vm);
void DosSimdCopy(void *destination, const void *source, SIZE_T byte_count);
void DosSimdFill32(void *destination, U32 value, SIZE_T dword_count);
U32 DosImageHash(const U8 *image, U32 size);
BOOLEAN DosImageIsDos4g(const U8 *image, U32 size);
U8 run_vm(DOS_VM *vm);

void DosXOpen(DOS_VM *vm);
void DosXClose(DOS_VM *vm);
void DosXInherit(DOS_VM *destination, const DOS_VM *source);
void DosXPoll(DOS_VM *vm);
void DosXUpdateInput(DOS_VM *vm);
BOOLEAN DosXPresent(DOS_VM *vm);
BOOLEAN DosReadKey(DOS_VM *vm, UINT32 *event);
BOOLEAN DosKeyAvailable(DOS_VM *vm);
const MOUSE_STATE *DosHostMouse(DOS_VM *vm);

void DosXAgent(DOS_VM *vm, void *packet);
void DosVideoDimensions(DOS_VM *vm, U16 *width, U16 *height);
U32 DosVideoRgbPixel(DOS_VM *vm, INT32 x, INT32 y);
#define DOS_X_PACKET_SIZE 76u
#endif
