/* message-delivery probe: posted messages must reach their window promptly (< 50 ms) whatever loop
 * is running and whatever woke the thread.  Phases:
 *   KEY_GETMSG / KEY_MSGWAIT   a key press (X event) is handled, the handler posts a message to its own
 *                              window; the GetMessage loop (resp. PeekMessage + MsgWaitForMultipleObjects)
 *                              must deliver it at once.  The script answers each "WAIT_KEY" line with a key.
 *   WORKER_GETMSG / WORKER_MSGWAIT  a worker thread posts to the window while the loop sleeps.
 *   CLIP_GETMSG / CLIP_KEY_MSGWAIT  the handler reads the clipboard (a fetch from another X client that
 *                              owns CLIPBOARD, so the runtime waits for the selection reply), then posts.
 *   IDLE                       GetMessage / MsgWaitForMultipleObjects sleep for one second; the process
 *                              must use < 150 ms of CPU.
 *   NESTED                     WM_SETCURSOR (sent from input dispatch when the pointer enters the window)
 *                              runs a nested GetMessage loop that must see a key press (driver: WAIT_MOVE).
 * Output: "ok" / "FAIL" lines and a final "postq_probe: ok" when every check passed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define WM_T_SELF (WM_APP + 1)
#define WM_T_WORKER (WM_APP + 2)
#define WM_T_CLIPDONE (WM_APP + 3)
#define WM_T_CLIP (WM_APP + 4)
#define WM_T_NEST_DONE (WM_APP + 5)
#define TIMER_WATCHDOG 99
#define TIMER_NEST 98
#define LATENCY_MAX_MS 50
#define CLIP_TEXT L"probe-clip-text"

static HWND g_wnd;
static int g_fails;
static UINT g_seen;   /* message the current phase waits for (0 = none yet) */
static int g_timeout; /* watchdog fired */
static DWORD g_stamp; /* time of the post the current phase measures */
static DWORD g_lat;   /* delivery latency of the message the phase waits for */
static int g_clip_ok;
static int g_key_mode; /* the KEY phases post from WM_KEYDOWN instead of from the main thread */
static int g_nest;       /* the next WM_SETCURSOR (sent from inside input dispatch) runs a nested loop */
static int g_nest_key;   /* the nested loop is running: a WM_KEYDOWN ends it */
static int g_nest_done;
static int g_nest_timeout;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (!ok)
        g_fails++;
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SETCURSOR:
        if (g_nest) {
            /* a nested loop inside the dispatch of a pointer event: it must still see keyboard input */
            g_nest = 0;
            g_nest_done = 0;
            g_nest_timeout = 0;
            g_nest_key = 1;
            SetTimer(h, TIMER_NEST, 3000, NULL);
            while (!g_nest_done && !g_nest_timeout) {
                MSG nm;
                if (!GetMessageA(&nm, NULL, 0, 0))
                    break;
                DispatchMessageA(&nm);
            }
            KillTimer(h, TIMER_NEST);
            g_nest_key = 0;
            PostMessageA(h, WM_T_NEST_DONE, 0, 0);
        }
        break;
    case WM_KEYDOWN:
        if (g_nest_key)
            g_nest_done = 1;
        if (g_key_mode) {
            /* the key press is the external trigger; the handler posts to its own window */
            g_stamp = GetTickCount();
            PostMessageA(h, g_key_mode == 1 ? WM_T_SELF : WM_T_CLIP, 0, 0);
            g_key_mode = 0;
        }
        return 0;
    case WM_T_CLIP: {
        /* a clipboard fetch from another client: the runtime waits for the selection reply in here */
        int ok = 0;
        if (OpenClipboard(h)) {
            HANDLE hd = GetClipboardData(CF_UNICODETEXT);
            if (hd) {
                const WCHAR *t = GlobalLock(hd);
                ok = t && wcscmp(t, CLIP_TEXT) == 0;
                GlobalUnlock(hd);
            }
            CloseClipboard();
        }
        g_clip_ok = ok;
        g_stamp = GetTickCount();
        PostMessageA(h, WM_T_CLIPDONE, 0, 0);
        return 0;
    }
    case WM_T_SELF:
    case WM_T_WORKER:
    case WM_T_CLIPDONE:
    case WM_T_NEST_DONE:
        if (m == g_seen) {
            g_lat = GetTickCount() - g_stamp;
            g_seen = m;
        }
        return 0;
    case WM_TIMER:
        if (w == TIMER_WATCHDOG)
            g_timeout = 1;
        if (w == TIMER_NEST)
            g_nest_timeout = 1;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* Pumps until the handler sees `want` or the watchdog fires.  loop 0 = GetMessage, 1 = PeekMessage +
 * MsgWaitForMultipleObjects (the PuTTY style).  Returns 1 when the message arrived. */
static int pump(int loop, UINT want, DWORD limit_ms)
{
    g_seen = want;
    g_lat = 0;
    g_timeout = 0;
    SetTimer(g_wnd, TIMER_WATCHDOG, limit_ms, NULL);
    if (loop == 0) {
        MSG msg;
        while (g_seen == want && !g_timeout) {
            if (!GetMessageA(&msg, NULL, 0, 0))
                break;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    } else {
        for (;;) {
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT)
                    goto done;
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
                if (g_seen != want || g_timeout)
                    goto done;
            }
            if (g_seen != want || g_timeout)
                break;
            MsgWaitForMultipleObjects(0, NULL, FALSE, INFINITE, QS_ALLINPUT);
        }
    }
