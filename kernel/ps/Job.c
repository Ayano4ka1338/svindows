/* Job object membership and the limit/accounting classes currently enforced
 * by the process manager. Unsupported limits fail at SetInformation time. */
#include <svindows/Ps.h>
#include <svindows/Ob.h>
#include <svindows/Mm.h>
#include <svindows/Rtl.h>
#include <nt/Ntstatus.h>

#define JOB_OBJECT_ASSIGN_PROCESS 0x0001u
#define JOB_OBJECT_SET_ATTRIBUTES 0x0002u
#define JOB_OBJECT_QUERY 0x0004u
#define JOB_OBJECT_LIMIT_ACTIVE_PROCESS 0x00000008u
#define JOB_OBJECT_NAME_EXISTS ((NTSTATUS)0x40000000u)

typedef struct _JOB_BASIC_LIMITS {
    INT64 PerProcessUserTimeLimit, PerJobUserTimeLimit;
    ULONG LimitFlags;
    ULONG Padding;
    UINT64 MinimumWorkingSetSize, MaximumWorkingSetSize;
    ULONG ActiveProcessLimit, Padding2;
    UINT64 Affinity;
    ULONG PriorityClass, SchedulingClass;
} JOB_BASIC_LIMITS;
typedef struct _JOB_EXTENDED_LIMITS {
    JOB_BASIC_LIMITS Basic;
    UINT64 IoCounters[6];
    UINT64 ProcessMemoryLimit, JobMemoryLimit;
    UINT64 PeakProcessMemoryUsed, PeakJobMemoryUsed;
} JOB_EXTENDED_LIMITS;
typedef struct _JOB_ACCOUNTING {
    INT64 TotalUserTime, TotalKernelTime;
    UINT64 ThisPeriodTotalUserTime, ThisPeriodTotalKernelTime;
    ULONG TotalPageFaultCount, TotalProcesses;
    ULONG ActiveProcesses, TotalTerminatedProcesses;
} JOB_ACCOUNTING;
typedef struct _JOB_OBJECT {
    LIST_ENTRY Members;
    JOB_EXTENDED_LIMITS Limits;
    ULONG TotalProcesses, ActiveProcesses, TotalTerminatedProcesses;
    char Name[260];
} JOB_OBJECT;

static POBJECT_TYPE g_job_type;

void PsInitializeJobObjects(void)
{
    g_job_type = ObCreateObjectType("Job", NULL);
    if (!g_job_type)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "Job object type");
    g_job_type->UnlinkNameOnLastHandle = TRUE;
}

void PsJobProcessExited(PKPROCESS process)
{
    JOB_OBJECT *job = (JOB_OBJECT *)process->JobObject;
    if (!job) return;
    RemoveEntryList(&process->JobEntry);
    InitializeListHead(&process->JobEntry);
    process->JobObject = NULL;
    if (job->ActiveProcesses) job->ActiveProcesses--;
    job->TotalTerminatedProcesses++;
    ObDereferenceObject((POBJECT)job);
}

