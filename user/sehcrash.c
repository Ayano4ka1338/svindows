/* Deliberately unhandled: the kernel must kill this process, not cmd.exe. */
int SehTriggerDivide(void);

int main(void)
{
    SehTriggerDivide();
    return 1;
}
