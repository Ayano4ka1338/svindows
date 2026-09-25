#include "../include/ntseh.h"

/* A small first-chance vectored chain.  It is deliberately allocation-free so
 * exception delivery remains usable when the heap itself is damaged. */
#define RTL_MAX_VECTORED_HANDLERS 8
static NT_VECTORED_EXCEPTION_HANDLER g_vectored_handlers[RTL_MAX_VECTORED_HANDLERS];

__declspec(dllexport) void *RtlAddVectoredExceptionHandler(
    ULONG first, NT_VECTORED_EXCEPTION_HANDLER handler)
{
    if (!handler)
        return 0;
    if (first) {
        for (int i = RTL_MAX_VECTORED_HANDLERS - 1; i > 0; --i)
            g_vectored_handlers[i] = g_vectored_handlers[i - 1];
        g_vectored_handlers[0] = handler;
        return handler;
    }
    for (int i = 0; i < RTL_MAX_VECTORED_HANDLERS; ++i) {
        if (!g_vectored_handlers[i]) {
            g_vectored_handlers[i] = handler;
            return handler;
        }
    }
    return 0;
}

__declspec(dllexport) ULONG RtlRemoveVectoredExceptionHandler(void *handle)
{
    for (int i = 0; i < RTL_MAX_VECTORED_HANDLERS; ++i) {
        if ((void *)g_vectored_handlers[i] == handle) {
            for (; i + 1 < RTL_MAX_VECTORED_HANDLERS; ++i)
                g_vectored_handlers[i] = g_vectored_handlers[i + 1];
            g_vectored_handlers[RTL_MAX_VECTORED_HANDLERS - 1] = 0;
            return 1;
        }
    }
    return 0;
}

typedef struct _RUNTIME_FUNCTION64 {
    DWORD BeginAddress;
    DWORD EndAddress;
    DWORD UnwindData;
} RUNTIME_FUNCTION64;

typedef struct _DISPATCHER_CONTEXT64 {
    U64 ControlPc;
    U64 ImageBase;
    RUNTIME_FUNCTION64 *FunctionEntry;
    U64 EstablisherFrame;
    U64 TargetIp;
    NT_CONTEXT64 *ContextRecord;
    void *LanguageHandler;
    void *HandlerData;
    void *HistoryTable;
    DWORD ScopeIndex;
    DWORD Fill0;
} DISPATCHER_CONTEXT64;

typedef LONG (*LANGUAGE_HANDLER64)(NT_EXCEPTION_RECORD64 *, U64,
                                   NT_CONTEXT64 *, DISPATCHER_CONTEXT64 *);

static BYTE *rtl_image_for_address(U64 address, DWORD *size_out)
{
    U64 peb;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
    if (!peb)
        return 0;
    BYTE *ldr = *(BYTE **)(peb + 0x18);
    if (!ldr)
        return 0;
    void **head = (void **)(ldr + 0x10);
    for (void *link = *head; link && link != head; link = *(void **)link) {
        BYTE *entry = (BYTE *)link;
        BYTE *base = *(BYTE **)(entry + 0x30);
        DWORD size = *(DWORD *)(entry + 0x40);
        if (base && address >= (U64)base && address < (U64)base + size) {
            if (size_out) *size_out = size;
            return base;
        }
    }
    return 0;
}

static RUNTIME_FUNCTION64 *rtl_exception_table(BYTE *base, DWORD image_size,
                                                DWORD *count_out)
{
    if (!base || image_size < 0x100 || *(unsigned short *)base != 0x5a4d)
        return 0;
    DWORD peoff = *(DWORD *)(base + 0x3c);
    if (peoff > image_size - 0x108 || *(DWORD *)(base + peoff) != 0x4550)
        return 0;
    BYTE *optional = base + peoff + 24;
    if (*(unsigned short *)optional != 0x20b)
        return 0;
    /* PE32+ data directory 3 is the exception directory. */
    DWORD rva = *(DWORD *)(optional + 112 + 3 * 8);
    DWORD bytes = *(DWORD *)(optional + 116 + 3 * 8);
    if (!rva || bytes < sizeof(RUNTIME_FUNCTION64) ||
        rva > image_size || bytes > image_size - rva)
        return 0;
    *count_out = bytes / sizeof(RUNTIME_FUNCTION64);
    return (RUNTIME_FUNCTION64 *)(base + rva);
}

