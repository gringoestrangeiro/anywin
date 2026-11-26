/* S8 tray probe (MinGW gcc): Shell_NotifyIconA bookkeeping without a tray manager.
 * Icons are keyed by (hWnd, uID): adding twice fails, modifying or deleting an unknown icon fails,
 * and every call that is valid returns TRUE even when no notification area is running.
 * Prints PASS/FAIL per check; the exit status is the number of failures. */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

static int fails;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        fails++;
}

int main(void)
{
    NOTIFYICONDATAA nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = (HWND)(uintptr_t)0x1234;
    nid.uID = 7;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_USER + 1;
    nid.hIcon = LoadIconA(NULL, (LPCSTR)IDI_APPLICATION);
    strcpy(nid.szTip, "probe");

    check("NIM_ADD returns TRUE without a tray manager", Shell_NotifyIconA(NIM_ADD, &nid));
    check("NIM_ADD of the same (hWnd, uID) fails", !Shell_NotifyIconA(NIM_ADD, &nid));
    nid.uFlags = NIF_TIP;
    check("NIM_MODIFY of the icon returns TRUE", Shell_NotifyIconA(NIM_MODIFY, &nid));
    nid.uID = 8;
    check("NIM_MODIFY of another uID fails", !Shell_NotifyIconA(NIM_MODIFY, &nid));
    check("NIM_SETFOCUS of an unknown icon fails", !Shell_NotifyIconA(NIM_SETFOCUS, &nid));
    nid.uID = 7;
    nid.uVersion = NOTIFYICON_VERSION_4;
    check("NIM_SETVERSION returns TRUE", Shell_NotifyIconA(NIM_SETVERSION, &nid));
    check("NIM_DELETE returns TRUE", Shell_NotifyIconA(NIM_DELETE, &nid));
    check("NIM_DELETE of a removed icon fails", !Shell_NotifyIconA(NIM_DELETE, &nid));
    check("NULL data fails", !Shell_NotifyIconA(NIM_ADD, NULL));
    printf("failures=%d\n", fails);
    return fails;
}
