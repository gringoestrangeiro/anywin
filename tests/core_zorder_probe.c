/* Child z-order and hit-testing probe (built with mingw, run under the anywin runtime).
 * Covers WindowFromPoint / ChildWindowFromPoint against z-order and visibility, hidden children
 * (ShowWindow SW_HIDE) that must not be hit, SetWindowPos z-order changes, and the visibility of
 * children of a hidden parent. */
#include <windows.h>
#include <stdio.h>

static int fails;

static void check(int ok, const char *what, int line)
{
    if (!ok) {
        printf("FAIL %s (line %d)\n", what, line);
        fails++;
    } else {
        printf("ok   %s\n", what);
    }
}
#define CHECK(c, what) check((c) != 0, what, __LINE__)

static HWND hit(HWND parent, int x, int y)
{
    POINT p = {x, y};
    return ChildWindowFromPoint(parent, p);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show)
{
    HWND top, a, b, c, n;
    POINT p = {20, 20};
    (void)hi; (void)prev; (void)cmd; (void)show;

    top = CreateWindowExW(0, L"STATIC", L"zorder", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 240, 240, NULL, NULL, hi, NULL);
    CHECK(top != NULL, "top-level window created");
    /* a is created first (bottom), b second (above a), c third (top); all overlap at (20,20) */
    a = CreateWindowExW(0, L"STATIC", L"a", WS_CHILD | WS_VISIBLE, 10, 10, 100, 100, top, (HMENU)1, hi, NULL);
    b = CreateWindowExW(0, L"BUTTON", L"b", WS_CHILD | WS_VISIBLE, 10, 10, 50, 50, top, (HMENU)2, hi, NULL);
    c = CreateWindowExW(0, L"STATIC", L"c", WS_CHILD | WS_VISIBLE, 60, 60, 100, 100, top, (HMENU)3, hi, NULL);
    CHECK(a && b && c, "three overlapping children created");

    CHECK(GetWindow(top, GW_CHILD) == c, "topmost child is the last created (GW_CHILD)");
    CHECK(hit(top, 20, 20) == b, "ChildWindowFromPoint hits the topmost child at (20,20)");
    CHECK(hit(top, 70, 70) == c, "ChildWindowFromPoint hits c where c overlaps a");
    {
        POINT s = {20, 20};
        ClientToScreen(top, &s);
        CHECK(WindowFromPoint(s) == b, "WindowFromPoint hits b at the same point");
    }

    ShowWindow(b, SW_HIDE);
    CHECK(!IsWindowVisible(b), "hidden child is not visible");
    CHECK(hit(top, 20, 20) == a, "hidden child is skipped by ChildWindowFromPoint");
    {
        POINT s = {20, 20};
        ClientToScreen(top, &s);
        CHECK(WindowFromPoint(s) == a, "hidden child is skipped by WindowFromPoint");
    }
    ShowWindow(b, SW_SHOW);
    CHECK(IsWindowVisible(b), "child shown again");

    SetWindowPos(a, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    CHECK(GetWindow(top, GW_CHILD) == a, "SetWindowPos HWND_TOP raises a to the top of the z-order");
    CHECK(hit(top, 20, 20) == a, "raised a is hit at (20,20)");

    SetWindowPos(a, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    CHECK(GetWindow(top, GW_CHILD) != a, "SetWindowPos HWND_BOTTOM moves a below the others");
    CHECK(hit(top, 20, 20) == b, "lowered a is no longer hit where b overlaps");

    ShowWindow(top, SW_HIDE);
    CHECK(!IsWindowVisible(b) && !IsWindowVisible(a), "children of a hidden parent are not visible");
    ShowWindow(top, SW_SHOW);
    CHECK(IsWindowVisible(a), "children visible again when the parent is shown");

    /* SWP_HIDEWINDOW / SWP_SHOWWINDOW through SetWindowPos */
    SetWindowPos(c, NULL, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    CHECK(!IsWindowVisible(c), "SWP_HIDEWINDOW hides c");
    CHECK(hit(top, 70, 70) != c, "hidden c is not hit");
    SetWindowPos(c, NULL, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    CHECK(IsWindowVisible(c), "SWP_SHOWWINDOW shows c");

    /* destroying a child removes it from the z-order and from hit testing */
    n = b;
    DestroyWindow(n);
    CHECK(!IsWindow(n), "destroyed child is gone");
    CHECK(hit(top, 20, 20) == a, "after destroying b, a is hit at (20,20)");

    DestroyWindow(top);
    printf("core_zorder: %d failures\n", fails);
    (void)p;
    return fails ? 1 : 0;
}
