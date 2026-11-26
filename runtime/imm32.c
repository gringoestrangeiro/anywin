/* anywin IMM32 (S-IMM): no input method is installed, so the IME entry points report the
 * documented "no IME" results.  ImmGetContext yields NULL, which makes callers skip composition
 * handling and fall back to WM_CHAR text input. */
#include "user32_int.h"

#define AW_IMM_ERROR_NODATA (-1)

API(imm32, HANDLE, ImmGetContext, (HWND hwnd))
{
    (void)hwnd;
    return NULL;
}

API(imm32, BOOL, ImmReleaseContext, (HWND hwnd, HANDLE himc))
{
    (void)hwnd;
    (void)himc;
    return TRUE;
}

API(imm32, LONG, ImmGetCompositionStringW, (HANDLE himc, DWORD index, void *buf, DWORD len))
{
    (void)himc;
    (void)index;
    (void)buf;
    (void)len;
    return AW_IMM_ERROR_NODATA; /* no composition string */
}

API(imm32, BOOL, ImmSetCompositionFontA, (HANDLE himc, LOGFONTA *lf))
{
    (void)himc;
    (void)lf;
    return FALSE;
}

API(imm32, BOOL, ImmSetCompositionWindow, (HANDLE himc, void *cf))
{
    (void)himc;
    (void)cf;
    return FALSE;
}
