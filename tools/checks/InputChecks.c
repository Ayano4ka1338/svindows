#include <assert.h>
#include <stdio.h>
#include <svindows/Ke.h>
static UINT64 HostIrqSave(void) { return 0; }
static void HostIrqRestore(UINT64 f) { (void)f; }
#define KiIrqSave HostIrqSave
#define KiIrqRestore HostIrqRestore
#include "../../kernel/hal/Keyboard.c"
static UINT32 take(void) { UINT32 event; assert(KbdReadInputEvent(&event)); return event; }
int main(void)
{
    kbd_decode(0x23); kbd_decode(0xA3);
    assert(take() == 0x23 && take() == (KBD_INPUT_CHARACTER|'h') && take() == 0x123);
    assert(KbdReadChar() == 'h');
    kbd_decode(0x1C); kbd_decode(0x9C);
    assert(take() == 0x1C && take() == (KBD_INPUT_CHARACTER|'\r') && take() == 0x11C);
    assert(KbdReadChar() == '\n');
    kbd_decode(0xE0); kbd_decode(0x47); kbd_decode(0xE0); kbd_decode(0xC7);
    assert(take() == 0x247 && take() == 0x347);
    UINT32 event; assert(!KbdReadInputEvent(&event));
    assert(KbdReadChar() == 27 && KbdReadChar() == '[' && KbdReadChar() == 'H');
    kbd_decode(0x1D); kbd_decode(0x1E); kbd_decode(0x9E); kbd_decode(0x9D);
    assert(take() == 0x1D && take() == 0x1E && take() == (KBD_INPUT_CHARACTER|1));
    assert(take() == 0x11E && take() == 0x11D);
    assert(KbdReadChar() == 'a');
    kbd_decode(0x2A); kbd_decode(0x1E); kbd_decode(0x9E); kbd_decode(0xAA);
    assert(take() == 0x2A && take() == 0x1E && take() == (KBD_INPUT_CHARACTER|'A'));
    assert(take() == 0x11E && take() == 0x12A);
    assert(KbdReadChar() == 'A');
    assert(!KbdReadInputEvent(&event) && !KbdDataAvailable());
    KbdInputMirrorAcquire();
    kbd_decode(0x1E); kbd_decode(0x9E);
    assert(take() == 0x1E && take() == (KBD_INPUT_CHARACTER|'a') &&
           take() == 0x11E);
    assert(KbdReadMirroredInputEvent(&event) && event == 0x1E);
    assert(KbdReadMirroredInputEvent(&event) &&
           event == (KBD_INPUT_CHARACTER|'a'));
    assert(KbdReadMirroredInputEvent(&event) && event == 0x11E);
    KbdInputMirrorRelease();
    assert(!KbdReadMirroredInputEvent(&event));
    assert(KbdReadChar() == 'a');
    puts("PASS ordered make/character/break, GUI CR, console LF/ANSI and Ctrl/Shift characters");
    return 0;
}
