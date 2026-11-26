/* kernel32-core probe for tests/core_env.sh (S1). Imports only kernel32 and is built without a
 * CRT (-nostdlib -e entry), so it relinks before msvcrt exists. Prints argv, selected environment
 * variables, module/proc lookups, a VirtualQuery/VirtualProtect round trip on its own data and
 * code, code-page conversion, CreateDirectoryA under APPDATA. Exit code 0 = every check passed;
 * otherwise a bit per failed group (1 env, 2 modules, 4 vmem, 8 conversion, 16 directory,
 * 32 last-error/MulDiv/tick). */
#include <windows.h>

static HANDLE out;
static DWORD fails;

static DWORD slen(const char *s)
{
    DWORD n = 0;
    while (s[n])
        n++;
    return n;
}

static void put(const char *s)
{
    DWORD w;
    WriteFile(out, s, slen(s), &w, NULL);
}

static void putu(DWORD v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    put(b + i);
}

static void put_ok(const char *what, int ok)
{
    put(what);
    put(ok ? "=ok\n" : "=FAIL\n");
}

/* prints NAME=value (or <unset>) and returns the value length, 0 when unset */
static DWORD env_show(const char *name, char *buf, DWORD cap)
{
    DWORD n = GetEnvironmentVariableA(name, buf, cap);
    put(name);
    put("=");
    if (!n)
        put("<unset>");
    else if (n >= cap)
        put("<too long>");
    else
        put(buf);
    put("\n");
    return n < cap ? n : 0;
}

static volatile int cell = 7; /* writable data on the image's own page */

static LONG WINAPI filt(EXCEPTION_POINTERS *p)
{
    (void)p;
    return 0;
}

