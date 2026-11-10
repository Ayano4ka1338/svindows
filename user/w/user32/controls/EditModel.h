#ifndef SVWINDOWS_EDIT_MODEL_H
#define SVWINDOWS_EDIT_MODEL_H
/* UTF-16 storage and selection offsets. No HWND, DC, theme or screen state. */
typedef unsigned short EDIT_CHAR;
typedef struct EDIT_MODEL {
    EDIT_CHAR *text;
    int length, capacity, caret, anchor;
    unsigned limit;
    int modified;
    void *storage;
    int (*grow_storage)(struct EDIT_MODEL *model, int capacity);
    void (*free_storage)(struct EDIT_MODEL *model);
} EDIT_MODEL;
void *SvControlAlloc(unsigned long long bytes);
void SvControlFree(void *pointer);
int SvEditSetText(EDIT_MODEL *m, const EDIT_CHAR *text);
int SvEditReplace(EDIT_MODEL *m, const EDIT_CHAR *text, int count);
void SvEditSelect(EDIT_MODEL *m, int anchor, int caret);
int SvEditDelete(EDIT_MODEL *m, int backspace);
int SvEditLineStart(const EDIT_MODEL *m, int line);
int SvEditLineFromOffset(const EDIT_MODEL *m, int offset);
int SvEditLineEnd(const EDIT_MODEL *m, int offset);
void SvEditDispose(EDIT_MODEL *m);
#endif
