/* keyboard probe: (1) ToUnicodeEx / ToAsciiEx translate only keys that are down in the state
 * array (PuTTY calls ToUnicodeEx from WM_KEYUP too); Shift/Ctrl/CapsLock rules of the US layout.
 * (2) a message loop that logs WM_KEYDOWN and WM_CHAR.  argv[1] = "tm" calls TranslateMessage
 * (WM_CHAR must then appear once per key), "notm" does not (no WM_CHAR at all). */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_KEYDOWN: printf("KD %02x\n", (unsigned)w); fflush(stdout); return 0;
    case WM_CHAR: printf("CH %04x\n", (unsigned)w); fflush(stdout); return 0;
    case WM_TIMER: PostQuitMessage(0); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv)
{
    BYTE ks[256];
    WCHAR wb[4];
    WORD ab[4];
    int tm = argc < 2 || strcmp(argv[1], "notm") != 0;

    memset(ks, 0, sizeof ks);
    check(ToUnicodeEx('A', 0x1E, ks, wb, 4, 0, NULL) == 0, "ToUnicodeEx: key up gives no text");
    ks['A'] = 0x80;
    check(ToUnicodeEx('A', 0x1E, ks, wb, 4, 0, NULL) == 1 && wb[0] == 'a', "ToUnicodeEx: A down -> a");
    ks[VK_SHIFT] = 0x80;
    check(ToUnicodeEx('A', 0x1E, ks, wb, 4, 0, NULL) == 1 && wb[0] == 'A', "ToUnicodeEx: shift+A -> A");
    ks[VK_SHIFT] = 0;
    ks[VK_CAPITAL] = 0x01;
    check(ToUnicodeEx('A', 0x1E, ks, wb, 4, 0, NULL) == 1 && wb[0] == 'A', "ToUnicodeEx: CapsLock+A -> A");
    ks[VK_CAPITAL] = 0;
    memset(ks, 0, sizeof ks);
    ks['1'] = 0x80;
    ks[VK_SHIFT] = 0x80;
    check(ToUnicodeEx('1', 0x02, ks, wb, 4, 0, NULL) == 1 && wb[0] == '!', "ToUnicodeEx: shift+1 -> !");
    memset(ks, 0, sizeof ks);
    ks['C'] = 0x80;
    ks[VK_CONTROL] = 0x80;
    check(ToAsciiEx('C', 0x2E, ks, ab, 0, NULL) == 1 && ab[0] == 3, "ToAsciiEx: ctrl+C -> 0x03");
    ks[VK_CONTROL] = 0;
    ks['C'] = 0;
    check(ToAsciiEx('C', 0x2E, ks, ab, 0, NULL) == 0, "ToAsciiEx: ctrl up, key up -> nothing");
    memset(ks, 0, sizeof ks);
    ks[VK_RETURN] = 0x80;
    check(ToAsciiEx(VK_RETURN, 0x1C, ks, ab, 0, NULL) == 1 && ab[0] == '\r', "ToAsciiEx: return -> CR");
    ks[VK_NUMPAD1] = 0x80;
    check(ToAsciiEx(VK_NUMPAD1, 0x4F, ks, ab, 0, NULL) == 0, "ToAsciiEx: numpad 1 without NumLock -> nothing");
    ks[VK_NUMLOCK] = 0x01;
    check(ToAsciiEx(VK_NUMPAD1, 0x4F, ks, ab, 0, NULL) == 1 && ab[0] == '1', "ToAsciiEx: numpad 1 with NumLock");

    WNDCLASSA c;
    memset(&c, 0, sizeof c);
    c.lpfnWndProc = proc;
    c.hInstance = GetModuleHandleA(NULL);
    c.lpszClassName = "KeysProbe";
    RegisterClassA(&c);
    HWND h = CreateWindowExA(0, "KeysProbe", "keys-probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 300, 200,
                             NULL, NULL, c.hInstance, NULL);
    SetTimer(h, 1, 3000, NULL);
    printf("READY %s\n", tm ? "tm" : "notm");
    fflush(stdout);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (tm)
            TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    printf("%s\n", fails ? "translate: FAILED" : "translate: passed");
    return fails ? 1 : 0;
}