done:
    KillTimer(g_wnd, TIMER_WATCHDOG);
    return g_seen != want ? 0 : 1;
}

static void report(const char *what, int got, DWORD lat)
{
    char buf[160];
    if (!got)
        snprintf(buf, sizeof buf, "%s: not delivered before the watchdog", what);
    else
        snprintf(buf, sizeof buf, "%s: delivered after %lu ms", what, (unsigned long)lat);
    check(got && lat < LATENCY_MAX_MS, buf);
}

/* ---- worker thread: posts after a delay ---- */
static DWORD WINAPI worker(LPVOID arg)
{
    Sleep((DWORD)(UINT_PTR)arg);
    g_stamp = GetTickCount();
    PostMessageA(g_wnd, WM_T_WORKER, 0, 0);
    return 0;
}

/* Asks the driver script for input (one line per request) before the phase starts. */
static void wait_key_line(const char *tag)
{
    static int n;
    printf("%s %d\n", tag, ++n);
    fflush(stdout);
}

static void wait_key(void)
{
    wait_key_line("WAIT_KEY");
}

static void key_phase(const char *name, int loop, UINT want, int mode)
{
    g_key_mode = mode; /* the next WM_KEYDOWN, handled inside pump(), posts `want` from its handler */
    wait_key();
    int got = pump(loop, want, 4000);
    report(name, got, g_lat);
    g_key_mode = 0;
}

static void worker_phase(const char *name, int loop, UINT want, DWORD delay)
{
    HANDLE t = CreateThread(NULL, 0, worker, (LPVOID)(UINT_PTR)delay, 0, NULL);
    int got = pump(loop, want, 4000);
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    report(name, got, g_lat);
}

static void clip_phase(const char *name, int loop, int mode)
{
    g_clip_ok = 0;
    if (mode == 0) {
        /* posted from outside: the handler does the clipboard fetch and then posts */
        PostMessageA(g_wnd, WM_T_CLIP, 0, 0);
        int got = pump(loop, WM_T_CLIPDONE, 4000);
        check(g_clip_ok, "clipboard fetch returns the text of the other client");
        report(name, got, g_lat);
    } else {
        g_key_mode = 2;
        wait_key();
        int got = pump(loop, WM_T_CLIPDONE, 4000);
        check(g_clip_ok, "clipboard fetch (key-triggered) returns the text");
        report(name, got, g_lat);
        g_key_mode = 0;
    }
}

static void nested_phase(void)
{
    g_nest = 1;
    wait_key_line("WAIT_MOVE");
    /* the pointer enters the window: WM_SETCURSOR runs inside the dispatch of that motion */
    int got = pump(0, WM_T_NEST_DONE, 6000);
    printf("nested: entered=%d key_seen=%d\n", !g_nest, g_nest_done);
    check(got && g_nest_done, "nested loop started from a pointer event still delivers keyboard input");
    g_nest = 0;
}

static void idle_phase(int loop)
{
    FILETIME c, e, k, u, k0, u0;
    HANDLE me = GetCurrentProcess();
    HANDLE t = CreateThread(NULL, 0, worker, (LPVOID)(UINT_PTR)1000, 0, NULL);
    GetProcessTimes(me, &c, &e, &k0, &u0);
    int got = pump(loop, WM_T_WORKER, 4000);
    GetProcessTimes(me, &c, &e, &k, &u);
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    ULONGLONG cpu = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) - ((ULONGLONG)k0.dwHighDateTime << 32 | k0.dwLowDateTime);
    cpu += ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime) - ((ULONGLONG)u0.dwHighDateTime << 32 | u0.dwLowDateTime);
    DWORD cpu_ms = (DWORD)(cpu / 10000);
    char buf[160];
    snprintf(buf, sizeof buf, "idle wait of 1 s (%s) used %lu ms of CPU", loop ? "MsgWaitForMultipleObjects" : "GetMessage",
             (unsigned long)cpu_ms);
    check(got && cpu_ms < 150, buf);
}

int main(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "postq_probe";
    RegisterClassA(&wc);
    g_wnd = CreateWindowExA(0, "postq_probe", "postq-probe", WS_OVERLAPPEDWINDOW, 50, 50, 300, 200,
                            NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) {
        printf("FAIL no window\n");
        return 1;
    }
    ShowWindow(g_wnd, SW_SHOW);
    UpdateWindow(g_wnd);

    key_phase("KEY_GETMSG: post from a key handler (GetMessage)", 0, WM_T_SELF, 1);
    key_phase("KEY_MSGWAIT: post from a key handler (MsgWaitForMultipleObjects)", 1, WM_T_SELF, 1);
    worker_phase("WORKER_GETMSG: post from a worker thread (GetMessage)", 0, WM_T_WORKER, 300);
    worker_phase("WORKER_MSGWAIT: post from a worker thread (MsgWaitForMultipleObjects)", 1, WM_T_WORKER, 300);
    clip_phase("CLIP_GETMSG: post after a clipboard fetch (GetMessage)", 0, 0);
    clip_phase("CLIP_KEY_MSGWAIT: post after a clipboard fetch from a key (MsgWait)", 1, 1);
    clip_phase("CLIP_KEY_GETMSG: post after a clipboard fetch from a key (GetMessage)", 0, 1);
    idle_phase(0);
    idle_phase(1);
    nested_phase();

    DestroyWindow(g_wnd);
    if (g_fails) {
        printf("postq_probe: %d FAIL\n", g_fails);
        return 1;
    }
    printf("postq_probe: ok\n");
    return 0;
}
