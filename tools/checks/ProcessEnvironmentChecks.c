/* Complete kernel environment blocks survive clone/replacement/failed exec
 * rollback. Real Environment.c code with an allocation-failure fixture. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <svindows/Ps.h>
#include <svindows/Ex.h>
#include <nt/Ntstatus.h>
static int fail_after=-1;
static unsigned allocations;
static UINT64 HostAcquire(PKSPIN_LOCK lock) { (void)lock; return 0; }
static void HostRelease(PKSPIN_LOCK lock,UINT64 flags) { (void)lock;(void)flags; }
#define KiAcquireRawSpinLock HostAcquire
#define KiReleaseRawSpinLock HostRelease
PVOID ExAllocatePoolWithTag(POOL_TYPE type,SIZE_T bytes,ULONG tag)
{
    (void)type;(void)tag;
    if(!fail_after) return NULL;
    if(fail_after>0) fail_after--;
    void *result=malloc(bytes); if(result) allocations++; return result;
}
void ExFreePool(void *pointer) { if(pointer) { allocations--;free(pointer); } }
#include "../../kernel/ps/Environment.c"

int main(void)
{
    KPROCESS parent={0},child={0};
    char values[120][64]; const char *entries[120];
    for(unsigned i=0;i<120;i++) { snprintf(values[i],sizeof(values[i]),"VARIABLE_%03u=complete-environment-value-%03u",i,i); entries[i]=values[i]; }
    assert(NT_SUCCESS(PsSetProcessEnvironment(&parent,entries,120)));
    SIZE_T bytes=0; const char *data=PsProcessEnvironmentData(&parent,&bytes);
    assert(bytes>2048&&strstr(data,"VARIABLE_000"));
    unsigned count=0;
    for(SIZE_T offset=0;offset<bytes&&data[offset];offset+=strlen(data+offset)+1) count++;
    assert(count==120);
    PsCloneProcessEnvironment(&child,&parent);
    assert(child.EnvironmentStorage==parent.EnvironmentStorage);
    const char *replacement[]={"REPLACED=true"};
    assert(NT_SUCCESS(PsSetProcessEnvironment(&parent,replacement,1)));
    data=PsProcessEnvironmentData(&child,&bytes);
    assert(bytes>2048&&strstr(data,"VARIABLE_000"));
    puts("PASS full environment blocks exceed legacy inline capacity and survive parent replacement");

    PS_ENVIRONMENT_SNAPSHOT snapshot;
    PsCaptureProcessEnvironment(&parent,&snapshot);
    assert(NT_SUCCESS(PsSetProcessEnvironment(&parent,entries,120)));
    PsRestoreProcessEnvironment(&parent,&snapshot);
    assert(!snapshot.Storage&&!strcmp(PsProcessEnvironmentData(&parent,&bytes),"REPLACED=true"));
    PVOID prior=parent.EnvironmentStorage; unsigned live=allocations;
    fail_after=0; assert(PsSetProcessEnvironment(&parent,entries,120)==STATUS_NO_MEMORY);
    fail_after=1; assert(PsSetProcessEnvironment(&parent,entries,120)==STATUS_NO_MEMORY);
    fail_after=-1;
    assert(parent.EnvironmentStorage==prior&&allocations==live);
    puts("PASS exec rollback restores immutable snapshot and allocation failures preserve current block");

    static const char unicode[]="TEXT=\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD1\x96\xD1\x82 \xF0\x9F\x98\x80\0";
    UINT16 wide[64]; SIZE_T units=0;
    assert(NT_SUCCESS(PsEnvironmentToWide(unicode,sizeof(unicode),NULL,0,&units)));
    assert(NT_SUCCESS(PsEnvironmentToWide(unicode,sizeof(unicode),wide,64,&units)));
    assert(wide[5]==0x43F&&wide[11]==' '&&wide[12]==0xD83D&&wide[13]==0xDE00);
    assert(wide[units-1]==0&&wide[units-2]==0);
    static const char surrogate[]="X=\xED\xA0\x80\0";
    assert(NT_SUCCESS(PsEnvironmentToWide(surrogate,sizeof(surrogate),wide,64,&units))&&wide[2]==0xD800);
    static const char bad[]="X=\xC0\xAF\0";
    assert(PsEnvironmentToWide(bad,sizeof(bad),wide,64,&units)==STATUS_INVALID_PARAMETER);
    assert(PsEnvironmentToWide(unicode,sizeof(unicode),wide,2,&units)==STATUS_BUFFER_TOO_SMALL);
    puts("PASS Unicode supplementary characters, lossless UTF-16 surrogate units and malformed UTF-8 checks");
    PsCleanupProcessEnvironment(&parent); PsCleanupProcessEnvironment(&child);
    assert(!allocations);
    puts("PASS final process environment cleanup releases every owned allocation");
    return 0;
}
