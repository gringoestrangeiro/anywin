/* anywin runtime - version.dll and wintrust.dll.
 * Version resources are not read: callers get ERROR_RESOURCE_DATA_NOT_FOUND and degrade.
 * Files are reported as unsigned (TRUST_E_NOSIGNATURE). */
#include "aw.h"

#define ERROR_RESOURCE_DATA_NOT_FOUND 1812
#define TRUST_E_NOSIGNATURE ((LONG)0x800B0100)

API(version, DWORD, GetFileVersionInfoSizeW, (const WCHAR *file, DWORD *handle))
{
    (void)file;
    if (handle)
        *handle = 0;
    aw_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
    return 0;
}

API(version, BOOL, GetFileVersionInfoW, (const WCHAR *file, DWORD handle, DWORD len, void *data))
{
    (void)file;
    (void)handle;
    (void)len;
    (void)data;
    aw_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
    return FALSE;
}

API(version, BOOL, VerQueryValueW, (const void *block, const WCHAR *sub, void **out, UINT *len))
{
    (void)block;
    (void)sub;
    if (out)
        *out = NULL;
    if (len)
        *len = 0;
    aw_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
    return FALSE;
}

API(wintrust, LONG, WinVerifyTrust, (void *hwnd, void *action, void *data))
{
    (void)hwnd;
    (void)action;
    (void)data;
    return TRUST_E_NOSIGNATURE;
}
