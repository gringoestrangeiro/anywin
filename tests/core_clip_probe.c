/* Clipboard and mouse/keyboard message probe (MinGW, run under Xvfb by tests/core_clip.sh).
 *
 *   core_clip_probe.exe local                 SetClipboardData / GetClipboardData / format enumeration
 *   core_clip_probe.exe remote <text1>        reads CF_UNICODETEXT and CF_TEXT of the X CLIPBOARD owned
 *                                             by another client, twice: the script changes the owner's
 *                                             text in between (freshness)
 *   core_clip_probe.exe mouse <seconds>       logs mouse/wheel/key messages of a window (wParam, lParam)
 *                                             while the script drives xdotool
 *
 * Output lines: "MSG ..." for the mouse log, "FAIL ..." for a failed check, and a final "clip: ok"
 * (local/remote) or "mouse: done" (mouse). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int fails;

#define CHECK(c, ...)                                                                                     \
    do {                                                                                                  \
        if (!(c)) {                                                                                       \
            printf("FAIL " __VA_ARGS__);                                                                  \
            printf("\n");                                                                                 \
            fails++;                                                                                      \
        }                                                                                                 \
    } while (0)

/* Reads CF_UNICODETEXT and CF_TEXT of the clipboard and checks them against the expected UTF-8 text. */
static void read_and_check(const char *tag, const char *expect_utf8)
{
    wchar_t want[256];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, expect_utf8, -1, want, 256);
    CHECK(wlen > 0, "%s: bad expectation", tag);

    CHECK(OpenClipboard(NULL), "%s: OpenClipboard", tag);
    CHECK(IsClipboardFormatAvailable(CF_UNICODETEXT), "%s: CF_UNICODETEXT not available", tag);
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    CHECK(h != NULL, "%s: GetClipboardData(CF_UNICODETEXT) = NULL", tag);
    if (h) {
        SIZE_T size = GlobalSize(h);
        const wchar_t *w = GlobalLock(h);
        CHECK(w != NULL, "%s: GlobalLock", tag);
        if (w) {
            size_t n = wcslen(w);
            printf("%s: unicode [%ls] len=%u size=%u\n", tag, w, (unsigned)n, (unsigned)size);
            CHECK(wcscmp(w, want) == 0, "%s: unicode text differs", tag);
            CHECK(size >= (n + 1) * sizeof(wchar_t), "%s: GlobalSize %u too small", tag, (unsigned)size);
            CHECK(size < (n + 1) * sizeof(wchar_t) + 64, "%s: GlobalSize %u far too large", tag, (unsigned)size);
            GlobalUnlock(h);
        }
    }
    h = GetClipboardData(CF_TEXT);
    CHECK(h != NULL, "%s: GetClipboardData(CF_TEXT) = NULL", tag);
    if (h) {
        const char *c = GlobalLock(h);
        CHECK(c != NULL, "%s: GlobalLock(CF_TEXT)", tag);
        if (c) {
            printf("%s: ansi [%s] len=%u\n", tag, c, (unsigned)strlen(c));
            CHECK(strcmp(c, expect_utf8) == 0, "%s: CF_TEXT differs from the clipboard text", tag);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

static void local_checks(void)
{
    static const wchar_t text[] = L"abc é xyz";
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof text);
    CHECK(g != NULL, "GlobalAlloc");
    wchar_t *p = GlobalLock(g);
    memcpy(p, text, sizeof text);
    GlobalUnlock(g);

    CHECK(OpenClipboard(NULL), "OpenClipboard");
    CHECK(EmptyClipboard(), "EmptyClipboard");
    CHECK(SetClipboardData(CF_UNICODETEXT, g) == g, "SetClipboardData(CF_UNICODETEXT)");
    CHECK(IsClipboardFormatAvailable(CF_UNICODETEXT), "IsClipboardFormatAvailable(CF_UNICODETEXT)");
    /* the set format, then the text formats the system synthesizes from it: 3 in all */
    CHECK(CountClipboardFormats() == 3, "CountClipboardFormats = %d (want 3)", CountClipboardFormats());
    CHECK(EnumClipboardFormats(0) == CF_UNICODETEXT, "EnumClipboardFormats(0) = %u", EnumClipboardFormats(0));
    CHECK(EnumClipboardFormats(CF_UNICODETEXT) == CF_TEXT, "EnumClipboardFormats(CF_UNICODETEXT) = %u",
          EnumClipboardFormats(CF_UNICODETEXT));
    CHECK(EnumClipboardFormats(CF_OEMTEXT) == 0 && GetLastError() == 0, "last format must end the enumeration");
    CloseClipboard();

    /* the data set is what GetClipboardData hands back, NUL-terminated, sized */
    OpenClipboard(NULL);
    HANDLE got = GetClipboardData(CF_UNICODETEXT);
    CHECK(got == g, "GetClipboardData returns the handle that was set");
    SIZE_T size = GlobalSize(g);
    const wchar_t *w = GlobalLock(g);
    CHECK(w && wcscmp(w, text) == 0, "data after SetClipboardData");
    CHECK(size >= sizeof text, "GlobalSize(%u) < %u", (unsigned)size, (unsigned)sizeof text);
    GlobalUnlock(g);
    CloseClipboard();

    /* the set data is the owner's: a second open sees the same content */
    CHECK(OpenClipboard(NULL), "OpenClipboard (2)");
    CloseClipboard();

    CHECK(CloseClipboard() == 0, "CloseClipboard when not open must fail");

    /* registered formats: same name, same id, in the registered range */
    UINT a = RegisterClipboardFormatA("anywin probe format");
    UINT b = RegisterClipboardFormatA("anywin probe format");
    CHECK(a >= 0xC000 && a == b, "RegisterClipboardFormatA ids %u %u", a, b);

    /* EmptyClipboard with nothing set: no formats are left and the owner is this window (NULL here) */
    CHECK(OpenClipboard(NULL), "OpenClipboard (empty)");
    CHECK(EmptyClipboard(), "EmptyClipboard (empty)");
    CHECK(CountClipboardFormats() == 0, "formats left after EmptyClipboard: %d", CountClipboardFormats());
    CHECK(!IsClipboardFormatAvailable(CF_UNICODETEXT), "CF_UNICODETEXT still available after EmptyClipboard");
    CloseClipboard();
}

static void msg_name(UINT m, char *out, size_t n)
{
    static const struct { UINT m; const char *s; } tab[] = {
        {WM_KEYDOWN, "WM_KEYDOWN"}, {WM_KEYUP, "WM_KEYUP"},
        {WM_LBUTTONDOWN, "WM_LBUTTONDOWN"}, {WM_LBUTTONUP, "WM_LBUTTONUP"},
        {WM_RBUTTONDOWN, "WM_RBUTTONDOWN"}, {WM_RBUTTONUP, "WM_RBUTTONUP"},
        {WM_MBUTTONDOWN, "WM_MBUTTONDOWN"}, {WM_MBUTTONUP, "WM_MBUTTONUP"},
        {WM_MOUSEWHEEL, "WM_MOUSEWHEEL"}, {WM_MOUSEMOVE, "WM_MOUSEMOVE"},
    };
    snprintf(out, n, "0x%x", m);
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (tab[i].m == m) {
            snprintf(out, n, "%s", tab[i].s);
            break;
        }
}

static LRESULT CALLBACK mouse_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL: {
        char name[32];
        msg_name(m, name, sizeof name);
        if (m == WM_MOUSEWHEEL)
            printf("MSG %s wParam=0x%llx delta=%d keys=0x%x lParam=%d,%d\n", name,
                   (unsigned long long)wp, (short)HIWORD(wp), LOWORD(wp), (short)LOWORD(lp), (short)HIWORD(lp));
        else if (m == WM_KEYDOWN || m == WM_KEYUP)
            printf("MSG %s vk=0x%x lParam=0x%lx shift=%d\n", name, (unsigned)wp, (unsigned long)lp,
                   (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        else
            printf("MSG %s wParam=0x%llx keys=0x%x client=%d,%d\n", name, (unsigned long long)wp,
                   (unsigned)LOWORD(wp), (short)LOWORD(lp), (short)HIWORD(lp));
        fflush(stdout);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

/* decimal argument (no atoi: the CRT convert module is not needed by this probe) */
static int digits(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static void mouse_mode(int seconds)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = mouse_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "anywin_clip_probe";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, wc.lpszClassName, "anywin clip probe mouse", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             100, 100, 400, 300, NULL, NULL, wc.hInstance, NULL);
    CHECK(h != NULL, "CreateWindowExA");
    POINT origin = {0, 0};
    ClientToScreen(h, &origin);
    printf("mouse: window ready origin=%ld,%ld\n", origin.x, origin.y);
    fflush(stdout);
    DWORD end = GetTickCount() + (DWORD)seconds * 1000;
    MSG msg;
    while ((LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                goto done;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(10);
    }
done:
    DestroyWindow(h);
    printf("mouse: done\n");
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "local";
    if (strcmp(mode, "local") == 0) {
        local_checks();
    } else if (strcmp(mode, "remote") == 0 && argc >= 3) {
        read_and_check("first", argv[2]);
        /* the script changes the owner's text during this pause: the second read must follow it */
        Sleep(2500);
        if (argc >= 4)
            read_and_check("second", argv[3]);
    } else if (strcmp(mode, "mouse") == 0) {
        mouse_mode(argc > 2 ? digits(argv[2]) : 12);
        return 0;
    } else {
        fprintf(stderr, "usage: core_clip_probe local | remote <text1> [text2] | mouse <seconds>\n");
        return 2;
    }
    if (fails) {
        printf("clip: %d failure(s)\n", fails);
        return 1;
    }
    printf("clip: ok\n");
    return 0;
}
