#ifndef SVINDOWS_LWIP_SYS_ARCH_H
#define SVINDOWS_LWIP_SYS_ARCH_H

#include <nt/Ntdef.h>
#include <svindows/Ke.h>

typedef struct _SV_LWIP_SEM *sys_sem_t;
typedef struct _SV_LWIP_MUTEX *sys_mutex_t;
typedef struct _SV_LWIP_MBOX *sys_mbox_t;
typedef PKTHREAD sys_thread_t;
typedef UINT64 sys_prot_t;

#endif
