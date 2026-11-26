/* advapi32: security identifiers and security descriptors (self-contained, in-memory structures
 * with the x64 layouts), the current user name, and RegEnumKeyA over RegEnumKeyExA.
 * There is no access-check model: descriptors record owner and DACL for the program to read back,
 * which is what programs that create their own pipes or mutexes rely on. */
#include "aw.h"
#include "k32.h"
#include <errno.h>
#include <pwd.h>
#include <unistd.h>

#define ERR_INVALID_PARAMETER 87
#define ERR_INSUFFICIENT_BUFFER 122
#define SID_REVISION 1
#define SID_MAX_SUB_AUTHORITIES 15
#define SECURITY_DESCRIPTOR_REVISION 1
#define SE_OWNER_DEFAULTED 0x0002
#define SE_DACL_PRESENT 0x0004
#define SE_DACL_DEFAULTED 0x0008

LONG WINAPI __aw_advapi32_RegEnumKeyExA(HKEY hkey, DWORD index, LPSTR name, DWORD *cch_name, DWORD *reserved,
                                        LPSTR cls, DWORD *cch_cls, FILETIME *ft);

typedef struct {
    BYTE Revision, SubAuthorityCount;
    BYTE IdentifierAuthority[6];
    DWORD SubAuthority[]; /* SubAuthorityCount entries */
} SID_L;

/* SECURITY_DESCRIPTOR, x64: 40 bytes */
typedef struct {
    BYTE Revision, Sbz1;
    WORD Control;
    void *Owner, *Group, *Sacl, *Dacl;
} SD_L;

static DWORD fail(DWORD e)
{
    aw_set_last_error(e);
    return 0;
}

static DWORD sid_length(const SID_L *s)
{
    return s ? 8u + 4u * s->SubAuthorityCount : 0;
}

API(advapi32, BOOL, AllocateAndInitializeSid, (const BYTE *auth, BYTE count, DWORD s0, DWORD s1, DWORD s2, DWORD s3,
                                              DWORD s4, DWORD s5, DWORD s6, DWORD s7, void **out))
{
    if (!auth || !out || count > 8) {
        fail(ERR_INVALID_PARAMETER);
        return FALSE;
    }
    SID_L *s = calloc(1, 8 + 4 * (size_t)(count ? count : 1));
    if (!s) {
        fail(8);
        return FALSE;
    }
    DWORD subs[8] = {s0, s1, s2, s3, s4, s5, s6, s7};
    s->Revision = SID_REVISION;
    s->SubAuthorityCount = count;
    memcpy(s->IdentifierAuthority, auth, 6);
    for (int i = 0; i < count; i++)
        s->SubAuthority[i] = subs[i];
    *out = s;
    return TRUE;
}

API(advapi32, void *, FreeSid, (void *sid))
{
    free(sid);
    return NULL;
}

API(advapi32, DWORD, GetLengthSid, (const void *sid)) { return sid_length(sid); }

API(advapi32, BOOL, CopySid, (DWORD len, void *dst, const void *src))
{
    DWORD n = sid_length(src);
    if (!dst || !n)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    if (len < n)
        return fail(ERR_INSUFFICIENT_BUFFER), FALSE;
    memcpy(dst, src, n);
    return TRUE;
}

API(advapi32, BOOL, EqualSid, (const void *a, const void *b))
{
    DWORD n = sid_length(a);
    if (!n || n != sid_length(b))
        return FALSE;
    return memcmp(a, b, n) == 0;
}

API(advapi32, BOOL, InitializeSecurityDescriptor, (void *sd, DWORD rev))
{
    if (!sd || rev != SECURITY_DESCRIPTOR_REVISION)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    memset(sd, 0, sizeof(SD_L));
    ((SD_L *)sd)->Revision = SECURITY_DESCRIPTOR_REVISION;
    return TRUE;
}

API(advapi32, BOOL, SetSecurityDescriptorDacl, (void *sd, BOOL present, void *dacl, BOOL defaulted))
{
    SD_L *d = sd;
    if (!d || d->Revision != SECURITY_DESCRIPTOR_REVISION)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    d->Dacl = present ? dacl : NULL;
    d->Control = (WORD)((d->Control & ~(SE_DACL_PRESENT | SE_DACL_DEFAULTED)) |
                        (present ? SE_DACL_PRESENT : 0) | (defaulted ? SE_DACL_DEFAULTED : 0));
    return TRUE;
}

API(advapi32, BOOL, SetSecurityDescriptorOwner, (void *sd, void *owner, BOOL defaulted))
{
    SD_L *d = sd;
    if (!d || d->Revision != SECURITY_DESCRIPTOR_REVISION)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    d->Owner = owner;
    d->Control = (WORD)((d->Control & ~SE_OWNER_DEFAULTED) | (defaulted ? SE_OWNER_DEFAULTED : 0));
    return TRUE;
}

API(advapi32, BOOL, GetUserNameA, (char *buf, DWORD *size))
{
    if (!buf || !size)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    struct passwd *pw = getpwuid(getuid());
    const char *name = pw ? pw->pw_name : getenv("USER");
    if (!name)
        name = getenv("LOGNAME");
    if (!name)
        name = "";
    DWORD need = (DWORD)strlen(name) + 1;
    if (*size < need) {
        *size = need;
        return fail(ERR_INSUFFICIENT_BUFFER), FALSE;
    }
    memcpy(buf, name, need);
    *size = need;
    return TRUE;
}

API(advapi32, LONG, RegEnumKeyA, (HKEY hkey, DWORD index, LPSTR name, DWORD cch))
{
    DWORD n = cch;
    return __aw_advapi32_RegEnumKeyExA(hkey, index, name, &n, NULL, NULL, NULL, NULL);
}