__declspec(dllexport) RUNTIME_FUNCTION64 *RtlLookupFunctionEntry(
    U64 control_pc, U64 *image_base, void *history_table)
{
    (void)history_table;
    DWORD image_size = 0, count = 0;
    BYTE *base = rtl_image_for_address(control_pc, &image_size);
    RUNTIME_FUNCTION64 *table = rtl_exception_table(base, image_size, &count);
    if (image_base) *image_base = (U64)base;
    if (!table)
        return 0;
    DWORD rva = (DWORD)(control_pc - (U64)base);
    DWORD lo = 0, hi = count;
    while (lo < hi) {
        DWORD mid = lo + (hi - lo) / 2;
        if (rva < table[mid].BeginAddress)
            hi = mid;
        else if (rva >= table[mid].EndAddress)
            lo = mid + 1;
        else
            return &table[mid];
    }
    return 0;
}

static U64 *rtl_context_register(NT_CONTEXT64 *c, BYTE reg)
{
    switch (reg) {
    case 0: return &c->Rax; case 1: return &c->Rcx;
    case 2: return &c->Rdx; case 3: return &c->Rbx;
    case 4: return &c->Rsp; case 5: return &c->Rbp;
    case 6: return &c->Rsi; case 7: return &c->Rdi;
    case 8: return &c->R8;  case 9: return &c->R9;
    case 10: return &c->R10; case 11: return &c->R11;
    case 12: return &c->R12; case 13: return &c->R13;
    case 14: return &c->R14; case 15: return &c->R15;
    default: return 0;
    }
}

__declspec(dllexport) void *RtlVirtualUnwind(
    DWORD handler_type, U64 image_base, U64 control_pc,
    RUNTIME_FUNCTION64 *function, NT_CONTEXT64 *context,
    void **handler_data, U64 *establisher_frame, void *context_pointers)
{
    (void)handler_type; (void)control_pc; (void)context_pointers;
    BYTE *info = (BYTE *)(image_base + function->UnwindData);
    BYTE flags = info[0] >> 3;
    BYTE count = info[2];
    BYTE frame = info[3] & 15;
    BYTE frame_offset = info[3] >> 4;
    U64 *frame_value = rtl_context_register(context, frame);
    if (establisher_frame)
        *establisher_frame = frame && frame_value ?
            *frame_value - (U64)frame_offset * 16 : context->Rsp;

    BYTE *codes = info + 4;
    for (DWORD i = 0; i < count; ++i) {
        BYTE op = codes[i * 2 + 1] & 15;
        BYTE opinfo = codes[i * 2 + 1] >> 4;
        U64 *reg;
        switch (op) {
        case 0: /* UWOP_PUSH_NONVOL */
            reg = rtl_context_register(context, opinfo);
            if (reg) *reg = *(U64 *)context->Rsp;
            context->Rsp += 8;
            break;
        case 1: /* UWOP_ALLOC_LARGE */
            if (opinfo == 0) {
                context->Rsp += (U64)*(unsigned short *)(codes + (i + 1) * 2) * 8;
                i += 1;
            } else {
                context->Rsp += *(DWORD *)(codes + (i + 1) * 2);
                i += 2;
            }
            break;
        case 2: /* UWOP_ALLOC_SMALL */
            context->Rsp += (U64)opinfo * 8 + 8;
            break;
        case 3: /* UWOP_SET_FPREG */
            if (frame_value)
                context->Rsp = *frame_value - (U64)frame_offset * 16;
            break;
        case 4: /* UWOP_SAVE_NONVOL */
            reg = rtl_context_register(context, opinfo);
            if (reg) *reg = *(U64 *)(context->Rsp +
                (U64)*(unsigned short *)(codes + (i + 1) * 2) * 8);
            i += 1;
            break;
        case 5: /* UWOP_SAVE_NONVOL_FAR */
            reg = rtl_context_register(context, opinfo);
            if (reg) *reg = *(U64 *)(context->Rsp +
                *(DWORD *)(codes + (i + 1) * 2));
            i += 2;
            break;
        case 8: i += 1; break; /* UWOP_SAVE_XMM128 */
        case 9: i += 2; break; /* UWOP_SAVE_XMM128_FAR */
        case 10: /* UWOP_PUSH_MACHFRAME */
            context->Rsp += opinfo ? 8 : 0;
            context->Rip = *(U64 *)context->Rsp;
            context->Rsp = *(U64 *)(context->Rsp + 24);
            return 0;
        default:
            break;
        }
    }

    DWORD aligned = (count + 1u) & ~1u;
    DWORD *language = (DWORD *)(info + 4 + aligned * 2);
    void *handler = 0;
    if (flags & 4) { /* UNW_FLAG_CHAININFO */
        RUNTIME_FUNCTION64 chained = *(RUNTIME_FUNCTION64 *)language;
        return RtlVirtualUnwind(handler_type, image_base, control_pc, &chained,
                                context, handler_data, 0, context_pointers);
    } else if (flags & 3) {
        handler = (void *)(image_base + language[0]);
        if (handler_data) *handler_data = &language[1];
    } else if (handler_data) {
        *handler_data = 0;
    }
    context->Rip = *(U64 *)context->Rsp;
    context->Rsp += 8;
    return handler;
}

