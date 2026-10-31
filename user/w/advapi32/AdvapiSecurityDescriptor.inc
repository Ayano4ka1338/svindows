/* SDDL revision 1 to self-relative SECURITY_DESCRIPTOR. This covers ordinary
 * owner/group SIDs and allow/deny/audit/mandatory-label ACEs. Unsupported
 * object/conditional ACE syntax fails instead of producing a weaker DACL. */
extern void *LocalFree(void *memory);

#define SDDL_LIMIT 2048u
typedef struct { BYTE Revision, Sbz1; unsigned short Control;
                 DWORD Owner, Group, Sacl, Dacl; } SDDL_RELATIVE_SD;

static int sddl_pair(const WCHAR *p, char a, char b)
{ return p[0] == (WCHAR)a && p[1] == (WCHAR)b; }

static int sddl_section(const WCHAR *p)
{
    return (p[0] == 'O' || p[0] == 'G' || p[0] == 'D' || p[0] == 'S') &&
           p[1] == ':';
}

static int sddl_copy_sid(const WCHAR *start, const WCHAR *end, BYTE *buffer,
                         DWORD *offset)
{
    WCHAR name[96];
    DWORD count = (DWORD)(end - start);
    if (!count || count >= 96) return 0;
    for (DWORD i = 0; i < count; i++) name[i] = start[i];
    name[count] = 0;
    void *sid = 0;
    if (!ConvertStringSidToSidW(name, &sid)) return 0;
    DWORD size = GetLengthSid(sid);
    if (!size || size > SDDL_LIMIT - *offset) {
        LocalFree(sid);
        return 0;
    }
    sid_copy(buffer + *offset, sid, size);
    *offset += size;
    LocalFree(sid);
    return 1;
}

static int sddl_rights(const WCHAR *start, const WCHAR *end, DWORD *mask)
{
    static const struct { char a, b; DWORD bit; } rights[] = {
        {'G','A',0x10000000u}, {'G','R',0x80000000u},
        {'G','W',0x40000000u}, {'G','X',0x20000000u},
        {'R','C',0x00020000u}, {'S','D',0x00010000u},
        {'W','D',0x00040000u}, {'W','O',0x00080000u},
        {'K','A',0x000f003fu}, {'K','R',0x00020019u},
        {'K','W',0x00020006u}, {'K','X',0x00020019u},
        {'F','A',0x001f01ffu}, {'F','R',0x00120089u},
        {'F','W',0x00120116u}, {'F','X',0x001200a0u},
        {'C','C',0x00000001u}, {'D','C',0x00000002u},
        {'L','C',0x00000004u}, {'S','W',0x00000008u},
        {'R','P',0x00000010u}, {'W','P',0x00000020u},
        {'D','T',0x00000040u}, {'L','O',0x00000080u},
        {'C','R',0x00000100u}, {'N','R',0x00000001u},
        {'N','W',0x00000002u}, {'N','X',0x00000004u}
    };
    *mask = 0;
    if (end - start >= 3 && start[0] == '0' &&
        (start[1] == 'x' || start[1] == 'X')) {
        for (const WCHAR *p = start + 2; p < end; p++) {
            DWORD digit = *p >= '0' && *p <= '9' ? *p - '0' :
                          *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 :
                          *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : 16;
            if (digit > 15 || *mask > 0x0fffffffu) return 0;
            *mask = (*mask << 4) | digit;
        }
        return 1;
    }
    if ((end - start) & 1) return 0;
    for (const WCHAR *p = start; p < end; p += 2) {
        DWORD i;
        for (i = 0; i < sizeof(rights) / sizeof(rights[0]); i++)
            if (sddl_pair(p, rights[i].a, rights[i].b)) break;
        if (i == sizeof(rights) / sizeof(rights[0])) return 0;
        *mask |= rights[i].bit;
    }
    return 1;
}

static int sddl_ace_flags(const WCHAR *start, const WCHAR *end, BYTE *flags)
{
    static const struct { char a, b; BYTE bit; } entries[] = {
        {'O','I',1}, {'C','I',2}, {'N','P',4}, {'I','O',8},
        {'I','D',16}, {'S','A',64}, {'F','A',128}
    };
    if ((end - start) & 1) return 0;
    *flags = 0;
    for (const WCHAR *p = start; p < end; p += 2) {
        DWORD i;
        for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++)
            if (sddl_pair(p, entries[i].a, entries[i].b)) break;
        if (i == sizeof(entries) / sizeof(entries[0])) return 0;
        *flags |= entries[i].bit;
    }
    return 1;
}

