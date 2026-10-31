/* Streaming MD5, SHA-1 and SHA-256 for CryptoAPI hash providers. The provider
 * context is ephemeral (CRYPT_VERIFYCONTEXT); key containers are not claimed. */
#define CRYPTO_PROVIDERS 32
#define CRYPTO_HASHES 64
#define CRYPTO_VERIFY_CONTEXT 0xf0000000u
#define CALG_MD5_LOCAL 0x00008003u
#define CALG_SHA1_LOCAL 0x00008004u
#define CALG_SHA256_LOCAL 0x0000800cu
#define NTE_BAD_UID 0x80090001u
#define NTE_BAD_HASH 0x80090002u
#define NTE_BAD_ALGID 0x80090008u
#define NTE_BAD_FLAGS 0x80090009u
#define NTE_BAD_PROV_TYPE 0x80090014u
#define NTE_BAD_KEYSET 0x80090016u

typedef struct {
    DWORD Generation, Active, Type;
} CRYPTO_PROVIDER;
typedef struct {
    DWORD Generation, Active, Algorithm, Finalized;
    DWORD State[8];
    ULONGLONG Bytes;
    BYTE Block[64], Used;
    BYTE Digest[32];
} CRYPTO_HASH;
static CRYPTO_PROVIDER g_crypto_providers[CRYPTO_PROVIDERS];
static CRYPTO_HASH g_crypto_hashes[CRYPTO_HASHES];
static volatile DWORD g_crypto_lock;

static void crypto_lock(void)
{
    while (__atomic_exchange_n(&g_crypto_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static void crypto_unlock(void)
{ __atomic_store_n(&g_crypto_lock, 0, __ATOMIC_RELEASE); }

static CRYPTO_PROVIDER *crypto_provider(ULONGLONG handle)
{
    DWORD index = (DWORD)handle;
    if (!index || index > CRYPTO_PROVIDERS) return 0;
    CRYPTO_PROVIDER *slot = &g_crypto_providers[index - 1];
    return slot->Active && slot->Generation == (DWORD)(handle >> 32)
        ? slot : 0;
}
static CRYPTO_HASH *crypto_hash(ULONGLONG handle)
{
    DWORD index = (DWORD)handle;
    if (!index || index > CRYPTO_HASHES) return 0;
    CRYPTO_HASH *slot = &g_crypto_hashes[index - 1];
    return slot->Active && slot->Generation == (DWORD)(handle >> 32)
        ? slot : 0;
}

static DWORD crypto_rotl(DWORD x, DWORD n)
{ return (x << n) | (x >> (32 - n)); }
static DWORD crypto_rotr(DWORD x, DWORD n)
{ return (x >> n) | (x << (32 - n)); }
static DWORD crypto_word_le(const BYTE *p)
{ return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) |
         ((DWORD)p[3] << 24); }
static DWORD crypto_word_be(const BYTE *p)
{ return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) |
         ((DWORD)p[2] << 8) | p[3]; }
static void crypto_store_le(BYTE *p, DWORD value)
{ for (DWORD i = 0; i < 4; i++) p[i] = (BYTE)(value >> (i * 8)); }
static void crypto_store_be(BYTE *p, DWORD value)
{ for (DWORD i = 0; i < 4; i++) p[i] = (BYTE)(value >> ((3 - i) * 8)); }

static const DWORD crypto_md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
    0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
    0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
    0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
    0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
    0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
    0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
    0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const BYTE crypto_md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};