typedef struct _SCOPE_RECORD64 {
    DWORD BeginAddress, EndAddress, HandlerAddress, JumpTarget;
} SCOPE_RECORD64;
typedef struct _SCOPE_TABLE64 {
    DWORD Count;
    SCOPE_RECORD64 ScopeRecord[1];
} SCOPE_TABLE64;

/* MSVC/Clang's x64 C language personality: first-pass except filters and
 * second-pass termination handlers share the same scope table. */
__declspec(dllexport) LONG __C_specific_handler(
    NT_EXCEPTION_RECORD64 *record, U64 frame, NT_CONTEXT64 *context,
    DISPATCHER_CONTEXT64 *dispatch)
{
    SCOPE_TABLE64 *table = (SCOPE_TABLE64 *)dispatch->HandlerData;
    DWORD control_rva = (DWORD)(dispatch->ControlPc - dispatch->ImageBase);
    NT_EXCEPTION_POINTERS64 pointers = { record, context };
    for (DWORD i = dispatch->ScopeIndex; table && i < table->Count; ++i) {
        SCOPE_RECORD64 *scope = &table->ScopeRecord[i];
        if (control_rva < scope->BeginAddress || control_rva >= scope->EndAddress)
            continue;
        if (record->ExceptionFlags & 6) { /* UNWINDING | EXIT_UNWIND */
            if (!scope->JumpTarget && scope->HandlerAddress) {
                NtDisplayString("[seh] termination handler\n");
                ((void (*)(BOOL, U64))
                 (dispatch->ImageBase + scope->HandlerAddress))(TRUE, frame);
            }
            continue;
        }
        if (!scope->JumpTarget)
            continue;
        LONG accept;
        if (scope->HandlerAddress == 1)
            accept = 1;
        else if (!scope->HandlerAddress)
            accept = 0;
        else
            accept = ((LONG (*)(NT_EXCEPTION_POINTERS64 *, U64))
                      (dispatch->ImageBase + scope->HandlerAddress))(&pointers,
                                                                    frame);
        if (accept < 0)
            return 0; /* continue at the faulting context */
        if (accept > 0) {
            NtDisplayString("[seh] handler accepted\n");
            context->Rip = dispatch->ImageBase + scope->JumpTarget;
            return 0; /* ExceptionContinueExecution */
        }
    }
    return 1; /* ExceptionContinueSearch */
}