static int sddl_acl(const WCHAR *start, const WCHAR *end, BYTE *buffer,
                    DWORD *offset, unsigned short *control, int sacl)
{
    DWORD acl_start = *offset;
    if (SDDL_LIMIT - *offset < 8) return 0;
    *offset += 8;
    BYTE *acl = buffer + acl_start;
    acl[0] = 2; acl[1] = 0;
    unsigned short count = 0;
    while (start < end && *start != '(') {
        if (end - start >= 2 && sddl_pair(start, 'A', 'I')) {
            *control |= sacl ? 0x0800 : 0x0400; start += 2;
        } else if (end - start >= 2 && sddl_pair(start, 'A', 'R')) {
            *control |= sacl ? 0x0200 : 0x0100; start += 2;
        } else if (end - start >= 1 && *start == 'P') {
            *control |= sacl ? 0x2000 : 0x1000; start++;
        } else return 0;
    }
    while (start < end) {
        if (*start++ != '(' || count == 0xffff) return 0;
        const WCHAR *fields[7];
        fields[0] = start;
        int part = 0;
        while (start < end && *start != ')') {
            if (*start == ';') {
                if (++part > 5) return 0;
                fields[part] = start + 1;
            }
            start++;
        }
        if (start == end || part != 5) return 0;
        fields[6] = start;
        BYTE type;
        if (fields[1] - fields[0] == 2 && *fields[0] == 'A' && !sacl)
            type = 0;
        else if (fields[1] - fields[0] == 2 && *fields[0] == 'D' && !sacl)
            type = 1;
        else if (fields[1] - fields[0] == 3 && sddl_pair(fields[0], 'A','U')
                 && sacl) type = 2;
        else if (fields[1] - fields[0] == 3 && sddl_pair(fields[0], 'M','L')
                 && sacl) type = 17;
        else return 0;
        if (fields[4] != fields[3] + 1 || fields[5] != fields[4] + 1)
            return 0; /* object GUIDs require object ACE layout */
        BYTE flags;
        DWORD mask;
        if (!sddl_ace_flags(fields[1], fields[2] - 1, &flags) ||
            !sddl_rights(fields[2], fields[3] - 1, &mask) ||
            SDDL_LIMIT - *offset < 8) return 0;
        DWORD ace_start = *offset;
        buffer[ace_start] = type; buffer[ace_start + 1] = flags;
        *(DWORD *)(buffer + ace_start + 4) = mask;
        *offset += 8;
        if (!sddl_copy_sid(fields[5], fields[6], buffer, offset)) return 0;
        *(unsigned short *)(buffer + ace_start + 2) =
            (unsigned short)(*offset - ace_start);
        count++;
        start++;
    }
    *(unsigned short *)(acl + 2) = (unsigned short)(*offset - acl_start);
    *(unsigned short *)(acl + 4) = count;
    acl[6] = acl[7] = 0;
    return 1;
}

__declspec(dllexport) BOOL_K ConvertStringSecurityDescriptorToSecurityDescriptorW(
    const WCHAR *string, DWORD revision, void **descriptor, DWORD *length)
{
    if (!string || revision != 1 || !descriptor) {
        SetLastError(revision != 1 ? 1305 : 87);
        return FALSE_K;
    }
    *descriptor = 0;
    BYTE buffer[SDDL_LIMIT] = {0};
    SDDL_RELATIVE_SD *sd = (SDDL_RELATIVE_SD *)buffer;
    sd->Revision = 1;
    sd->Control = 0x8000; /* SE_SELF_RELATIVE */
    DWORD offset = sizeof(*sd);
    unsigned seen = 0;
    const WCHAR *p = string;
    while (*p) {
        if (!sddl_section(p)) goto invalid;
        WCHAR kind = *p;
        unsigned bit = kind == 'O' ? 1 : kind == 'G' ? 2 :
                       kind == 'D' ? 4 : 8;
        if (seen & bit) goto invalid;
        seen |= bit;
        p += 2;
        const WCHAR *end = p;
        int nesting = 0;
        while (*end) {
            if (*end == '(') nesting++;
            else if (*end == ')') {
                if (!nesting) goto invalid;
                nesting--;
            }
            if (!nesting && sddl_section(end)) break;
            end++;
        }
        if (nesting) goto invalid;
        if (kind == 'O' || kind == 'G') {
            DWORD *field = kind == 'O' ? &sd->Owner : &sd->Group;
            *field = offset;
            if (!sddl_copy_sid(p, end, buffer, &offset)) goto invalid;
        } else {
            DWORD *field = kind == 'D' ? &sd->Dacl : &sd->Sacl;
            sd->Control |= kind == 'D' ? 0x0004 : 0x0010;
            *field = offset;
            if (!sddl_acl(p, end, buffer, &offset, &sd->Control,
                          kind == 'S')) goto invalid;
        }
        p = end;
    }
    void *result = LocalAlloc(0, offset);
    if (!result) { SetLastError(8); return FALSE_K; }
    sid_copy(result, buffer, offset);
    *descriptor = result;
    if (length) *length = offset;
    return TRUE_K;
invalid:
    SetLastError(1336); /* ERROR_INVALID_ACL */
    return FALSE_K;
}
