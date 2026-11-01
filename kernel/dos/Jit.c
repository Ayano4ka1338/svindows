/*
 * DOS JIT is intentionally disabled in this recovery build.
 *
 * This tombstone replaces the experimental x86-64 translator when this ZIP is
 * extracted over an older working tree. The Makefile also explicitly excludes
 * this file from C_SRC, so stale JIT implementation code cannot enter the
 * normal kernel build.
 */
#include "Internal.h"

U32 DosJitRun(DOS_VM *vm, U32 instruction_budget) {
  (void)vm;
  (void)instruction_budget;
  return 0; /* zero means use the checked interpreter */
}

void DosJitDestroy(DOS_VM *vm) {
  (void)vm;
}