static void rtl_unwind_termination(NT_EXCEPTION_RECORD64 *record,
                                   const NT_CONTEXT64 *start,
                                   U64 target_frame, U64 target_ip)
{
    NT_EXCEPTION_RECORD64 unwind_record = *record;
    unwind_record.ExceptionFlags |= 2; /* EXCEPTION_UNWINDING */
    NT_CONTEXT64 walk = *start;
    for (DWORD depth = 0; depth < 64 && walk.Rip && walk.Rsp; ++depth) {
        U64 image_base = 0;
        U64 lookup_pc = depth ? walk.Rip - 1 : walk.Rip;
        RUNTIME_FUNCTION64 *function =
            RtlLookupFunctionEntry(lookup_pc, &image_base, 0);
        if (!function) {
            walk.Rip = *(U64 *)walk.Rsp;
            walk.Rsp += 8;
            continue;
        }

        NT_CONTEXT64 caller = walk;
        void *handler_data = 0;
        U64 establisher = 0;
        void *language = RtlVirtualUnwind(2, image_base, lookup_pc, function,
                                          &caller, &handler_data, &establisher,
                                          0);
        if (language) {
            DISPATCHER_CONTEXT64 dispatch;
            memset(&dispatch, 0, sizeof(dispatch));
            dispatch.ControlPc = lookup_pc;
            dispatch.ImageBase = image_base;
            dispatch.FunctionEntry = function;
            dispatch.EstablisherFrame = establisher;
            dispatch.TargetIp = target_ip;
            dispatch.ContextRecord = &walk;
            dispatch.LanguageHandler = language;
            dispatch.HandlerData = handler_data;
            ((LANGUAGE_HANDLER64)language)(&unwind_record, establisher, &walk,
                                           &dispatch);
        }
        /* The frame that owns the target runs its own inner termination
         * handlers (up to the catch state) but is not unwound past. */
        if (establisher == target_frame)
            return;
        walk = caller;
    }
}

/* Called only by the assembly KiUserExceptionDispatcher. */
BOOL RtlDispatchException(NT_EXCEPTION_POINTERS64 *pointers)
{
    for (int i = 0; i < RTL_MAX_VECTORED_HANDLERS; ++i) {
        NT_VECTORED_EXCEPTION_HANDLER handler = g_vectored_handlers[i];
        if (handler && handler(pointers) == EXCEPTION_CONTINUE_EXECUTION)
            return TRUE;
    }

    NT_CONTEXT64 original = *pointers->ContextRecord;
    NT_CONTEXT64 walk = original;
    for (DWORD depth = 0; depth < 64 && walk.Rip && walk.Rsp; ++depth) {
        U64 image_base = 0;
        U64 lookup_pc = depth ? walk.Rip - 1 : walk.Rip;
        RUNTIME_FUNCTION64 *function =
            RtlLookupFunctionEntry(lookup_pc, &image_base, 0);
        if (!function) {
            walk.Rip = *(U64 *)walk.Rsp;
            walk.Rsp += 8;
            continue;
        }

        NT_CONTEXT64 caller = walk;
        void *handler_data = 0;
        U64 establisher = 0;
        void *language = RtlVirtualUnwind(1, image_base, lookup_pc, function,
                                          &caller, &handler_data, &establisher,
                                          0);
        if (language) {
            DISPATCHER_CONTEXT64 dispatch;
            memset(&dispatch, 0, sizeof(dispatch));
            dispatch.ControlPc = lookup_pc;
            dispatch.ImageBase = image_base;
            dispatch.FunctionEntry = function;
            dispatch.EstablisherFrame = establisher;
            dispatch.ContextRecord = &walk;
            dispatch.LanguageHandler = language;
            dispatch.HandlerData = handler_data;
            U64 control_before_handler = walk.Rip;
            LONG disposition = ((LANGUAGE_HANDLER64)language)(
                pointers->ExceptionRecord, establisher, &walk, &dispatch);
            if (disposition == 0) {
                if (walk.Rip != control_before_handler)
                    rtl_unwind_termination(pointers->ExceptionRecord, &original,
                                           establisher, walk.Rip);
                *pointers->ContextRecord = walk;
                return TRUE;
            }
        }
        walk = caller;
    }
    return FALSE;
}

