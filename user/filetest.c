/* FAT32 write-path regression program. */
typedef void *HANDLE;
typedef void *LPVOID;
typedef unsigned long DWORD;
typedef int BOOL;

#define INVALID_HANDLE_VALUE ((HANDLE)(~0ULL))
#define GENERIC_READ          0x80000000u
#define GENERIC_WRITE         0x40000000u
#define CREATE_ALWAYS         2u
#define OPEN_EXISTING         3u
#define STD_OUTPUT_HANDLE     ((DWORD)-11)

__declspec(dllimport) HANDLE CreateFileA(const char *, DWORD, DWORD, LPVOID,
                                         DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *,
                                     LPVOID);
__declspec(dllimport) BOOL CloseHandle(HANDLE);
__declspec(dllimport) HANDLE GetStdHandle(DWORD);
__declspec(dllimport) BOOL CreateDirectoryA(const char *, LPVOID);
__declspec(dllimport) BOOL RemoveDirectoryA(const char *);
__declspec(dllimport) BOOL DeleteFileA(const char *);
__declspec(dllimport) BOOL MoveFileA(const char *, const char *);

static DWORD text_length(const char *s)
{
    DWORD n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *s)
{
    DWORD done;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, text_length(s), &done, 0);
}

int main(void)
{
    static const char payload[] = "FAT32 writes survive close and reopen.\r\n";
    HANDLE file = CreateFileA("WRITE.TXT", GENERIC_WRITE, 0, 0,
                              CREATE_ALWAYS, 0, 0);
    DWORD done = 0;
    if (file == INVALID_HANDLE_VALUE ||
        !WriteFile(file, payload, sizeof(payload) - 1, &done, 0) ||
        done != sizeof(payload) - 1) {
        print("[fail] could not create/write WRITE.TXT\r\n");
        return 1;
    }
    CloseHandle(file);

    file = CreateFileA("WRITE.TXT", GENERIC_READ, 1, 0,
                       OPEN_EXISTING, 0, 0);
    char readback[sizeof(payload)];
    DWORD got = 0;
    if (file == INVALID_HANDLE_VALUE ||
        !ReadFile(file, readback, sizeof(payload) - 1, &got, 0) ||
        got != sizeof(payload) - 1) {
        print("[fail] could not reopen/read WRITE.TXT\r\n");
        return 2;
    }
    CloseHandle(file);
    for (DWORD i = 0; i < got; i++) {
        if (readback[i] != payload[i]) {
            print("[fail] WRITE.TXT contents differ\r\n");
            return 3;
        }
    }
    print("[ok] FAT32 create + write + close + reopen + read\r\n");

    HANDLE first = CreateFileA("WRITE.TXT", GENERIC_READ, 3, 0,
                               OPEN_EXISTING, 0, 0);
    HANDLE second = CreateFileA("WRITE.TXT", GENERIC_READ, 3, 0,
                                OPEN_EXISTING, 0, 0);
    char a = 0, b = 0;
    DWORD one_a = 0, one_b = 0;
    if (first == INVALID_HANDLE_VALUE || second == INVALID_HANDLE_VALUE ||
        !ReadFile(first, &a, 1, &one_a, 0) ||
        !ReadFile(second, &b, 1, &one_b, 0) ||
        one_a != 1 || one_b != 1 || a != b) {
        print("[fail] independent file offsets\r\n");
        return 7;
    }
    CloseHandle(first);
    CloseHandle(second);

    first = CreateFileA("WRITE.TXT", GENERIC_READ, 1, 0,
                        OPEN_EXISTING, 0, 0);
    second = CreateFileA("WRITE.TXT", GENERIC_WRITE, 1, 0,
                         OPEN_EXISTING, 0, 0);
    if (first == INVALID_HANDLE_VALUE || second != INVALID_HANDLE_VALUE) {
        print("[fail] file share matrix\r\n");
        return 8;
    }
    CloseHandle(first);
    print("[ok] independent offsets + read/write share conflict\r\n");

    /* Make the test repeatable after a previous successful run. */
    DeleteFileA("DATA\\NEW.TXT");
    RemoveDirectoryA("DATA");
    if (!CreateDirectoryA("DATA", 0)) {
        print("[fail] CreateDirectoryA(DATA)\r\n");
        return 4;
    }
    file = CreateFileA("DATA\\OLD.TXT", GENERIC_WRITE, 0, 0,
                       CREATE_ALWAYS, 0, 0);
    done = 0;
    if (file == INVALID_HANDLE_VALUE ||
        !WriteFile(file, payload, sizeof(payload) - 1, &done, 0)) {
        print("[fail] write inside DATA\r\n");
        return 5;
    }
    CloseHandle(file);
    if (!MoveFileA("DATA\\OLD.TXT", "DATA\\NEW.TXT") ||
        !DeleteFileA("DATA\\NEW.TXT") ||
        !RemoveDirectoryA("DATA")) {
        print("[fail] rename/delete/rmdir\r\n");
        return 6;
    }
    print("[ok] FAT32 mkdir + nested write + rename + delete + rmdir\r\n");
    print("FAT32_TEST: PASS\r\n");
    return 0;
}
