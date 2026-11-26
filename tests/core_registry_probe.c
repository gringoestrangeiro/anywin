/* S8 registry probe (MinGW gcc). Usage: core_registry_probe.exe set|check|delete
 *   set:    creates HKCU\Software\AnyWinProbe\Sub with several value types, checks sizes,
 *           ERROR_MORE_DATA, case-insensitive names, and writes a Run value that starts this
 *           program (autostart).
 *   check:  a separate process reads back what "set" stored (persistence) and enumerates.
 *   delete: removes values and keys, checks the subkey rule and the Run value removal.
 * Prints PASS/FAIL per check; the exit status is the number of failures. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define KEY "Software\\AnyWinProbe\\Sub"
#define PARENT "Software\\AnyWinProbe"
#define RUN "Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static int fails;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    fflush(stdout);
    if (!ok)
        fails++;
}

static void do_set(void)
{
    HKEY hk = NULL, run = NULL;
    DWORD disp = 0, type = 0, cb = 0;
    LONG rc = RegCreateKeyExA(HKEY_CURRENT_USER, KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, &disp);
    check("create key", rc == ERROR_SUCCESS && hk);
    check("create reports a new key", disp == REG_CREATED_NEW_KEY);

    rc = RegSetValueExA(hk, "Greeting", 0, REG_SZ, (const BYTE *)"hello world", 12);
    check("set REG_SZ", rc == ERROR_SUCCESS);
    static const BYTE blob[5] = {1, 2, 0, 3, 255};
    rc = RegSetValueExA(hk, "Blob", 0, REG_BINARY, blob, sizeof blob);
    check("set REG_BINARY", rc == ERROR_SUCCESS);
    rc = RegSetValueExA(hk, "Multi", 0, REG_MULTI_SZ, (const BYTE *)"one\0two\0\0", 9);
    check("set REG_MULTI_SZ", rc == ERROR_SUCCESS);
    DWORD number = 0x01020304;
    rc = RegSetValueExA(hk, "Number", 0, REG_DWORD, (const BYTE *)&number, 4);
    check("set REG_DWORD", rc == ERROR_SUCCESS);

    cb = 0;
    rc = RegQueryValueExA(hk, "Greeting", NULL, &type, NULL, &cb);
    check("size query returns the terminated size", rc == ERROR_SUCCESS && cb == 12 && type == REG_SZ);
    char small[5];
    cb = sizeof small;
    rc = RegQueryValueExA(hk, "Greeting", NULL, &type, (BYTE *)small, &cb);
    check("short buffer gives ERROR_MORE_DATA with the needed size", rc == ERROR_MORE_DATA && cb == 12);
    char text[64];
    cb = sizeof text;
    rc = RegQueryValueExA(hk, "GREETING", NULL, &type, (BYTE *)text, &cb);
    check("value name is case-insensitive", rc == ERROR_SUCCESS && cb == 12 && !strcmp(text, "hello world"));
    cb = 0;
    rc = RegQueryValueExA(hk, "Multi", NULL, NULL, NULL, &cb);
    check("REG_MULTI_SZ size", rc == ERROR_SUCCESS && cb == 9);
    BYTE raw[16];
    cb = sizeof raw;
    rc = RegQueryValueExA(hk, "Blob", NULL, &type, raw, &cb);
    check("REG_BINARY round trip", rc == ERROR_SUCCESS && cb == 5 && type == REG_BINARY && !memcmp(raw, blob, 5));
    cb = sizeof number;
    rc = RegQueryValueExA(hk, "Number", NULL, &type, (BYTE *)&number, &cb);
    check("REG_DWORD value", rc == ERROR_SUCCESS && number == 0x01020304 && type == REG_DWORD);
    rc = RegQueryValueExA(hk, "Missing", NULL, NULL, NULL, NULL);
    check("missing value is ERROR_FILE_NOT_FOUND", rc == ERROR_FILE_NOT_FOUND);

    /* the system facts seeded for HKLM read like Windows */
    HKEY nt;
    rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &nt);
    check("open HKLM CurrentVersion", rc == ERROR_SUCCESS);
    cb = sizeof text;
    rc = RegQueryValueExA(nt, "CurrentBuild", NULL, &type, (BYTE *)text, &cb);
    check("HKLM CurrentBuild", rc == ERROR_SUCCESS && !strcmp(text, "19045"));
    RegCloseKey(nt);

    /* autostart: the value is this program's own command line */
    char exe[MAX_PATH], cmd[MAX_PATH + 32];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" --autostart", exe);
    rc = RegOpenKeyExA(HKEY_CURRENT_USER, RUN, 0, KEY_ALL_ACCESS, &run);
    check("open Run key", rc == ERROR_SUCCESS);
    rc = RegSetValueExA(run, "AnyWinProbe", 0, REG_SZ, (const BYTE *)cmd, (DWORD)strlen(cmd) + 1);
    check("set Run value", rc == ERROR_SUCCESS);
    RegCloseKey(run);
    RegCloseKey(hk);
}

