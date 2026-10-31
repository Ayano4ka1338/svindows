#ifndef _SVINDOWS_DOS_H_
#define _SVINDOWS_DOS_H_
#include <nt/Ntdef.h>
#include <nt/Ntstatus.h>
BOOLEAN DosIsImage(const char *filename, const UINT8 *image, SIZE_T size);
NTSTATUS DosCreateVm(const UINT8 *image, SIZE_T size, const char *command,
                     PVOID *out);
void DosDestroyVm(PVOID context);
void DosRun(PVOID context);
#endif