UINT64 NtCreateJobObject(UINT64 *a)
{
    PHANDLE out = (PHANDLE)a[0];
    if (!MmProbeForWrite((UINT64)out, sizeof(*out)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    *out = NULL;
    char path[260];
    BOOLEAN named = a[2] != 0;
    if (named) {
        NTSTATUS status = PsCaptureNamedObjectPath((POBJECT_ATTRIBUTES)a[2],
                                                    path, sizeof(path));
        if (!NT_SUCCESS(status)) return (UINT64)status;
        POBJECT existing;
        status = ObLookupObjectByName(path, &existing);
        if (NT_SUCCESS(status)) {
            if (ObHeaderFromObject(existing)->Type != g_job_type) {
                ObDereferenceObject(existing);
                return (UINT64)STATUS_OBJECT_TYPE_MISMATCH;
            }
            HANDLE handle;
            status = ObCreateHandle(existing, (ACCESS_MASK)a[1], &handle);
            ObDereferenceObject(existing);
            if (NT_SUCCESS(status)) *out = handle;
            return NT_SUCCESS(status) ? (UINT64)JOB_OBJECT_NAME_EXISTS :
                                        (UINT64)status;
        }
        if (status != STATUS_OBJECT_NAME_NOT_FOUND)
            return (UINT64)status;
    }
    POBJECT object;
    NTSTATUS status = ObCreateObject(g_job_type, sizeof(JOB_OBJECT), &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    JOB_OBJECT *job = (JOB_OBJECT *)object;
    InitializeListHead(&job->Members);
    if (named) {
        char *last = path;
        for (char *p = path; *p; p++) if (*p == '\\') last = p;
        *last = 0;
        POBJECT parent;
        status = ObLookupObjectByName(path, &parent);
        if (!NT_SUCCESS(status)) {
            ObDereferenceObject(object); return (UINT64)status;
        }
        ULONG i = 0;
        do { job->Name[i] = last[1+i]; } while (job->Name[i++]);
        status = ObInsertObjectByName((struct _OBJECT_DIRECTORY *)parent,
                                      job->Name, object);
        ObDereferenceObject(parent);
        if (!NT_SUCCESS(status)) {
            ObDereferenceObject(object); return (UINT64)status;
        }
    }
    HANDLE handle;
    status = ObCreateHandle(object, (ACCESS_MASK)a[1], &handle);
    if (!NT_SUCCESS(status) && named) ObRemoveObjectName(object);
    ObDereferenceObject(object);
    if (NT_SUCCESS(status)) *out = handle;
    return (UINT64)status;
}

UINT64 NtAssignProcessToJobObject(UINT64 *a)
{
    POBJECT job_object;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)a[0],
                    JOB_OBJECT_ASSIGN_PROCESS, g_job_type, &job_object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    POBJECT process_object = NULL;
    PKPROCESS process = NULL;
    if (a[1] == ~(UINT64)0) {
        PKTHREAD thread = KeGetCurrentThread();
        if (thread) process = thread->Process;
    } else {
        status = ObReferenceObjectByHandle((HANDLE)a[1], 0, NULL,
                                            &process_object);
        if (NT_SUCCESS(status)) process = PsGetProcessFromObject(process_object);
    }
    if (!process || !process->Object) status = STATUS_INVALID_HANDLE;
    else if (process->JobObject || process->Exiting) status = STATUS_ACCESS_DENIED;
    else {
        JOB_OBJECT *job = (JOB_OBJECT *)job_object;
        if ((job->Limits.Basic.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) &&
            job->ActiveProcesses >= job->Limits.Basic.ActiveProcessLimit)
            status = STATUS_ACCESS_DENIED;
        else {
            ObReferenceObject(job_object); /* process membership keeps job alive */
            process->JobObject = job_object;
            InsertTailList(&job->Members, &process->JobEntry);
            job->TotalProcesses++;
            job->ActiveProcesses++;
            status = STATUS_SUCCESS;
        }
    }
    if (process_object) ObDereferenceObject(process_object);
    ObDereferenceObject(job_object);
    return (UINT64)status;
}

UINT64 NtSetInformationJobObject(UINT64 *a)
{
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)a[0],
                    JOB_OBJECT_SET_ATTRIBUTES, g_job_type, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    ULONG info_class = (ULONG)a[1], length = (ULONG)a[3];
    const void *buffer = (const void *)a[2];
    JOB_OBJECT *job = (JOB_OBJECT *)object;
    if (info_class == 4) {
        if (length != sizeof(ULONG)) status = STATUS_INFO_LENGTH_MISMATCH;
        else if (!MmProbeForRead((UINT64)buffer, length))
            status = STATUS_ACCESS_VIOLATION;
        else status = *(const ULONG *)buffer ? STATUS_NOT_SUPPORTED :
                                                   STATUS_SUCCESS;
    } else if (info_class == 2 || info_class == 9) {
        ULONG required = info_class == 2 ? sizeof(JOB_BASIC_LIMITS) :
                                            sizeof(JOB_EXTENDED_LIMITS);
        if (length != required) status = STATUS_INFO_LENGTH_MISMATCH;
        else if (!MmProbeForRead((UINT64)buffer, length))
            status = STATUS_ACCESS_VIOLATION;
        else {
            JOB_BASIC_LIMITS basic = *(const JOB_BASIC_LIMITS *)buffer;
            if (basic.LimitFlags & ~JOB_OBJECT_LIMIT_ACTIVE_PROCESS)
                status = STATUS_NOT_SUPPORTED;
            else if ((basic.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) &&
                     (!basic.ActiveProcessLimit ||
                      job->ActiveProcesses > basic.ActiveProcessLimit))
                status = STATUS_INVALID_PARAMETER;
            else {
                memset(&job->Limits, 0, sizeof(job->Limits));
                job->Limits.Basic = basic;
                status = STATUS_SUCCESS;
            }
        }
    } else status = STATUS_INVALID_INFO_CLASS;
    ObDereferenceObject(object);
    return (UINT64)status;
}

UINT64 NtQueryInformationJobObject(UINT64 *a)
{
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle((HANDLE)a[0],
                    JOB_OBJECT_QUERY, g_job_type, &object);
    if (!NT_SUCCESS(status)) return (UINT64)status;
    JOB_OBJECT *job = (JOB_OBJECT *)object;
    ULONG info_class = (ULONG)a[1], length = (ULONG)a[3];
    void *buffer = (void *)a[2];
    ULONG *returned = (ULONG *)a[4];
    ULONG required = info_class == 1 ? sizeof(JOB_ACCOUNTING) :
                     info_class == 2 ? sizeof(JOB_BASIC_LIMITS) :
                     info_class == 4 ? sizeof(ULONG) :
                     info_class == 9 ? sizeof(JOB_EXTENDED_LIMITS) :
                     info_class == 3 ? 8 + job->ActiveProcesses * 8 : 0;
    if (!required) status = STATUS_INVALID_INFO_CLASS;
    else if (returned && !MmProbeForWrite((UINT64)returned, sizeof(ULONG)))
        status = STATUS_ACCESS_VIOLATION;
    else if (length < required) status = STATUS_INFO_LENGTH_MISMATCH;
    else if (!MmProbeForWrite((UINT64)buffer, required))
        status = STATUS_ACCESS_VIOLATION;
    else {
        memset(buffer, 0, required);
        if (info_class == 1) {
            JOB_ACCOUNTING *account = (JOB_ACCOUNTING *)buffer;
            account->TotalProcesses = job->TotalProcesses;
            account->ActiveProcesses = job->ActiveProcesses;
            account->TotalTerminatedProcesses = job->TotalTerminatedProcesses;
        } else if (info_class == 2) memcpy(buffer, &job->Limits.Basic, required);
        else if (info_class == 9) memcpy(buffer, &job->Limits, required);
        else if (info_class == 3) {
            ULONG *counts = (ULONG *)buffer;
            counts[0] = counts[1] = job->ActiveProcesses;
            UINT64 *ids = (UINT64 *)(counts + 2);
            ULONG index = 0;
            for (PLIST_ENTRY e = job->Members.Flink; e != &job->Members;
                 e = e->Flink) {
                PKPROCESS process = CONTAINING_RECORD(e, KPROCESS, JobEntry);
                ids[index++] = process->ProcessId;
            }
        }
        status = STATUS_SUCCESS;
    }
    if (returned && MmProbeForWrite((UINT64)returned, sizeof(ULONG)))
        *returned = required;
    ObDereferenceObject(object);
    return (UINT64)status;
}