static void do_check(void)
{
    HKEY hk = NULL;
    DWORD disp = 0, type = 0, cb = 0;
    LONG rc = RegCreateKeyExA(HKEY_CURRENT_USER, KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, &disp);
    check("reopen key from a new process", rc == ERROR_SUCCESS && disp == REG_OPENED_EXISTING_KEY);
    char text[64];
    cb = sizeof text;
    rc = RegQueryValueExA(hk, "greeting", NULL, &type, (BYTE *)text, &cb);
    check("REG_SZ persisted", rc == ERROR_SUCCESS && !strcmp(text, "hello world"));
    cb = 0;
    rc = RegQueryValueExA(hk, "Multi", NULL, NULL, NULL, &cb);
    check("REG_MULTI_SZ persisted with its size", rc == ERROR_SUCCESS && cb == 9);
    DWORD number = 0;
    cb = sizeof number;
    rc = RegQueryValueExA(hk, "Number", NULL, NULL, (BYTE *)&number, &cb);
    check("REG_DWORD persisted", rc == ERROR_SUCCESS && number == 0x01020304);

    int values = 0;
    for (DWORD i = 0;; i++) {
        char name[64];
        DWORD nl = sizeof name;
        rc = RegEnumValueA(hk, i, name, &nl, NULL, NULL, NULL, NULL);
        if (rc != ERROR_SUCCESS)
            break;
        values++;
    }
    check("enumerate the four values, then ERROR_NO_MORE_ITEMS", values == 4);

    HKEY parent;
    rc = RegOpenKeyExA(HKEY_CURRENT_USER, PARENT, 0, KEY_READ, &parent);
    char sub[32];
    DWORD sl = sizeof sub;
    rc = (rc == ERROR_SUCCESS) ? RegEnumKeyExA(parent, 0, sub, &sl, NULL, NULL, NULL, NULL) : rc;
    check("enumerate subkeys", rc == ERROR_SUCCESS && !strcmp(sub, "Sub"));
    RegCloseKey(parent);

    HKEY run;
    rc = RegOpenKeyExA(HKEY_CURRENT_USER, RUN, 0, KEY_READ, &run);
    cb = 0;
    rc = (rc == ERROR_SUCCESS) ? RegQueryValueExA(run, "AnyWinProbe", NULL, &type, NULL, &cb) : rc;
    check("Run value persisted", rc == ERROR_SUCCESS && cb > 0 && type == REG_SZ);
    RegCloseKey(run);
    RegCloseKey(hk);
}

static void do_delete(void)
{
    HKEY hk = NULL, run = NULL;
    LONG rc = RegOpenKeyExA(HKEY_CURRENT_USER, KEY, 0, KEY_ALL_ACCESS, &hk);
    check("open key before deleting", rc == ERROR_SUCCESS);
    rc = RegDeleteValueA(hk, "Blob");
    check("delete value", rc == ERROR_SUCCESS);
    rc = RegQueryValueExA(hk, "Blob", NULL, NULL, NULL, NULL);
    check("deleted value is gone", rc == ERROR_FILE_NOT_FOUND);
    rc = RegDeleteKeyA(HKEY_CURRENT_USER, PARENT);
    check("a key with subkeys is not deleted", rc == ERROR_ACCESS_DENIED);
    RegCloseKey(hk);
    rc = RegOpenKeyExA(HKEY_CURRENT_USER, RUN, 0, KEY_ALL_ACCESS, &run);
    rc = (rc == ERROR_SUCCESS) ? RegDeleteValueA(run, "AnyWinProbe") : rc;
    check("delete Run value", rc == ERROR_SUCCESS);
    RegCloseKey(run);
    rc = RegDeleteKeyA(HKEY_CURRENT_USER, KEY);
    check("delete key", rc == ERROR_SUCCESS);
    rc = RegOpenKeyExA(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &hk);
    check("deleted key is gone", rc == ERROR_FILE_NOT_FOUND);
    rc = RegDeleteKeyA(HKEY_CURRENT_USER, PARENT);
    check("empty parent key deleted", rc == ERROR_SUCCESS);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "set";
    if (!strcmp(mode, "check"))
        do_check();
    else if (!strcmp(mode, "delete"))
        do_delete();
    else
        do_set();
    printf("failures=%d\n", fails);
    return fails;
}
