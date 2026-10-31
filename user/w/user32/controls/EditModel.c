#include "EditModel.h"

static int length(const EDIT_CHAR *s)
{
    int n = 0;
    if (s) while (n < 0x7FFFFFFE && s[n]) ++n;
    return n;
}
static int reserve(EDIT_MODEL *m, int needed)
{
    if (needed <= m->capacity) return 1;
    unsigned capacity = m->capacity ? (unsigned)m->capacity : 64;
    while (capacity < (unsigned)needed) {
        if (capacity > 0x3FFFFFFFu) { capacity = (unsigned)needed; break; }
        capacity *= 2;
    }
    EDIT_CHAR *text = SvControlAlloc((unsigned long long)capacity * sizeof(*text));
    if (!text) return 0;
    for (int i = 0; i <= m->length && m->text; ++i) text[i] = m->text[i];
    SvControlFree(m->text);
    m->text = text; m->capacity = (int)capacity;
    return 1;
}
void SvEditSelect(EDIT_MODEL *m, int anchor, int caret)
{
    if (anchor < 0) { m->anchor = m->caret; return; }
    if (caret < 0 || caret > m->length) caret = m->length;
    if (anchor > m->length) anchor = m->length;
    m->anchor = anchor; m->caret = caret;
}
int SvEditSetText(EDIT_MODEL *m, const EDIT_CHAR *text)
{
    int n = length(text);
    if (!reserve(m, n + 1)) return 0;
    for (int i = 0; i < n; ++i) m->text[i] = text[i];
    m->text[n] = 0; m->length = n;
    m->caret = m->anchor = 0; m->modified = 0;
    return 1;
}
int SvEditReplace(EDIT_MODEL *m, const EDIT_CHAR *text, int count)
{
    int start = m->caret < m->anchor ? m->caret : m->anchor;
    int end = m->caret > m->anchor ? m->caret : m->anchor;
    if (count < 0) count = length(text);
    if (count && !text) return 0;
    unsigned long long wanted = (unsigned)m->length - (unsigned)(end - start) +
                                 (unsigned)count;
    if (wanted > m->limit || wanted >= 0x7FFFFFFEu) return 0;
    int n = (int)wanted;
    /* Copy inserted data first: EM_REPLACESEL may refer to this buffer. */
    EDIT_CHAR small[32];
    EDIT_CHAR *insert = small;
    if (count) {
        if (count > 32) insert = SvControlAlloc((unsigned long long)count * sizeof(*insert));
        if (!insert) return 0;
        for (int i = 0; i < count; ++i) insert[i] = text[i];
    }
    if (!reserve(m, n + 1)) { if (insert != small) SvControlFree(insert); return 0; }
    if (count > end - start)
        for (int i = m->length; i >= end; --i)
            m->text[i + count - (end - start)] = m->text[i];
    else
        for (int i = end; i <= m->length; ++i)
            m->text[i + count - (end - start)] = m->text[i];
    for (int i = 0; i < count; ++i) m->text[start + i] = insert[i];
    if (insert != small) SvControlFree(insert);
    m->length = n; m->text[n] = 0;
    m->caret = m->anchor = start + count; m->modified = 1;
    return 1;
}
int SvEditDelete(EDIT_MODEL *m, int backspace)
{
    int old_anchor = m->anchor, old_caret = m->caret;
    if (m->caret == m->anchor) {
        int from = m->caret, to = m->caret;
        if (backspace) {
            if (!from) return 0;
            --from;
            if (from && m->text[from] == '\n' && m->text[from - 1] == '\r') --from;
            if (from && m->text[from] >= 0xDC00 && m->text[from] <= 0xDFFF &&
                m->text[from-1] >= 0xD800 && m->text[from-1] <= 0xDBFF) --from;
        } else {
            if (to >= m->length) return 0;
            ++to;
            if (to < m->length && m->text[to-1] == '\r' && m->text[to] == '\n') ++to;
            if (to < m->length && m->text[to-1] >= 0xD800 && m->text[to-1] <= 0xDBFF &&
                m->text[to] >= 0xDC00 && m->text[to] <= 0xDFFF) ++to;
        }
        m->anchor = from; m->caret = to;
    }
    if (SvEditReplace(m, 0, 0)) return 1;
    m->anchor = old_anchor; m->caret = old_caret;
    return 0;
}
int SvEditLineStart(const EDIT_MODEL *m, int line)
{
    if (line < 0) return -1;
    if (!line) return 0;
    for (int i = 0; i < m->length; ++i)
        if (m->text[i] == '\n' && !--line) return i + 1;
    return -1;
}
int SvEditLineFromOffset(const EDIT_MODEL *m, int offset)
{
    if (offset < 0) offset = m->caret;
    if (offset > m->length) offset = m->length;
    int line = 0;
    for (int i = 0; i < offset; ++i) if (m->text[i] == '\n') ++line;
    return line;
}
int SvEditLineEnd(const EDIT_MODEL *m, int offset)
{
    if (offset < 0) offset = m->caret;
    if (offset > m->length) offset = m->length;
    while (offset < m->length && m->text[offset] != '\r' && m->text[offset] != '\n')
        ++offset;
    return offset;
}
void SvEditDispose(EDIT_MODEL *m)
{
    SvControlFree(m->text); m->text = 0;
    m->length = m->capacity = m->caret = m->anchor = 0;
}
