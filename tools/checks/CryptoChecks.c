#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t BYTE;
typedef uint16_t WCHAR;
typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
typedef int BOOL_K;
#define TRUE_K 1
#define FALSE_K 0
#define __declspec(x)
static DWORD last_error;
void SetLastError(DWORD error) { last_error = error; }

#include "../../user/w/advapi32/AdvapiCrypto.inc"

static void check_digest(ULONGLONG provider, DWORD algorithm,
                         const BYTE *expected, DWORD expected_size)
{
    ULONGLONG hash = 0;
    assert(CryptCreateHash(provider, algorithm, 0, 0, &hash) && hash);
    assert(CryptHashData(hash, (const BYTE *)"a", 1, 0));
    assert(CryptHashData(hash, (const BYTE *)"bc", 2, 0));
    DWORD size = 0;
    BYTE digest[32];
    assert(!CryptGetHashParam(hash, 2, digest, &size, 0));
    assert(last_error == 234 && size == expected_size);
    assert(CryptGetHashParam(hash, 2, digest, &size, 0));
    assert(!memcmp(digest, expected, expected_size));
    assert(!CryptHashData(hash, (const BYTE *)"d", 1, 0));
    assert(last_error == NTE_BAD_HASH);
    assert(CryptDestroyHash(hash));
    assert(!CryptGetHashParam(hash, 2, digest, &size, 0));
    assert(last_error == NTE_BAD_HASH);
}

int main(void)
{
    static const BYTE md5[] = {
        0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,
        0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72};
    static const BYTE sha1[] = {
        0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
        0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    static const BYTE sha256[] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    ULONGLONG provider = 0;
    assert(CryptAcquireContextW(&provider, NULL, NULL, 24,
                                CRYPTO_VERIFY_CONTEXT));
    check_digest(provider, CALG_MD5_LOCAL, md5, sizeof(md5));
    check_digest(provider, CALG_SHA1_LOCAL, sha1, sizeof(sha1));
    check_digest(provider, CALG_SHA256_LOCAL, sha256, sizeof(sha256));
    assert(CryptReleaseContext(provider, 0));
    assert(!CryptCreateHash(provider, CALG_MD5_LOCAL, 0, 0, &provider));
    assert(last_error == NTE_BAD_UID);
    puts("PASS CryptoAPI provider lifetime and streaming MD5/SHA-1/SHA-256");
    return 0;
}