/* ==================================================================== */
/* C++ exceptions (MSVC x64 personality and throw support)              */
/*                                                                      */
/* A `throw` raises exception 0xE06D7363 whose third information word   */
/* points at the module's _ThrowInfo (RTTI descriptors for the thrown    */
/* object). The C++ personality below is installed as the language       */
/* handler of every function that has try/catch or local destructors;    */
/* its handler data is the module's FuncInfo (unwind map, try/catch map  */
/* and IP-to-state map). The unwinder in RtlDispatchException already    */
/* invokes language handlers for first-pass dispatch and second-pass     */
/* unwinding, so catch matching and destructor execution live here.      */
/* ==================================================================== */

#define CXX_EXCEPTION_CODE 0xE06D7363u
#define CXX_ADJECTIVE_CATCH_ALL 0x40u

typedef struct _CXX_TYPE_DESCRIPTOR {
    void   *Vtable;
    void   *Spare;
    char    Name[1];
} CXX_TYPE_DESCRIPTOR;

typedef struct _CXX_CATCHABLE_TYPE {
    UINT32 Properties;
    UINT32 Type;        /* RVA -> CXX_TYPE_DESCRIPTOR */
    UINT32 CopyCtor;    /* RVA */
    UINT32 Dtor;        /* RVA */
    UINT32 Size;
    UINT32 Offset;
    UINT32 VBaseDtor;
} CXX_CATCHABLE_TYPE;

typedef struct _CXX_CATCHABLE_TYPE_ARRAY {
    UINT32 Count;
    UINT32 Types[1];    /* RVA -> CXX_CATCHABLE_TYPE */
} CXX_CATCHABLE_TYPE_ARRAY;

typedef struct _CXX_THROW_INFO {
    UINT32 Attributes;
    UINT32 Unwind;              /* RVA */
    UINT32 ForwardCompat;
    UINT32 CatchableTypeArray;  /* RVA -> CXX_CATCHABLE_TYPE_ARRAY */
} CXX_THROW_INFO;

typedef struct _CXX_UNWIND_MAP_ENTRY {
    INT32  ToState;
    UINT32 Action;              /* RVA -> destructor funclet */
} CXX_UNWIND_MAP_ENTRY;

typedef struct _CXX_TRY_BLOCK_MAP_ENTRY {
    INT32  TryLow, TryHigh, CatchHigh;
    UINT32 NumCatches;
    UINT32 HandlerArray;        /* RVA -> CXX_HANDLER_TYPE[] */
} CXX_TRY_BLOCK_MAP_ENTRY;

/* x64 HandlerType entries are 20 bytes: the fifth word is emitted by the
 * compiler even though only the first four are consumed here. */
typedef struct _CXX_HANDLER_TYPE {
    UINT32 Adjectives;
    UINT32 Type;                /* RVA -> CXX_TYPE_DESCRIPTOR */
    UINT32 DispCatchObj;
    UINT32 AddressOfHandler;    /* RVA -> catch funclet */
    UINT32 Reserved;
} CXX_HANDLER_TYPE;

typedef struct _CXX_IP_TO_STATE {
    UINT32 Ip;
    INT32  State;
} CXX_IP_TO_STATE;

typedef struct _CXX_FUNC_INFO {
    UINT32 MagicNumber;
    UINT32 MaxState;
    UINT32 UnwindMap;           /* RVA -> CXX_UNWIND_MAP_ENTRY[MaxState] */
    UINT32 NumTryBlocks;
    UINT32 TryBlockMap;         /* RVA -> CXX_TRY_BLOCK_MAP_ENTRY[] */
    UINT32 NumIPMapEntries;
    UINT32 IPToStateMap;        /* RVA -> CXX_IP_TO_STATE[] */
    UINT32 ESTypeList;          /* RVA */
    UINT32 EHFlags;
} CXX_FUNC_INFO;

/* Funclets are entered with the establisher frame in RDX (the second ms_abi
 * argument); the first argument is the thrown object for catches and unused
 * for destructors. A catch funclet returns the continuation address. */
typedef void *(*CXX_CATCH_FUNCLET)(void *, U64);
typedef void  (*CXX_UNWIND_FUNCLET)(void *, U64);

static int cxx_name_eq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static const char *cxx_type_name(U64 image_base, UINT32 rva)
{
    if (!rva)
        return 0;
    return ((CXX_TYPE_DESCRIPTOR *)(image_base + rva))->Name;
}