int entry(void)
{
    char buf[512], dir[600];
    MEMORY_BASIC_INFORMATION mbi;
    DWORD old, old2;
    WCHAR w[8];
    char u8[8];

    out = GetStdHandle((DWORD)-11);

    put("cmdline=");
    put(GetCommandLineA());
    put("\n");

    /* environment defaults */
    if (!env_show("APPDATA", buf, sizeof buf))
        fails |= 1;
    env_show("TEMP", buf, sizeof buf);
    env_show("TMP", buf, sizeof buf);
    env_show("USERPROFILE", buf, sizeof buf);
    env_show("SystemRoot", buf, sizeof buf);
    env_show("OS", buf, sizeof buf);
    env_show("NUMBER_OF_PROCESSORS", buf, sizeof buf);

    /* APPDATA is usable: create a directory under it */
    {
        DWORD n = GetEnvironmentVariableA("APPDATA", buf, sizeof buf);
        DWORD i = 0, j = 0;
        if (n && n < sizeof buf) {
            for (; buf[i]; i++)
                dir[i] = buf[i];
            const char *tail = "\\anywin-core-env";
            for (; tail[j]; j++)
                dir[i + j] = tail[j];
            dir[i + j] = 0;
            if (CreateDirectoryA(dir, NULL)) {
                put("mkdir=created\n");
                if (CreateDirectoryA(dir, NULL) || GetLastError() != 183) {
                    put("mkdir-again=FAIL\n");
                    fails |= 16;
                }
            } else if (GetLastError() == 183) {
                put("mkdir=exists\n");
            } else {
                put("mkdir=FAIL\n");
                fails |= 16;
            }
        } else {
            fails |= 16;
        }
    }

    /* module and proc lookups */
    {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        HMODULE u = GetModuleHandleA("user32.dll");
        HMODULE none;
        HMODULE own = GetModuleHandleA(NULL);
        char path[512];
        DWORD pn = GetModuleFileNameA(NULL, path, sizeof path);
        int ok_k = k && GetProcAddress(k, "Sleep") && !GetProcAddress(k, "NoSuchExport");
        int ok_perr = 0;
        none = GetModuleHandleA("nosuchdll.dll");
        if (!none && GetLastError() == 126)
            ok_perr = 1;
        put_ok("kernel32-proc", ok_k);
        put_ok("user32-handle", u != NULL);
        put_ok("nosuchdll-126", ok_perr);
        put_ok("own-handle", own != NULL && own == GetModuleHandleA(path));
        put("module=");
        put(pn ? path : "<none>");
        put("\n");
        if (!ok_k || !u || !ok_perr || !own || own != GetModuleHandleA(path))
            fails |= 2;
    }

    /* VirtualQuery / VirtualProtect on the image's own data and code */
    {
        SIZE_T got = VirtualQuery((const void *)&cell, &mbi, sizeof mbi);
        uintptr_t page = (ULONG_PTR)&cell & ~(ULONG_PTR)4095;
        int ok = got == sizeof mbi && mbi.State == MEM_COMMIT && mbi.Protect != 0;
        DWORD before = mbi.Protect;
        if (ok && VirtualProtect((void *)page, 4096, PAGE_READONLY, &old) && old == before) {
            VirtualQuery((const void *)&cell, &mbi, sizeof mbi);
            ok = ok && mbi.Protect == PAGE_READONLY;
            ok = ok && VirtualProtect((void *)page, 4096, old, &old2) && old2 == PAGE_READONLY;
            VirtualQuery((const void *)&cell, &mbi, sizeof mbi);
            ok = ok && mbi.Protect == before;
        } else
            ok = 0;
        put_ok("vprotect-roundtrip", ok);

        VirtualQuery((const void *)&entry, &mbi, sizeof mbi);
        put_ok("code-image", mbi.Type == MEM_IMAGE && (mbi.Protect & 0xf0) != 0);
        if (!ok || !(mbi.Type == MEM_IMAGE && (mbi.Protect & 0xf0) != 0))
            fails |= 4;

        /* address 0x10 is never mapped: MEM_FREE, and VirtualProtect refuses it */
        VirtualQuery((const void *)0x10, &mbi, sizeof mbi);
        ok = mbi.State == MEM_FREE && mbi.BaseAddress == 0;
        ok = ok && !VirtualProtect((void *)0x10, 4096, PAGE_READONLY, &old) && GetLastError() == 487;
        put_ok("vmem-unmapped", ok);
        if (!ok)
            fails |= 4;
    }

    /* code pages: UTF-8 round trip of U+00E9 */
    {
        u8[0] = (char)0xc3;
        u8[1] = (char)0xa9;
        u8[2] = 0;
        int nw = MultiByteToWideChar(65001, 0, u8, -1, w, 8);
        int nb = WideCharToMultiByte(65001, 0, w, -1, buf, sizeof buf, NULL, NULL);
        int ok = nw == 2 && w[0] == 0xe9 && w[1] == 0 && nb == 3 && (unsigned char)buf[0] == 0xc3;
        put_ok("utf8-roundtrip", ok);
        if (!ok)
            fails |= 8;
    }

    /* small kernel32 items */
    {
        int ok = MulDiv(3, 1, 2) == 2 && MulDiv(-3, 1, 2) == -2 && MulDiv(7, 3, 0) == -1;
        SetLastError(1234);
        ok = ok && GetLastError() == 1234;
        ok = ok && GetTickCount64() != 0;
        /* SetUnhandledExceptionFilter returns the previous filter */
        ok = ok && SetUnhandledExceptionFilter(filt) == NULL;
        ok = ok && SetUnhandledExceptionFilter(NULL) == filt;
        put_ok("muldiv-lasterror-tick-filter", ok);
        if (!ok)
            fails |= 32;
    }

    /* startup info and code-page queries */
    {
        STARTUPINFOA si;
        GetStartupInfoA(&si);
        int ok = si.cb == sizeof si && si.wShowWindow == 10 && si.hStdOutput == GetStdHandle((DWORD)-11);
        ok = ok && !IsDBCSLeadByteEx(65001, 0xc3);
        put_ok("startupinfo-dbcs", ok);
        if (!ok)
            fails |= 32;
    }

    put("failures=");
    putu(fails);
    put("\n");
    ExitProcess(fails);
    return 0;
}