static const DWORD crypto_sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void crypto_transform(CRYPTO_HASH *hash, const BYTE block[64])
{
    DWORD w[80];
    if (hash->Algorithm == CALG_MD5_LOCAL) {
        for (DWORD i = 0; i < 16; i++) w[i] = crypto_word_le(block + i * 4);
        DWORD a = hash->State[0], b = hash->State[1];
        DWORD c = hash->State[2], d = hash->State[3];
        for (DWORD i = 0; i < 64; i++) {
            DWORD f, g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5*i+1)%16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3*i+5)%16; }
            else { f = c ^ (b | ~d); g = (7*i)%16; }
            DWORD next = b + crypto_rotl(a + f + crypto_md5_k[i] + w[g],
                                         crypto_md5_s[i]);
            a = d; d = c; c = b; b = next;
        }
        hash->State[0] += a; hash->State[1] += b;
        hash->State[2] += c; hash->State[3] += d;
        return;
    }
    for (DWORD i = 0; i < 16; i++) w[i] = crypto_word_be(block + i * 4);
    if (hash->Algorithm == CALG_SHA1_LOCAL) {
        for (DWORD i = 16; i < 80; i++)
            w[i] = crypto_rotl(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
        DWORD a = hash->State[0], b = hash->State[1], c = hash->State[2];
        DWORD d = hash->State[3], e = hash->State[4];
        for (DWORD i = 0; i < 80; i++) {
            DWORD f = i < 20 ? ((b & c) | (~b & d)) :
                      i < 40 ? (b ^ c ^ d) :
                      i < 60 ? ((b & c) | (b & d) | (c & d)) :
                               (b ^ c ^ d);
            DWORD k = i < 20 ? 0x5a827999 : i < 40 ? 0x6ed9eba1 :
                      i < 60 ? 0x8f1bbcdc : 0xca62c1d6;
            DWORD t = crypto_rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = crypto_rotl(b, 30); b = a; a = t;
        }
        hash->State[0] += a; hash->State[1] += b;
        hash->State[2] += c; hash->State[3] += d; hash->State[4] += e;
        return;
    }
    for (DWORD i = 16; i < 64; i++) {
        DWORD x = w[i-15], y = w[i-2];
        w[i] = w[i-16] + (crypto_rotr(x,7)^crypto_rotr(x,18)^(x>>3)) +
               w[i-7] + (crypto_rotr(y,17)^crypto_rotr(y,19)^(y>>10));
    }
    DWORD a=hash->State[0],b=hash->State[1],c=hash->State[2],d=hash->State[3];
    DWORD e=hash->State[4],f=hash->State[5],g=hash->State[6],h=hash->State[7];
    for (DWORD i = 0; i < 64; i++) {
        DWORD s1=crypto_rotr(e,6)^crypto_rotr(e,11)^crypto_rotr(e,25);
        DWORD t1=h+s1+((e&f)^(~e&g))+crypto_sha256_k[i]+w[i];
        DWORD s0=crypto_rotr(a,2)^crypto_rotr(a,13)^crypto_rotr(a,22);
        DWORD t2=s0+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    hash->State[0]+=a;hash->State[1]+=b;hash->State[2]+=c;hash->State[3]+=d;
    hash->State[4]+=e;hash->State[5]+=f;hash->State[6]+=g;hash->State[7]+=h;
}

static void crypto_init(CRYPTO_HASH *hash, DWORD algorithm)
{
    static const DWORD md5[] = {0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    static const DWORD sha1[] = {0x67452301,0xefcdab89,0x98badcfe,0x10325476,
                                 0xc3d2e1f0};
    static const DWORD sha256[] = {0x6a09e667,0xbb67ae85,0x3c6ef372,
        0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const DWORD *start = algorithm == CALG_MD5_LOCAL ? md5 :
                         algorithm == CALG_SHA1_LOCAL ? sha1 : sha256;
    DWORD count = algorithm == CALG_MD5_LOCAL ? 4 :
                  algorithm == CALG_SHA1_LOCAL ? 5 : 8;
    for (DWORD i = 0; i < count; i++) hash->State[i] = start[i];
    hash->Algorithm = algorithm;
    hash->Bytes = 0; hash->Used = 0; hash->Finalized = 0;
}

static void crypto_update(CRYPTO_HASH *hash, const BYTE *data, DWORD size)
{
    hash->Bytes += size;
    for (DWORD i = 0; i < size; i++) {
        hash->Block[hash->Used++] = data[i];
        if (hash->Used == 64) {
            crypto_transform(hash, hash->Block);
            hash->Used = 0;
        }
    }
}
static DWORD crypto_digest_size(DWORD algorithm)
{ return algorithm == CALG_MD5_LOCAL ? 16 :
         algorithm == CALG_SHA1_LOCAL ? 20 : 32; }

static void crypto_finish(CRYPTO_HASH *hash)
{
    if (hash->Finalized) return;
    ULONGLONG bits = hash->Bytes * 8;
    hash->Block[hash->Used++] = 0x80;
    if (hash->Used > 56) {
        while (hash->Used < 64) hash->Block[hash->Used++] = 0;
        crypto_transform(hash, hash->Block);
        hash->Used = 0;
    }
    while (hash->Used < 56) hash->Block[hash->Used++] = 0;
    for (DWORD i = 0; i < 8; i++)
        hash->Block[56+i] = (BYTE)(bits >>
            (hash->Algorithm == CALG_MD5_LOCAL ? 8*i : 8*(7-i)));
    crypto_transform(hash, hash->Block);
    DWORD words = crypto_digest_size(hash->Algorithm) / 4;
    for (DWORD i = 0; i < words; i++) {
        if (hash->Algorithm == CALG_MD5_LOCAL)
            crypto_store_le(hash->Digest + 4*i, hash->State[i]);
        else crypto_store_be(hash->Digest + 4*i, hash->State[i]);
    }
    hash->Finalized = 1;
}

__declspec(dllexport) BOOL_K CryptAcquireContextW(ULONGLONG *out,
    const WCHAR *container, const WCHAR *provider, DWORD type, DWORD flags)
{
    (void)provider;
    if (!out) { SetLastError(87); return FALSE_K; }
    *out = 0;
    if (type != 1 && type != 24) {
        SetLastError(NTE_BAD_PROV_TYPE); return FALSE_K;
    }
    if (flags != CRYPTO_VERIFY_CONTEXT || (container && *container)) {
        SetLastError(flags != CRYPTO_VERIFY_CONTEXT ? NTE_BAD_FLAGS :
                     NTE_BAD_KEYSET);
        return FALSE_K;
    }
    crypto_lock();
    for (DWORD i = 0; i < CRYPTO_PROVIDERS; i++) {
        CRYPTO_PROVIDER *slot = &g_crypto_providers[i];
        if (slot->Active) continue;
        if (!++slot->Generation) ++slot->Generation;
        slot->Type = type; slot->Active = 1;
        *out = ((ULONGLONG)slot->Generation << 32) | (i + 1);
        crypto_unlock();
        return TRUE_K;
    }
    crypto_unlock();
    SetLastError(8); return FALSE_K;
}

__declspec(dllexport) BOOL_K CryptReleaseContext(ULONGLONG provider,
                                                  DWORD flags)
{
    if (flags) { SetLastError(NTE_BAD_FLAGS); return FALSE_K; }
    crypto_lock();
    CRYPTO_PROVIDER *slot = crypto_provider(provider);
    if (!slot) { crypto_unlock(); SetLastError(NTE_BAD_UID); return FALSE_K; }
    slot->Active = 0;
    crypto_unlock();
    return TRUE_K;
}

__declspec(dllexport) BOOL_K CryptCreateHash(ULONGLONG provider,
    DWORD algorithm, ULONGLONG key, DWORD flags, ULONGLONG *out)
{
    if (!out || flags || key) { SetLastError(87); return FALSE_K; }
    *out = 0;
    crypto_lock();
    CRYPTO_PROVIDER *context = crypto_provider(provider);
    if (!context) {
        crypto_unlock(); SetLastError(NTE_BAD_UID); return FALSE_K;
    }
    if (algorithm != CALG_MD5_LOCAL && algorithm != CALG_SHA1_LOCAL &&
        algorithm != CALG_SHA256_LOCAL) {
        crypto_unlock(); SetLastError(NTE_BAD_ALGID); return FALSE_K;
    }
    if (algorithm == CALG_SHA256_LOCAL && context->Type != 24) {
        crypto_unlock(); SetLastError(NTE_BAD_ALGID); return FALSE_K;
    }
    for (DWORD i = 0; i < CRYPTO_HASHES; i++) {
        CRYPTO_HASH *slot = &g_crypto_hashes[i];
        if (slot->Active) continue;
        if (!++slot->Generation) ++slot->Generation;
        crypto_init(slot, algorithm);
        slot->Active = 1;
        *out = ((ULONGLONG)slot->Generation << 32) | (i + 1);
        crypto_unlock();
        return TRUE_K;
    }
    crypto_unlock();
    SetLastError(8); return FALSE_K;
}

__declspec(dllexport) BOOL_K CryptHashData(ULONGLONG handle,
    const BYTE *data, DWORD size, DWORD flags)
{
    if (flags || (size && !data)) { SetLastError(87); return FALSE_K; }
    crypto_lock();
    CRYPTO_HASH *hash = crypto_hash(handle);
    if (!hash || hash->Finalized) {
        crypto_unlock(); SetLastError(NTE_BAD_HASH); return FALSE_K;
    }
    if (size) crypto_update(hash, data, size);
    crypto_unlock();
    return TRUE_K;
}

__declspec(dllexport) BOOL_K CryptGetHashParam(ULONGLONG handle,
    DWORD param, BYTE *data, DWORD *size, DWORD flags)
{
    if (!size || flags) { SetLastError(87); return FALSE_K; }
    crypto_lock();
    CRYPTO_HASH *hash = crypto_hash(handle);
    if (!hash) { crypto_unlock(); SetLastError(NTE_BAD_HASH); return FALSE_K; }
    DWORD required = param == 2 ? crypto_digest_size(hash->Algorithm) : 4;
    if (param != 1 && param != 2 && param != 4) {
        crypto_unlock(); SetLastError(87); return FALSE_K;
    }
    DWORD capacity = *size;
    *size = required;
    if (!data || capacity < required) {
        crypto_unlock(); SetLastError(234); return FALSE_K;
    }
    if (param == 2) {
        crypto_finish(hash);
        for (DWORD i = 0; i < required; i++) data[i] = hash->Digest[i];
    } else {
        DWORD value = param == 1 ? hash->Algorithm :
                      crypto_digest_size(hash->Algorithm);
        crypto_store_le(data, value);
    }
    crypto_unlock();
    return TRUE_K;
}

__declspec(dllexport) BOOL_K CryptDestroyHash(ULONGLONG handle)
{
    crypto_lock();
    CRYPTO_HASH *hash = crypto_hash(handle);
    if (!hash) { crypto_unlock(); SetLastError(NTE_BAD_HASH); return FALSE_K; }
    hash->Active = 0;
    for (DWORD i = 0; i < sizeof(hash->State); i++)
        ((BYTE *)hash->State)[i] = 0;
    crypto_unlock();
    return TRUE_K;
}
