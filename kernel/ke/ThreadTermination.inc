static POBJECT KepPublishThreadTerminationWithSchedulerLock(PKTHREAD thread)
{
    thread->State = ThreadStateTerminated;
    if (!thread->TerminationObject) return NULL;
    PDISPATCHER_HEADER termination = thread->TerminationObject;
    typedef struct { DISPATCHER_HEADER Header; PKTHREAD Thread; NTSTATUS Exit; }
        THREAD_TERMINATION_RECORD;
    /* The initial thread retains its process object as its termination
     * object. That body is Header + KPROCESS, not Header + thread record;
     * writing Exit there would overwrite KPROCESS.HandleTable. */
    if (!thread->Process || termination != thread->Process->Object)
        ((THREAD_TERMINATION_RECORD *)termination)->Exit = thread->ExitStatus;
    termination->SignalState = 1;
    KiSignalObjectWithSchedulerLock(termination);
    thread->TerminationObject = NULL;
    return (POBJECT)termination;
}

