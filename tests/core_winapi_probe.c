/* Probe for the W-variant, file, service, token and shell exports added in runtime/file.c,
 * runtime/kernel32_ext.c, runtime/services.c, runtime/shell_exec.c and advapi32.c.
 * Usage: core_winapi_probe.exe <existing empty directory, Windows form>. Prints PASS/FAIL lines. */
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

static int fails;

/* wide-string helpers: the runtime has no wide printf or wcscmp family, so the probe carries its own */
static void wcopy(WCHAR *d, const WCHAR *s)
{
    while ((*d++ = *s++))
        ;
}
static void wcat(WCHAR *d, const WCHAR *s)
{
    wcopy(d + wcslen(d), s);
}
static int wequal(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static int wcontains(const WCHAR *hay, const WCHAR *needle)
{
    size_t n = wcslen(needle);
    for (; *hay; hay++)
        if (!memcmp(hay, needle, n * sizeof(WCHAR)))
            return 1;
    return 0;
}
static void check(int ok, const char *name)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        printf("     last error %lu\n", GetLastError());
        fails++;
    }
}

static void wide(WCHAR *d, size_t cap, const WCHAR *dir, const WCHAR *leaf)
{
    (void)cap;
    wcopy(d, dir);
    wcat(d, L"\\");
    wcat(d, leaf);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: core_winapi_probe <dir>\n");
        return 2;
    }
    WCHAR dir[MAX_PATH * 2];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, dir, MAX_PATH * 2);
    WCHAR p1[MAX_PATH * 2], p2[MAX_PATH * 2], p3[MAX_PATH * 2];
    wide(p1, MAX_PATH * 2, dir, L"probe.txt");
    wide(p2, MAX_PATH * 2, dir, L"probe2.txt");
    wide(p3, MAX_PATH * 2, dir, L"probe3.txt");

    /* heap */
    BYTE *h = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 64);
    int zeroed = h && h[0] == 0 && h[63] == 0;
    if (h)
        h[0] = 7;
    h = HeapReAlloc(GetProcessHeap(), 0, h, 4096);
    check(zeroed && h && h[0] == 7, "HeapAlloc zeroes, HeapReAlloc keeps data");
    check(HeapFree(GetProcessHeap(), 0, h), "HeapFree");

    /* temp path and current directory */
    WCHAR tmp[MAX_PATH * 2], cwd[MAX_PATH * 2], full[MAX_PATH * 2];
    DWORD n = GetTempPathW(MAX_PATH * 2, tmp);
    check(n > 0 && tmp[n - 1] == L'\\', "GetTempPathW ends with a backslash");
    check(GetCurrentDirectoryW(MAX_PATH * 2, cwd) > 0, "GetCurrentDirectoryW");
    WCHAR *part = NULL;
    n = GetFullPathNameW(L"..\\x.txt", MAX_PATH * 2, full, &part);
    check(n > 0 && full[1] == L':' && part && wequal(part, L"x.txt"), "GetFullPathNameW resolves .. and file part");

    /* files */
    HANDLE f = CreateFileW(p1, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wrote = 0;
    check(f != INVALID_HANDLE_VALUE && WriteFile(f, "abc", 3, &wrote, NULL) && wrote == 3 && CloseHandle(f),
          "CreateFileW CREATE_ALWAYS and WriteFile");
    f = CreateFileW(p1, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char buf[8] = {0};
    DWORD got = 0;
    check(f != INVALID_HANDLE_VALUE && ReadFile(f, buf, sizeof buf, &got, NULL) && got == 3 && !memcmp(buf, "abc", 3),
          "ReadFile returns the written bytes");
    FILE_STANDARD_INFO fsi;
    check(GetFileInformationByHandleEx(f, FileStandardInfo, &fsi, sizeof fsi) && fsi.EndOfFile.QuadPart == 3,
          "GetFileInformationByHandleEx FileStandardInfo size");
    CloseHandle(f);
    f = CreateFileW(p1, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    check(f != INVALID_HANDLE_VALUE, "CreateFileW OPEN_EXISTING opens an existing file");
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);

    WIN32_FIND_DATAW fd;
    WCHAR pat[MAX_PATH * 2];
    wide(pat, MAX_PATH * 2, dir, L"*.txt");
    HANDLE find = FindFirstFileExW(pat, FindExInfoStandard, &fd, FindExSearchNameMatch, NULL, 0);
    check(find != INVALID_HANDLE_VALUE && wequal(fd.cFileName, L"probe.txt") && fd.nFileSizeLow == 3,
          "FindFirstFileExW finds the file by wildcard");
    check(!FindNextFileW(find, &fd) && GetLastError() == ERROR_NO_MORE_FILES, "FindNextFileW ends with ERROR_NO_MORE_FILES");
    check(FindClose(find), "FindClose");

    check(MoveFileExW(p1, p2, 0), "MoveFileExW renames a file");
    f = CreateFileW(p1, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    check(f == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND, "old name is gone after the move");

    SHFILEOPSTRUCTW op = {0};
    WCHAR from[MAX_PATH * 2 + 2];
    memset(from, 0, sizeof from);
    wcopy(from, p2);
    op.wFunc = FO_RENAME;
    op.pFrom = from;
    op.pTo = p3;
    check(SHFileOperationW(&op) == 0, "SHFileOperationW FO_RENAME");

    /* registry (W variants) */
    HKEY key = NULL;
    DWORD disp = 0;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnywinWinapiProbe", 0, NULL, 0, KEY_ALL_ACCESS, NULL,
                              &key, &disp);
    check(rc == 0 && key, "RegCreateKeyExW");
    const WCHAR greeting[] = L"hello w";
    rc = RegSetValueExW(key, L"Greeting", 0, REG_SZ, (const BYTE *)greeting, sizeof greeting);
    check(rc == 0, "RegSetValueExW REG_SZ");
    WCHAR back[64] = {0};
    DWORD cb = sizeof back, type = 0;
    rc = RegQueryValueExW(key, L"Greeting", NULL, &type, (BYTE *)back, &cb);
    check(rc == 0 && type == REG_SZ && wequal(back, greeting), "RegQueryValueExW round trip");
    WCHAR vname[64];
    DWORD vlen = 64;
    rc = RegEnumValueW(key, 0, vname, &vlen, NULL, &type, NULL, NULL);
    check(rc == 0 && wequal(vname, L"Greeting"), "RegEnumValueW lists the value");
    rc = RegDeleteValueW(key, L"Greeting");
    check(rc == 0, "RegDeleteValueW");
    RegCloseKey(key);
    HKEY missing = NULL;
    rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnywinWinapiProbe\\Nope", 0, KEY_READ, &missing);
    check(rc == ERROR_FILE_NOT_FOUND, "RegOpenKeyExW on a missing key");
    check(RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\AnywinWinapiProbe") == 0, "RegDeleteKeyW");

    /* known folders */
    WCHAR *kf = NULL;
    HRESULT hr = SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &kf);
    check(hr == S_OK && kf && kf[0], "SHGetKnownFolderPath(RoamingAppData)");
    CoTaskMemFree(kf);

    /* services */
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    DWORD need = 0, count = 0, resume = 0;
    BOOL ok = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &need,
                                    &count, &resume, NULL);
    check(scm && !ok && GetLastError() == ERROR_MORE_DATA, "EnumServicesStatusExW size query");
    BYTE *list = malloc(need ? need : 1);
    ok = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, list, need, &need,
                               &count, &resume, NULL);
    check(ok, "EnumServicesStatusExW fills the list");
    free(list);
    CloseServiceHandle(scm);

    /* tokens */
    HANDLE tok = NULL;
    DWORD elev = 99, len = 0;
    check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok) && tok, "OpenProcessToken");
    check(GetTokenInformation(tok, TokenElevation, &elev, sizeof elev, &len) && len == 4 && elev <= 1,
          "GetTokenInformation(TokenElevation)");
    CloseHandle(tok);

    /* messages, accelerators, system parameters, timers, toolhelp */
    WCHAR msg[256] = {0};
    DWORD mlen = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, 2, 0, msg, 256, NULL);
    check(mlen > 0 && wcontains(msg, L"cannot find the file"), "FormatMessageW(ERROR_FILE_NOT_FOUND)");
    ACCEL acc = {FVIRTKEY, 'A', 100};
    HACCEL accel = CreateAcceleratorTableW(&acc, 1);
    check(accel && DestroyAcceleratorTable(accel), "CreateAcceleratorTableW / DestroyAcceleratorTable");
    RECT wa = {0};
    check(SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0) && wa.right > 0, "SystemParametersInfoW(SPI_GETWORKAREA)");
    LARGE_INTEGER q1, q2, qf;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q1);
    QueryPerformanceCounter(&q2);
    check(qf.QuadPart > 0 && q2.QuadPart >= q1.QuadPart, "QueryPerformanceCounter is monotonic");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    MODULEENTRY32W me = {sizeof me};
    check(snap != INVALID_HANDLE_VALUE && Module32FirstW(snap, &me) && me.szModule[0], "Toolhelp main module");
    CloseHandle(snap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    check(st.wYear >= 2024, "GetLocalTime");

    printf("failures=%d\n", fails);
    return fails ? 1 : 0;
}
