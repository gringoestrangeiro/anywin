/* anywin runtime - COM and OLE Automation basics (ole32, combase, oleaut32, comctl32).
 * No COM server is available, so object creation fails with REGDB_E_CLASSNOTREG and
 * initialization succeeds. BSTRs use the standard layout: a 32-bit byte count followed by the
 * UTF-16 characters and a terminator, so SysStringLen reads the length back. */
#include "aw.h"
#include <stdlib.h>

#define S_OK 0
#define S_FALSE 1
#define REGDB_E_CLASSNOTREG ((HRESULT)0x80040154)
#define E_NOINTERFACE ((HRESULT)0x80004002)

API(ole32, HRESULT, CoInitializeEx, (void *reserved, DWORD coinit))
{
    (void)reserved;
    (void)coinit;
    return S_OK;
}

API(ole32, void, CoUninitialize, (void)) { }

API(ole32, HRESULT, CoCreateInstance, (const void *clsid, void *outer, DWORD ctx, const void *iid, void **out))
{
    (void)clsid;
    (void)outer;
    (void)ctx;
    (void)iid;
    if (out)
        *out = NULL;
    return REGDB_E_CLASSNOTREG;
}

API(combase, void, CoTaskMemFree, (void *p)) { free(p); }
API(ole32, void, CoTaskMemFree, (void *p)) { free(p); }

API(ole32, void *, CoTaskMemAlloc, (SIZE_T n)) { return malloc(n ? n : 1); }

API(oleaut32, WCHAR *, SysAllocStringLen, (const WCHAR *s, UINT n))
{
    uint32_t *p = malloc(sizeof(uint32_t) + ((size_t)n + 1) * sizeof(WCHAR));
    if (!p)
        return NULL;
    *p = n * sizeof(WCHAR);
    WCHAR *chars = (WCHAR *)(p + 1);
    if (s && n)
        memcpy(chars, s, n * sizeof(WCHAR));
    else
        memset(chars, 0, n * sizeof(WCHAR));
    chars[n] = 0;
    return chars;
}

API(oleaut32, WCHAR *, SysAllocString, (const WCHAR *s))
{
    return __aw_oleaut32_SysAllocStringLen(s, s ? (UINT)aw_wcslen(s) : 0);
}

API(oleaut32, void, SysFreeString, (WCHAR *bstr))
{
    if (bstr)
        free((uint32_t *)bstr - 1);
}

API(oleaut32, UINT, SysStringLen, (WCHAR *bstr))
{
    return bstr ? *((uint32_t *)bstr - 1) / sizeof(WCHAR) : 0;
}

/* IErrorInfo is never set by this runtime, so there is no error object to return. */
API(oleaut32, HRESULT, GetErrorInfo, (DWORD reserved, void **info))
{
    (void)reserved;
    if (info)
        *info = NULL;
    return S_FALSE;
}

API(comctl32, BOOL, InitCommonControlsEx, (const void *icc))
{
    (void)icc;
    return TRUE;
}

/* ole32 and oleaut32 names kept for callers that import them by their home DLL. */
API(ole32, HRESULT, CoInitialize, (void *reserved))
{
    (void)reserved;
    return S_OK;
}
