/* Local LSA policy lookup. The account namespace is the process-token domain
 * plus well-known NT, BUILTIN and WORLD principals. Unknown SIDs are returned
 * in canonical string form with STATUS_SOME/STATUS_NONE_MAPPED. */
typedef struct { DWORD Use, Padding; UNICODE_STRING Name;
                 LONG DomainIndex; DWORD Padding2; } LSA_TRANSLATED_NAME_LOCAL;
typedef struct { UNICODE_STRING Name; void *Sid; } LSA_DOMAIN_LOCAL;
typedef struct { DWORD Entries, Padding; LSA_DOMAIN_LOCAL *Domains; }
    LSA_DOMAIN_LIST_LOCAL;
static DWORD g_lsa_generation[16];
static BYTE g_lsa_active[16];
static DWORD g_lsa_access[16];
static volatile DWORD g_lsa_lock;

static void lsa_lock(void)
{
    while (__atomic_exchange_n(&g_lsa_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static void lsa_unlock(void)
{ __atomic_store_n(&g_lsa_lock, 0, __ATOMIC_RELEASE); }

static int lsa_policy(HANDLE handle, DWORD access)
{
    ULONGLONG bits = (ULONGLONG)handle;
    DWORD index = (DWORD)bits;
    return index && index <= 16 && g_lsa_active[index - 1] &&
           g_lsa_generation[index - 1] == (DWORD)(bits >> 32) &&
           (g_lsa_access[index - 1] & access) == access;
}

__declspec(dllexport) NTSTATUS LsaOpenPolicy(const UNICODE_STRING *system,
                                             const void *attributes,
                                             DWORD access, HANDLE *policy)
{
    (void)attributes;
    if (!policy) return (NTSTATUS)0xC000000Du;
    *policy = 0;
    if (system && system->Length) return (NTSTATUS)0xC00000BBu;
    lsa_lock();
    for (DWORD i = 0; i < 16; i++) {
        if (g_lsa_active[i]) continue;
        if (!++g_lsa_generation[i]) ++g_lsa_generation[i];
        g_lsa_active[i] = 1;
        g_lsa_access[i] = access;
        *policy = (HANDLE)(((ULONGLONG)g_lsa_generation[i] << 32) | (i + 1));
        lsa_unlock();
        return 0;
    }
    lsa_unlock();
    return (NTSTATUS)0xC0000017u;
}

__declspec(dllexport) NTSTATUS LsaClose(HANDLE policy)
{
    lsa_lock();
    if (!lsa_policy(policy, 0)) {
        lsa_unlock(); return (NTSTATUS)0xC0000008u;
    }
    g_lsa_active[(DWORD)(ULONGLONG)policy - 1] = 0;
    lsa_unlock();
    return 0;
}

__declspec(dllexport) NTSTATUS LsaFreeMemory(void *memory)
{
    if (memory) LocalFree(memory);
    return 0;
}

static void lsa_string(UNICODE_STRING *out, WCHAR *target,
                       const WCHAR *source)
{
    DWORD count = 0;
    while (source[count]) { target[count] = source[count]; count++; }
    target[count] = 0;
    out->Length = (WCHAR)(count * 2);
    out->MaximumLength = (WCHAR)((count + 1) * 2);
    out->Buffer = target;
}

static int lsa_classify(const BYTE *sid, const WCHAR **name,
                        DWORD *use, DWORD *domain)
{
    BYTE count = sid[1];
    ULONGLONG authority = sid_authority(sid);
    DWORD first = count ? sid_subauthority(sid, 0) : 0;
    if (authority == 1 && count == 1 && first == 0) {
        *name = L"Everyone"; *use = 5; *domain = 0; return 1;
    }
    if (authority == 5 && count == 1) {
        *domain = 1; *use = 5;
        switch (first) {
        case 4: *name = L"INTERACTIVE"; return 1;
        case 11: *name = L"Authenticated Users"; return 1;
        case 18: *name = L"SYSTEM"; *use = 1; return 1;
        case 19: *name = L"LOCAL SERVICE"; *use = 1; return 1;
        case 20: *name = L"NETWORK SERVICE"; *use = 1; return 1;
        }
    }
    if (authority == 5 && count == 2 && first == 32) {
        *domain = 2; *use = 4;
        switch (sid_subauthority(sid, 1)) {
        case 544: *name = L"Administrators"; return 1;
        case 545: *name = L"Users"; return 1;
        case 546: *name = L"Guests"; return 1;
        case 555: *name = L"Remote Desktop Users"; return 1;
        }
    }
    if (authority == 5 && count == 5 && first == 21 &&
        sid_subauthority(sid, 1) == 1 && sid_subauthority(sid, 2) == 2 &&
        sid_subauthority(sid, 3) == 3) {
        *domain = 3;
        if (sid_subauthority(sid, 4) == 1000) {
            *name = L"User"; *use = 1; return 1;
        }
    }
    return 0;
}

__declspec(dllexport) NTSTATUS LsaLookupSids(HANDLE policy, DWORD count,
                                              void *const *sids,
                                              void **domains_out,
                                              void **names_out)
{
    if (!lsa_policy(policy, 0x800)) return (NTSTATUS)0xC0000008u;
    if (!count || count > 256 || !sids || !domains_out || !names_out)
        return (NTSTATUS)0xC000000Du;
    *domains_out = *names_out = 0;
    /* One allocation per public output, including pointed-to strings/SIDs. */
    DWORD name_bytes = count * (sizeof(LSA_TRANSLATED_NAME_LOCAL) + 512);
    BYTE *names_raw = LocalAlloc(0, name_bytes);
    BYTE *domains_raw = LocalAlloc(0, 16 + 4 * (sizeof(LSA_DOMAIN_LOCAL) + 96));
    if (!names_raw || !domains_raw) {
        LocalFree(names_raw); LocalFree(domains_raw);
        return (NTSTATUS)0xC0000017u;
    }
    LSA_TRANSLATED_NAME_LOCAL *names = (void *)names_raw;
    LSA_DOMAIN_LIST_LOCAL *domains = (void *)domains_raw;
    domains->Entries = 0; domains->Padding = 0;
    domains->Domains = (void *)(domains_raw + 16);
    static const WCHAR *domain_names[] = {
        L"WORLD", L"NT AUTHORITY", L"BUILTIN", L"SVINDOWS"
    };
    static const BYTE domain_sids[][24] = {
        {1,0,0,0,0,0,0,1},
        {1,0,0,0,0,0,0,5},
        {1,1,0,0,0,0,0,5,32},
        {1,4,0,0,0,0,0,5,21,0,0,0,1,0,0,0,2,0,0,0,3}
    };
    DWORD mapped = 0;
    LONG domain_indices[4] = {-1, -1, -1, -1};
    for (DWORD i = 0; i < count; i++) {
        BYTE *sid = sids[i];
        if (!sid_valid(sid)) {
            LocalFree(names_raw); LocalFree(domains_raw);
            return (NTSTATUS)0xC0000078u;
        }
        const WCHAR *account = 0;
        DWORD use = 8, category = 0;
        int known = lsa_classify(sid, &account, &use, &category);
        WCHAR *name_space = (WCHAR *)(names_raw +
            count * sizeof(LSA_TRANSLATED_NAME_LOCAL) + i * 512);
        if (known) {
            mapped++;
            LONG index = domain_indices[category];
            if (index < 0) {
                index = (LONG)domains->Entries;
                LSA_DOMAIN_LOCAL *record = &domains->Domains[index];
                BYTE *data = domains_raw + 16 + 4 * sizeof(LSA_DOMAIN_LOCAL)
                             + index * 96;
                WCHAR *domain_text = (WCHAR *)data;
                lsa_string(&record->Name, domain_text,
                           domain_names[category]);
                DWORD sid_size = 8 + 4 * domain_sids[category][1];
                record->Sid = data + 64;
                sid_copy(record->Sid, domain_sids[category], sid_size);
                domains->Entries++;
                domain_indices[category] = index;
            }
            names[i].DomainIndex = (LONG)index;
            names[i].Use = use;
            lsa_string(&names[i].Name, name_space, account);
        } else {
            WCHAR *sid_text = 0;
            if (!ConvertSidToStringSidW(sid, &sid_text)) {
                LocalFree(names_raw); LocalFree(domains_raw);
                return (NTSTATUS)0xC0000017u;
            }
            if (wide_len(sid_text) >= 256) {
                LocalFree(sid_text);
                LocalFree(names_raw); LocalFree(domains_raw);
                return (NTSTATUS)0xC000000Du;
            }
            lsa_string(&names[i].Name, name_space, sid_text);
            LocalFree(sid_text);
            names[i].DomainIndex = -1;
            names[i].Use = 8; /* SidTypeUnknown */
        }
        names[i].Padding = names[i].Padding2 = 0;
    }
    *domains_out = domains_raw;
    *names_out = names_raw;
    return mapped == count ? 0 :
           mapped ? (NTSTATUS)0x00000107u : (NTSTATUS)0xC0000073u;
}
