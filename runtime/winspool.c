/* winspool: the printer API as seen by a machine with no printers installed. The runtime has no
 * print spooler, so the enumeration succeeds with zero printers, and opening or printing to a named
 * printer fails with ERROR_INVALID_PRINTER_NAME (1801), as Windows reports for an unknown printer.
 * Programs that build a printer list (PuTTY's Printing panel) therefore find the list empty instead
 * of calling a missing export. spoolss.dll maps to this module (modmap.h). */
#include "aw.h"
#include "k32.h"

#ifndef LPBYTE
typedef BYTE *LPBYTE;
#endif
#ifndef LPDWORD
typedef DWORD *LPDWORD;
#endif

#define AW_ERROR_INVALID_PRINTER_NAME 1801
#define AW_ERROR_NOT_SUPPORTED 50

API(winspool, BOOL, EnumPrintersA, (DWORD flags, LPSTR name, DWORD level, LPBYTE buf, DWORD cbbuf, LPDWORD needed, LPDWORD returned))
{
    (void)flags; (void)name; (void)level; (void)buf; (void)cbbuf;
    if (needed) *needed = 0;
    if (returned) *returned = 0;
    return TRUE;
}

API(winspool, BOOL, EnumPrintersW, (DWORD flags, LPWSTR name, DWORD level, LPBYTE buf, DWORD cbbuf, LPDWORD needed, LPDWORD returned))
{
    (void)flags; (void)name; (void)level; (void)buf; (void)cbbuf;
    if (needed) *needed = 0;
    if (returned) *returned = 0;
    return TRUE;
}

API(winspool, BOOL, OpenPrinterA, (LPSTR name, HANDLE *out, void *defaults))
{
    (void)name; (void)defaults;
    if (out) *out = NULL;
    aw_set_last_error(AW_ERROR_INVALID_PRINTER_NAME);
    return FALSE;
}

API(winspool, BOOL, OpenPrinterW, (LPWSTR name, HANDLE *out, void *defaults))
{
    (void)name; (void)defaults;
    if (out) *out = NULL;
    aw_set_last_error(AW_ERROR_INVALID_PRINTER_NAME);
    return FALSE;
}

API(winspool, BOOL, ClosePrinter, (HANDLE h))
{
    (void)h;
    return TRUE;
}

API(winspool, DWORD, StartDocPrinterA, (HANDLE h, DWORD level, const void *doc))
{
    (void)h; (void)level; (void)doc;
    aw_set_last_error(AW_ERROR_NOT_SUPPORTED);
    return 0;
}

API(winspool, DWORD, StartDocPrinterW, (HANDLE h, DWORD level, const void *doc))
{
    (void)h; (void)level; (void)doc;
    aw_set_last_error(AW_ERROR_NOT_SUPPORTED);
    return 0;
}

API(winspool, BOOL, EndDocPrinter, (HANDLE h))
{
    (void)h;
    aw_set_last_error(AW_ERROR_NOT_SUPPORTED);
    return FALSE;
}

API(winspool, BOOL, StartPagePrinter, (HANDLE h))
{
    (void)h;
    aw_set_last_error(AW_ERROR_NOT_SUPPORTED);
    return FALSE;
}

API(winspool, BOOL, EndPagePrinter, (HANDLE h))
{
    (void)h;
    aw_set_last_error(AW_ERROR_NOT_SUPPORTED);
    return FALSE;
}

API(winspool, BOOL, WritePrinter, (HANDLE h, LPCVOID buf, DWORD n, LPDWORD written))
{
    (void)h; (void)buf; (void)n;
    if (written) *written = 0;
    aw_set_last_error(AW_ERROR_NOT_SUPPORTED);
    return FALSE;
}

API(winspool, BOOL, GetDefaultPrinterA, (LPSTR buf, LPDWORD n))
{
    (void)buf;
    if (n) *n = 0;
    aw_set_last_error(2 /* ERROR_FILE_NOT_FOUND: no default printer */);
    return FALSE;
}

API(winspool, BOOL, GetDefaultPrinterW, (LPWSTR buf, LPDWORD n))
{
    (void)buf;
    if (n) *n = 0;
    aw_set_last_error(2);
    return FALSE;
}
