/* Sample the interrupted mode before dispatching IRQ0 or switching threads.
 * Idle is kernel time on Windows; process totals survive thread teardown. */
void KeAccountClockTick(BOOLEAN user_mode)
{
    PKPRCB cpu=KeGetCurrentProcessorBlock();
    PKTHREAD thread=cpu->CurrentThread;
    if(!thread)return;
    const UINT64 elapsed=KE_TIME_UNITS_PER_TICK;
    if(thread==cpu->IdleThread){
        __atomic_add_fetch(&cpu->IdleTime,elapsed,__ATOMIC_RELAXED);
        __atomic_add_fetch(&cpu->KernelTime,elapsed,__ATOMIC_RELAXED);
        return;
    }
    if(user_mode){
        __atomic_add_fetch(&cpu->UserTime,elapsed,__ATOMIC_RELAXED);
        __atomic_add_fetch(&thread->UserTime,elapsed,__ATOMIC_RELAXED);
        if(thread->Process)__atomic_add_fetch(&thread->Process->UserTime,elapsed,__ATOMIC_RELAXED);
    }else{
        __atomic_add_fetch(&cpu->KernelTime,elapsed,__ATOMIC_RELAXED);
        __atomic_add_fetch(&thread->KernelTime,elapsed,__ATOMIC_RELAXED);
        if(thread->Process)__atomic_add_fetch(&thread->Process->KernelTime,elapsed,__ATOMIC_RELAXED);
    }
}
