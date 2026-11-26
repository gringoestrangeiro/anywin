/* Module name normalization shared by the relinker and the runtime. */
#ifndef AW_MODMAP_H
#define AW_MODMAP_H
#include <string.h>
#include <ctype.h>

/* Returns a pointer to a static buffer holding the runtime namespace that
 * implements the given DLL. */
static inline const char *aw_normalize_module(const char *dll)
{
    static char buf[256];
    const char *b = dll;
    for (const char *p = dll; *p; p++)
        if (*p == '\\' || *p == '/')
            b = p + 1;
    size_t n = 0;
    for (; b[n] && n < sizeof buf - 1; n++)
        buf[n] = (char)tolower((unsigned char)b[n]);
    buf[n] = 0;
    if (n > 4 && (!strcmp(buf + n - 4, ".dll") || !strcmp(buf + n - 4, ".drv") || !strcmp(buf + n - 4, ".exe")))
        buf[n -= 4] = 0;
    static const char *crt[] = {"api-ms-win-crt-", "ucrtbase", "msvcr", "vcruntime", "msvcrt", NULL};
    for (int i = 0; crt[i]; i++)
        if (!strncmp(buf, crt[i], strlen(crt[i])))
            return "msvcrt";
    if (!strncmp(buf, "api-ms-win-core-registry", 24) || !strncmp(buf, "api-ms-win-security", 19) ||
        !strncmp(buf, "api-ms-win-eventing", 19) || !strcmp(buf, "sechost"))
        return "advapi32";
    if (!strncmp(buf, "api-ms-win-core-", 16) || !strcmp(buf, "kernelbase"))
        return "kernel32";
    if (!strncmp(buf, "api-ms-win-shell-", 17) || !strncmp(buf, "api-ms-win-shcore", 17))
        return "shell32";
    if (!strncmp(buf, "api-ms-win-ntuser", 17))
        return "user32";
    if (!strcmp(buf, "wsock32"))
        return "ws2_32";
    if (!strcmp(buf, "spoolss"))
        return "winspool";
    for (char *p = buf; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_')
            *p = '_';
    return buf;
}

typedef struct { const char *module; int ord; const char *name; } aw_ordinal_entry;

static const aw_ordinal_entry aw_ordinals[] = {
    {"comctl32", 17, "InitCommonControls"},
    {"comctl32", 413, "SetWindowSubclass"}, {"comctl32", 410, "SetWindowSubclass"},
    {"comctl32", 412, "RemoveWindowSubclass"}, {"comctl32", 415, "DefSubclassProc"},
    {"ws2_32", 1, "accept"}, {"ws2_32", 2, "bind"}, {"ws2_32", 3, "closesocket"},
    {"ws2_32", 4, "connect"}, {"ws2_32", 5, "getpeername"}, {"ws2_32", 6, "getsockname"},
    {"ws2_32", 7, "getsockopt"}, {"ws2_32", 8, "htonl"}, {"ws2_32", 9, "htons"},
    {"ws2_32", 10, "ioctlsocket"}, {"ws2_32", 11, "inet_addr"}, {"ws2_32", 12, "inet_ntoa"},
    {"ws2_32", 13, "listen"}, {"ws2_32", 14, "ntohl"}, {"ws2_32", 15, "ntohs"},
    {"ws2_32", 16, "recv"}, {"ws2_32", 17, "recvfrom"}, {"ws2_32", 18, "select"},
    {"ws2_32", 19, "send"}, {"ws2_32", 20, "sendto"}, {"ws2_32", 21, "setsockopt"},
    {"ws2_32", 22, "shutdown"}, {"ws2_32", 23, "socket"}, {"ws2_32", 51, "gethostbyaddr"},
    {"ws2_32", 52, "gethostbyname"}, {"ws2_32", 57, "gethostname"}, {"ws2_32", 111, "WSAGetLastError"},
    {"ws2_32", 112, "WSASetLastError"}, {"ws2_32", 115, "WSAStartup"}, {"ws2_32", 116, "WSACleanup"},
    {"ws2_32", 151, "__WSAFDIsSet"},
    {"oleaut32", 2, "SysAllocString"}, {"oleaut32", 4, "SysAllocStringLen"},
    {"oleaut32", 6, "SysFreeString"}, {"oleaut32", 7, "SysStringLen"},
    {"oleaut32", 8, "VariantInit"}, {"oleaut32", 9, "VariantClear"},
    {NULL, 0, NULL}};

static inline const char *aw_ordinal_name(const char *module, int ord)
{
    for (const aw_ordinal_entry *e = aw_ordinals; e->module; e++)
        if (e->ord == ord && !strcmp(e->module, module))
            return e->name;
    return NULL;
}
#endif