static INT32 cxx_ip_to_state(CXX_FUNC_INFO *info, U64 image_base, UINT32 rva)
{
    if (!info->IPToStateMap || !info->NumIPMapEntries)
        return -1;
    CXX_IP_TO_STATE *map = (CXX_IP_TO_STATE *)(image_base + info->IPToStateMap);
    INT32 state = -1;
    for (UINT32 i = 0; i < info->NumIPMapEntries; ++i) {
        if (map[i].Ip > rva)
            break;
        state = map[i].State;
    }
    return state;
}

/* Resolve the thrown object's RTTI name from the exception record. */
static const char *cxx_thrown_name(NT_EXCEPTION_RECORD64 *record, U64 *object)
{
    if (record->ExceptionCode != (NTSTATUS)CXX_EXCEPTION_CODE ||
        record->NumberParameters < 3)
        return 0;
    *object = record->ExceptionInformation[1];

    U64 info_va = record->ExceptionInformation[2];
    DWORD size = 0;
    BYTE *base = rtl_image_for_address(info_va, &size);
    if (!base)
        return 0;
    CXX_THROW_INFO *info = (CXX_THROW_INFO *)info_va;
    if (!info->CatchableTypeArray)
        return 0;
    CXX_CATCHABLE_TYPE_ARRAY *array =
        (CXX_CATCHABLE_TYPE_ARRAY *)(base + info->CatchableTypeArray);
    if (!array->Count)
        return 0;
    CXX_CATCHABLE_TYPE *type = (CXX_CATCHABLE_TYPE *)(base + array->Types[0]);
    return cxx_type_name((U64)base, type->Type);
}

static CXX_FUNC_INFO *cxx_func_info(DISPATCHER_CONTEXT64 *dispatch)
{
    /* The unwind-info handler-data field is an RVA to FuncInfo. */
    UINT32 rva = *(UINT32 *)dispatch->HandlerData;
    if (!rva)
        return 0;
    return (CXX_FUNC_INFO *)(dispatch->ImageBase + rva);
}

static void cxx_run_unwind_map(CXX_FUNC_INFO *info, U64 image_base,
                               U64 frame, INT32 state, INT32 target_state)
{
    if (!info->UnwindMap || info->MaxState == 0)
        return;
    CXX_UNWIND_MAP_ENTRY *map =
        (CXX_UNWIND_MAP_ENTRY *)(image_base + info->UnwindMap);
    while (state > target_state && state >= 0 &&
           state < (INT32)info->MaxState) {
        CXX_UNWIND_MAP_ENTRY *entry = &map[state];
        if (entry->Action) {
            CXX_UNWIND_FUNCLET action =
                (CXX_UNWIND_FUNCLET)(image_base + entry->Action);
            action(0, frame);
        }
        if (entry->ToState == state)
            break;
        state = entry->ToState;
    }
}

