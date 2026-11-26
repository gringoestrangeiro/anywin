/* text probe: draws a line with ExtTextOutW the way PuTTY does (ETO_OPAQUE|ETO_CLIPPED, rc = the
 * cell box, lpDx = cell advances) into a memory DC, then prints the bitmap as ASCII art and
 * checks that the first glyph is not clipped and that the background is painted. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define CELL_W 8
#define CELL_H 16
#define COLS 6

int main(void)
{
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bm = CreateCompatibleBitmap(screen, 200, 40);
    HBITMAP old = SelectObject(dc, bm);
    LOGFONTA lf;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -13;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = DEFAULT_QUALITY;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    strcpy(lf.lfFaceName, "Courier New");
    HFONT font = CreateFontIndirectA(&lf);
    HFONT oldf = SelectObject(dc, font);
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    printf("metrics: avg=%ld max=%ld height=%ld\n", tm.tmAveCharWidth, tm.tmMaxCharWidth, tm.tmHeight);

    RECT all = {0, 0, 200, 40};
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &all, black);
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));

    const WCHAR text[COLS + 1] = {'H', 'I', 'l', '|', 'W', 'm'};
    INT dx[COLS];
    for (int i = 0; i < COLS; i++)
        dx[i] = CELL_W;
    /* PuTTY draws the first row of its terminal at x=1 with the clip box starting at x=1 */
    RECT cell = {1, 10, 1 + COLS * CELL_W, 10 + CELL_H};
    ExtTextOutW(dc, 1, 10, ETO_OPAQUE | ETO_CLIPPED, &cell, text, COLS, dx);

    int ink_first = 0, ink_total = 0, minx = 1000;
    for (int y = 0; y < 40; y++) {
        char line[201];
        for (int x = 0; x < 200; x++) {
            COLORREF c = GetPixel(dc, x, y);
            int on = (c & 0xFF) > 96;
            line[x] = on ? '#' : (c ? '+' : '.');
            if (on) {
                ink_total++;
                if (x < minx) minx = x;
                if (x >= 1 && x < 1 + CELL_W && y >= 10 && y < 10 + CELL_H)
                    ink_first++;
            }
        }
        line[60] = 0;
        if (y >= 8 && y <= 28)
            printf("%s\n", line);
    }
    printf("first cell ink=%d total=%d minx=%d\n", ink_first, ink_total, minx);
    SelectObject(dc, oldf);
    SelectObject(dc, old);
    DeleteObject(font);
    DeleteObject(bm);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
    /* the first glyph must have ink inside its own cell and the glyph must start at its cell */
    return ink_first > 4 && minx >= 1 ? 0 : 1;
}