__declspec(dllexport) LONG __CxxFrameHandler3(
    NT_EXCEPTION_RECORD64 *record, U64 frame, NT_CONTEXT64 *context,
    DISPATCHER_CONTEXT64 *dispatch)
{
    CXX_FUNC_INFO *info = cxx_func_info(dispatch);
    if (!info)
        return 1; /* ExceptionContinueSearch */
    U64 image_base = dispatch->ImageBase;
    UINT32 control_rva = (UINT32)(dispatch->ControlPc - image_base);
    INT32 state = cxx_ip_to_state(info, image_base, control_rva);

    if (record->ExceptionFlags & 6) { /* UNWINDING | EXIT_UNWIND */
        /* When the target lies in this same function, control is about to
         * enter a catch here: destroy only the locals of the enclosing try
         * block, down to the state it was entered in. Any other frame is an
         * intermediate one and unwinds completely. */
        INT32 target_state = -1;
        RUNTIME_FUNCTION64 *fn = dispatch->FunctionEntry;
        if (fn && dispatch->TargetIp >= image_base + fn->BeginAddress &&
            dispatch->TargetIp < image_base + fn->EndAddress &&
            info->TryBlockMap) {
            CXX_TRY_BLOCK_MAP_ENTRY *blocks =
                (CXX_TRY_BLOCK_MAP_ENTRY *)(image_base + info->TryBlockMap);
            for (UINT32 t = 0; t < info->NumTryBlocks; ++t) {
                if (state >= blocks[t].TryLow && state <= blocks[t].TryHigh) {
                    target_state = blocks[t].TryLow;
                    break;
                }
            }
        }
        cxx_run_unwind_map(info, image_base, frame, state, target_state);
        return 1;
    }

    if (state < 0 || !info->TryBlockMap || !info->NumTryBlocks)
        return 1;
    CXX_TRY_BLOCK_MAP_ENTRY *blocks =
        (CXX_TRY_BLOCK_MAP_ENTRY *)(image_base + info->TryBlockMap);
    U64 object = 0;
    const char *thrown = cxx_thrown_name(record, &object);

    for (UINT32 t = 0; t < info->NumTryBlocks; ++t) {
        if (state < blocks[t].TryLow || state > blocks[t].TryHigh)
            continue;
        CXX_HANDLER_TYPE *handlers =
            (CXX_HANDLER_TYPE *)(image_base + blocks[t].HandlerArray);
        for (UINT32 h = 0; h < blocks[t].NumCatches; ++h) {
            CXX_HANDLER_TYPE *handler = &handlers[h];
            /* catch (...) has no type descriptor; some encodings also set the
             * catch-all adjective bit. */
            int match = handler->Type == 0 ||
                        (handler->Adjectives & CXX_ADJECTIVE_CATCH_ALL) != 0;
            if (!match && thrown)
                match = cxx_name_eq(cxx_type_name(image_base, handler->Type),
                                    thrown);
            if (!match)
                continue;
            if (handler->DispCatchObj)
                *(U64 *)(frame + handler->DispCatchObj) = object;
            CXX_CATCH_FUNCLET funclet =
                (CXX_CATCH_FUNCLET)(image_base + handler->AddressOfHandler);
            /* The catch funclet runs its catch body and returns the address to
             * continue at. RtlDispatchException then runs the second pass that
             * destroys the try block's locals; real MSVC destroys them before
             * entering the catch, so a future revision should defer the funclet
             * through a trampoline to fix the ordering. */
            context->Rip = (U64)funclet((void *)object, frame);
            context->Rsp = frame;
            return 0; /* ExceptionContinueExecution */
        }
    }
    return 1; /* ExceptionContinueSearch */
}

/* Raise a software exception and dispatch it from this call site. */
extern void     RtlCaptureContext(NT_CONTEXT64 *context);
extern NTSTATUS NtContinue(NT_CONTEXT64 *context, BOOLEAN test_alert);
extern NTSTATUS NtTerminateThread(HANDLE thread, NTSTATUS status);

__declspec(dllexport) void RtlRaiseException(NT_EXCEPTION_RECORD64 *record)
{
    NT_CONTEXT64 context;
    memset(&context, 0, sizeof(context));
    RtlCaptureContext(&context);
    record->ExceptionAddress = context.Rip;

    NT_EXCEPTION_POINTERS64 pointers;
    pointers.ExceptionRecord = record;
    pointers.ContextRecord = &context;
    if (RtlDispatchException(&pointers))
        NtContinue(&context, 0);

    NtTerminateThread(0, record->ExceptionCode);
}

/* vcruntime's throw entry point; the compiler emits a call here for `throw`. */
__declspec(dllexport) void _CxxThrowException(void *object, void *throw_info)
{
    NT_EXCEPTION_RECORD64 record;
    memset(&record, 0, sizeof(record));
    record.ExceptionCode = (NTSTATUS)CXX_EXCEPTION_CODE;
    record.ExceptionFlags = 1; /* EXCEPTION_NONCONTINUABLE */
    record.NumberParameters = 3;
    record.ExceptionInformation[0] = 0x19930520; /* MSVC EH magic */
    record.ExceptionInformation[1] = (U64)object;
    record.ExceptionInformation[2] = (U64)throw_info;
    RtlRaiseException(&record);
}
