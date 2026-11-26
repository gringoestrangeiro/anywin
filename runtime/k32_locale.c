/* kernel32: code pages, locale data, string comparison and case mapping, date and time formatting.
 *
 * The runtime presents one locale: en-US (LCID 0x0409) with the Windows 1252 ANSI and 437 OEM code
 * pages. Comparison and case mapping are ordinal with simple case tables (ASCII, Latin-1, Greek,
 * Cyrillic); they do not implement full linguistic collation. */
#include "aw.h"
#include "k32.h"
#include <errno.h>
#include <time.h>

#define ERR_INVALID_PARAMETER 87
#define ERR_INSUFFICIENT_BUFFER 122
#define ERR_NOT_SUPPORTED 50

#define LCID_EN_US 0x0409

int WINAPI __aw_kernel32_GetLocaleInfoW(DWORD lcid, DWORD type, WCHAR *buf, int cch);
#define LOCALE_RETURN_NUMBER 0x20000000u
#define NORM_IGNORECASE 0x00000001u
#define LCMAP_LOWERCASE 0x00000100u
#define LCMAP_UPPERCASE 0x00000200u
#define LCMAP_SORTKEY 0x00000400u
#define CT_CTYPE1 0x00000001u
#define DATE_SHORTDATE 0x00000001u
#define DATE_LONGDATE 0x00000002u
#define TIME_NOMINUTESORSECONDS 0x00000001u
#define TIME_NOSECONDS 0x00000002u
#define TIME_NOTIMEMARKER 0x00000004u
#define TIME_FORCE24HOURFORMAT 0x00000008u

#define C1_UPPER 0x0001
#define C1_LOWER 0x0002
#define C1_DIGIT 0x0004
#define C1_SPACE 0x0008
#define C1_PUNCT 0x0010
#define C1_CNTRL 0x0020
#define C1_BLANK 0x0040
#define C1_XDIGIT 0x0080
#define C1_ALPHA 0x0100

static DWORD fail(DWORD e)
{
    aw_set_last_error(e);
    return 0;
}

/* ---- code pages ----------------------------------------------------------------- */
API(kernel32, UINT, GetACP, (void)) { return 1252; }
API(kernel32, UINT, GetOEMCP, (void)) { return 437; }

static int code_page_known(UINT cp)
{
    static const UINT list[] = {437,   720,   737,   775,   850,   852,   855,   857,   858,   862,  866,
                                874,   932,   936,   949,   950,   1200,  1201,  1250,  1251,  1252, 1253,
                                1254,  1255,  1256,  1257,  1258,  20127, 28591, 65000, 65001};
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++)
        if (list[i] == cp)
            return 1;
    return 0;
}

API(kernel32, BOOL, IsValidCodePage, (UINT cp)) { return code_page_known(cp); }

typedef struct {
    UINT MaxCharSize;
    BYTE DefaultChar[2];
    BYTE LeadByte[12];
} CPINFO_L;

API(kernel32, BOOL, GetCPInfo, (UINT cp, CPINFO_L *info))
{
    if (!code_page_known(cp) || !info)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    memset(info, 0, sizeof *info);
    info->MaxCharSize = cp == 65001 ? 4 : (cp == 932 || cp == 936 || cp == 949 || cp == 950) ? 2 : 1;
    info->DefaultChar[0] = '?';
    return TRUE;
}

/* ---- locale data ------------------------------------------------------------------ */
static int locale_known(DWORD lcid)
{
    if (lcid == 0 || lcid == 0x0400 || lcid == 0x0800) /* neutral, user default, system default */
        return 1;
    return lcid == LCID_EN_US || lcid == 0x0009;
}

API(kernel32, WORD, GetUserDefaultLangID, (void)) { return LCID_EN_US; }
API(kernel32, DWORD, GetUserDefaultLCID, (void)) { return LCID_EN_US; }
API(kernel32, DWORD, GetSystemDefaultLCID, (void)) { return LCID_EN_US; }

API(kernel32, BOOL, IsValidLocale, (DWORD lcid, DWORD flags))
{
    (void)flags;
    return locale_known(lcid);
}

/* LOCALE_* type -> value; numeric values are flagged so LOCALE_RETURN_NUMBER can return them */
static const struct {
    DWORD type;
    const char *value;
    int numeric;
} locale_table[] = {
    {0x0001, "0409", 1},                 /* LOCALE_ILANGUAGE (hex LANGID) */
    {0x0002, "English (United States)", 0}, /* LOCALE_SLANGUAGE */
    {0x0006, "United States", 0},        /* LOCALE_SCOUNTRY */
    {0x000B, "437", 0},                  /* LOCALE_IDEFAULTCODEPAGE */
    {0x000C, ",", 0},                    /* LOCALE_SLIST */
    {0x000D, "0", 1},                    /* LOCALE_IMEASURE */
    {0x000E, ".", 0},                    /* LOCALE_SDECIMAL */
    {0x000F, ",", 0},                    /* LOCALE_STHOUSAND */
    {0x0010, "3;0", 0},                  /* LOCALE_SGROUPING */
    {0x0011, "2", 1},                    /* LOCALE_IDIGITS */
    {0x0012, "1", 1},                    /* LOCALE_ILZERO */
    {0x0014, "$", 0},                    /* LOCALE_SCURRENCY */
    {0x0015, "USD", 0},                  /* LOCALE_SINTLSYMBOL */
    {0x001E, ":", 0},                    /* LOCALE_STIME */
    {0x0021, "0", 1},                    /* LOCALE_IDATE */
    {0x0023, "0", 1},                    /* LOCALE_ITIME */
    {0x0028, "AM", 0},                   /* LOCALE_S1159 */
    {0x0029, "PM", 0},                   /* LOCALE_S2359 */
    {0x0059, "en", 0},                   /* LOCALE_SISO639LANGNAME */
    {0x005A, "US", 0},                   /* LOCALE_SISO3166CTRYNAME */
    {0x100C, "6", 1},                    /* LOCALE_IFIRSTDAYOFWEEK */
    {0x1001, "English", 0},              /* LOCALE_SENGLANGUAGE */
    {0x1002, "United States", 0},        /* LOCALE_SENGCOUNTRY */
    {0x1003, "h:mm:ss tt", 0},           /* LOCALE_STIMEFORMAT */
    {0x1004, "1252", 0},                 /* LOCALE_IDEFAULTANSICODEPAGE */
    {0x1010, "1", 1},                    /* LOCALE_INEGNUMBER */
    {0x1D,   "/", 0},                    /* LOCALE_SDATE */
    {0x1F,   "M/d/yyyy", 0},             /* LOCALE_SSHORTDATE */
    {0x20,   "dddd, MMMM dd, yyyy", 0},  /* LOCALE_SLONGDATE */
};

API(kernel32, int, GetLocaleInfoW, (DWORD lcid, DWORD type, WCHAR *buf, int cch))
{
    if (!locale_known(lcid))
        return fail(ERR_INVALID_PARAMETER), 0;
    int numeric_req = (type & LOCALE_RETURN_NUMBER) != 0;
    type &= ~LOCALE_RETURN_NUMBER;
    for (size_t i = 0; i < sizeof locale_table / sizeof locale_table[0]; i++) {
        if (locale_table[i].type != type)
            continue;
        if (numeric_req) {
            if (!locale_table[i].numeric)
                return fail(ERR_INVALID_PARAMETER), 0;
            UINT v = (UINT)strtoul(locale_table[i].value, NULL, type == 0x0001 ? 16 : 10); /* ILANGUAGE is hex */
            if (cch == 0)
                return (int)(sizeof(UINT) / sizeof(WCHAR));
            if (cch < (int)(sizeof(UINT) / sizeof(WCHAR)) || !buf)
                return fail(ERR_INSUFFICIENT_BUFFER), 0;
            memcpy(buf, &v, sizeof v);
            return (int)(sizeof(UINT) / sizeof(WCHAR));
        }
        WCHAR *w = aw_u2w(locale_table[i].value, -1);
        int n = (int)aw_wcslen(w) + 1;
        if (cch == 0) {
            free(w);
            return n;
        }
        if (cch < n) {
            free(w);
            return fail(ERR_INSUFFICIENT_BUFFER), 0;
        }
        memcpy(buf, w, (size_t)n * sizeof(WCHAR));
        free(w);
        return n;
    }
    return fail(ERR_INVALID_PARAMETER), 0;
}

API(kernel32, int, GetLocaleInfoA, (DWORD lcid, DWORD type, char *buf, int cch))
{
    /* numeric results are binary UINT values, not text */
    if (type & LOCALE_RETURN_NUMBER)
        return __aw_kernel32_GetLocaleInfoW(lcid, type, (WCHAR *)buf, cch);
    if (cch == 0)
        return __aw_kernel32_GetLocaleInfoW(lcid, type, NULL, 0);
    WCHAR *w = malloc((size_t)cch * sizeof(WCHAR));
    if (!w)
        return fail(8), 0;
    int n = __aw_kernel32_GetLocaleInfoW(lcid, type, w, cch);
    if (n > 0) {
        char *u = aw_w2u(w, -1);
        int len = u ? (int)strlen(u) + 1 : 0;
        if (len > cch) {
            free(u);
            free(w);
            return fail(ERR_INSUFFICIENT_BUFFER), 0;
        }
        memcpy(buf, u, (size_t)len);
        free(u);
        n = len;
    }
    free(w);
    return n;
}

/* EnumSystemLocalesW: the single installed locale is reported once */
API(kernel32, BOOL, EnumSystemLocalesW, (BOOL(WINAPI * proc)(WCHAR *), DWORD flags))
{
    (void)flags;
    if (!proc)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    WCHAR name[] = {'0', '0', '0', '0', '0', '4', '0', '9', 0};
    proc(name);
    return TRUE;
}

/* ---- string comparison and case mapping ------------------------------------------ */
static WCHAR fold_upper(WCHAR c)
{
    if (c < 0x80)
        return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return c - 32;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2)
        return c - 32;
    if (c >= 0x430 && c <= 0x44F)
        return c - 32;
    if (c >= 0x450 && c <= 0x45F)
        return c - 80;
    return c;
}

static WCHAR fold_lower(WCHAR c)
{
    if (c < 0x80)
        return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return c + 32;
    if (c >= 0x391 && c <= 0x3A9)
        return c + 32;
    if (c >= 0x410 && c <= 0x42F)
        return c + 32;
    if (c >= 0x400 && c <= 0x40F)
        return c + 80;
    return c;
}

static size_t wlen_or(const WCHAR *s, int n)
{
    return n < 0 ? aw_wcslen(s) : (size_t)n;
}

/* CompareStringW: 1 less, 2 equal, 3 greater (CSTR_*); 0 on error */
API(kernel32, int, CompareStringW, (DWORD lcid, DWORD flags, const WCHAR *s1, int n1, const WCHAR *s2, int n2))
{
    if (!locale_known(lcid) || !s1 || !s2)
        return fail(ERR_INVALID_PARAMETER), 0;
    size_t l1 = wlen_or(s1, n1), l2 = wlen_or(s2, n2);
    int ic = (flags & NORM_IGNORECASE) != 0;
    for (size_t i = 0; i < l1 && i < l2; i++) {
        WCHAR a = s1[i], b = s2[i];
        if (ic) {
            a = fold_upper(a);
            b = fold_upper(b);
        }
        if (a != b)
            return a < b ? 1 : 3;
    }
    if (l1 == l2)
        return 2;
    return l1 < l2 ? 1 : 3;
}

/* LCMapStringW: lower or upper case mapping; sort keys are not produced */
API(kernel32, int, LCMapStringW, (DWORD lcid, DWORD flags, const WCHAR *src, int srclen, WCHAR *dst, int dstlen))
{
    if (!locale_known(lcid) || !src || (flags & LCMAP_SORTKEY) || !(flags & (LCMAP_LOWERCASE | LCMAP_UPPERCASE)))
        return fail(flags & LCMAP_SORTKEY ? ERR_NOT_SUPPORTED : ERR_INVALID_PARAMETER), 0;
    size_t n = wlen_or(src, srclen);
    int count = (int)(srclen < 0 ? n + 1 : n); /* -1 counts the terminating NUL */
    if (dstlen == 0)
        return count;
    if (!dst || dstlen < count)
        return fail(ERR_INSUFFICIENT_BUFFER), 0;
    for (int i = 0; i < count; i++) {
        WCHAR c = src[i];
        if (i < (int)n || srclen < 0)
            c = (flags & LCMAP_LOWERCASE) ? fold_lower(c) : fold_upper(c);
        dst[i] = c;
    }
    return count;
}

static int is_upper(WCHAR c) { return fold_lower(c) != c; }
static int is_lower(WCHAR c) { return fold_upper(c) != c; }
static int is_digit(WCHAR c) { return c >= '0' && c <= '9'; }
static int is_alpha(WCHAR c)
{
    if (c < 0x80)
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (c < 0xAA || c == 0xD7 || c == 0xF7)
        return 0;
    return c < 0x2000 || (c >= 0x3040 && c < 0xA000) || (c >= 0xAC00 && c < 0xD7A4);
}
static int is_space(WCHAR c) { return c == ' ' || (c >= 9 && c <= 13) || c == 0xA0; }

/* GetStringTypeW with CT_CTYPE1: character classes per character */
API(kernel32, BOOL, GetStringTypeW, (DWORD type, const WCHAR *src, int cch, WORD *out))
{
    if (!src || !out)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    if (type != CT_CTYPE1)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    size_t n = wlen_or(src, cch);
    for (size_t i = 0; i < n; i++) {
        WCHAR c = src[i];
        WORD f = 0;
        if (is_upper(c))
            f |= C1_UPPER;
        if (is_lower(c))
            f |= C1_LOWER;
        if (is_digit(c))
            f |= C1_DIGIT;
        if (is_space(c))
            f |= C1_SPACE;
        if (c == ' ' || c == '\t')
            f |= C1_BLANK;
        if (c < 0x20 || c == 0x7F)
            f |= C1_CNTRL;
        if (c < 0x80 && c > 0x20 && c != 0x7F && !is_alpha(c) && !is_digit(c))
            f |= C1_PUNCT;
        if (is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
            f |= C1_XDIGIT;
        if (is_alpha(c))
            f |= C1_ALPHA;
        out[i] = f;
    }
    return TRUE;
}

/* ---- date and time formatting ------------------------------------------------------- */
static const char *const month_names[] = {"January", "February", "March",     "April",   "May",      "June",
                                          "July",    "August",   "September", "October", "November", "December"};
static const char *const day_names[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

/* Expands a format picture into `out` (UTF-8). Tokens follow the en-US .NET-style picture:
 * d, M, y for dates; h, H, m, s, t for times; single quotes quote literal text. */
static size_t expand(const char *fmt, const SYSTEMTIME *st, DWORD flags, int time_part, char *out, size_t cap)
{
    size_t o = 0;
#define PUT(...)                                                                                    \
    do {                                                                                            \
        int k_ = snprintf(out + o, o < cap ? cap - o : 0, __VA_ARGS__);                             \
        if (k_ > 0)                                                                                 \
            o += (size_t)k_;                                                                        \
    } while (0)
    for (const char *p = fmt; *p;) {
        if (*p == '\'') {
            for (p++; *p && *p != '\''; p++)
                PUT("%c", *p);
            if (*p)
                p++;
            continue;
        }
        char c = *p;
        int run = 0;
        while (p[run] == c)
            run++;
        if (c == 'd' || c == 'M' || c == 'y' || c == 'h' || c == 'H' || c == 'm' || c == 's' || c == 't') {
            switch (c) {
            case 'd':
                if (run <= 2)
                    PUT(run == 2 ? "%02u" : "%u", st->wDay);
                else
                    PUT("%.*s", run >= 4 ? 99 : 3, day_names[st->wDayOfWeek % 7]);
                break;
            case 'M':
                if (run <= 2)
                    PUT(run == 2 ? "%02u" : "%u", st->wMonth);
                else
                    PUT("%.*s", run >= 4 ? 99 : 3, month_names[(st->wMonth + 11) % 12]);
                break;
            case 'y':
                if (run <= 2)
                    PUT("%02u", st->wYear % 100);
                else
                    PUT("%04u", st->wYear);
                break;
            case 'h': {
                unsigned h = st->wHour % 12;
                PUT(run >= 2 ? "%02u" : "%u", h ? h : 12);
                break;
            }
            case 'H':
                PUT(run >= 2 ? "%02u" : "%u", st->wHour);
                break;
            case 'm':
                PUT(run >= 2 ? "%02u" : "%u", st->wMinute);
                break;
            case 's':
                PUT(run >= 2 ? "%02u" : "%u", st->wSecond);
                break;
            case 't':
                if (!(flags & TIME_NOTIMEMARKER))
                    PUT("%s", st->wHour < 12 ? (run >= 2 ? "AM" : "A") : (run >= 2 ? "PM" : "P"));
                break;
            }
            p += run;
            continue;
        }
        PUT("%c", c);
        p++;
    }
#undef PUT
    (void)time_part;
    return o;
}

static void now_local(SYSTEMTIME *st)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    memset(st, 0, sizeof *st);
    st->wYear = (WORD)(lt.tm_year + 1900);
    st->wMonth = (WORD)(lt.tm_mon + 1);
    st->wDayOfWeek = (WORD)lt.tm_wday;
    st->wDay = (WORD)lt.tm_mday;
    st->wHour = (WORD)lt.tm_hour;
    st->wMinute = (WORD)lt.tm_min;
    st->wSecond = (WORD)lt.tm_sec;
}

/* shared tail of GetDateFormatW and GetTimeFormatW: expands, then copies to the wide buffer */
static int emit(char *u, WCHAR *out, int cch)
{
    WCHAR *w = aw_u2w(u, -1);
    int n = (int)aw_wcslen(w) + 1;
    free(u);
    if (cch == 0) {
        free(w);
        return n;
    }
    if (cch < n) {
        free(w);
        return fail(ERR_INSUFFICIENT_BUFFER), 0;
    }
    memcpy(out, w, (size_t)n * sizeof(WCHAR));
    free(w);
    return n;
}

API(kernel32, int, GetDateFormatW, (DWORD lcid, DWORD flags, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *out,
                                    int cch))
{
    if (!locale_known(lcid))
        return fail(ERR_INVALID_PARAMETER), 0;
    SYSTEMTIME now;
    if (!st) {
        now_local(&now);
        st = &now;
    }
    char *f = fmt ? aw_w2u(fmt, -1) : NULL;
    const char *pic = f ? f : (flags & DATE_LONGDATE) ? "dddd, MMMM d, yyyy" : "M/d/yyyy";
    char buf[1024];
    size_t n = expand(pic, st, flags, 0, buf, sizeof buf);
    free(f);
    if (n >= sizeof buf)
        return fail(ERR_INSUFFICIENT_BUFFER), 0;
    return emit(strdup(buf), out, cch);
}

API(kernel32, int, GetTimeFormatW, (DWORD lcid, DWORD flags, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *out,
                                    int cch))
{
    if (!locale_known(lcid))
        return fail(ERR_INVALID_PARAMETER), 0;
    SYSTEMTIME now;
    if (!st) {
        now_local(&now);
        st = &now;
    }
    char *f = fmt ? aw_w2u(fmt, -1) : NULL;
    const char *pic;
    if (f)
        pic = f;
    else if (flags & TIME_FORCE24HOURFORMAT)
        pic = (flags & TIME_NOSECONDS) ? "HH:mm" : "HH:mm:ss";
    else if (flags & TIME_NOSECONDS)
        pic = "h:mm tt";
    else
        pic = "h:mm:ss tt";
    char buf[1024];
    size_t n = expand(pic, st, flags, 1, buf, sizeof buf);
    free(f);
    if (n >= sizeof buf)
        return fail(ERR_INSUFFICIENT_BUFFER), 0;
    return emit(strdup(buf), out, cch);
}

/* ---- time zone ------------------------------------------------------------------------ */
typedef struct {
    LONG Bias;
    WCHAR StandardName[32];
    SYSTEMTIME StandardDate;
    LONG StandardBias;
    WCHAR DaylightName[32];
    SYSTEMTIME DaylightDate;
    LONG DaylightBias;
} TIME_ZONE_INFORMATION_L;

/* Reports the zone in force now as the standard zone: no transition rules are kept */
API(kernel32, DWORD, GetTimeZoneInformation, (TIME_ZONE_INFORMATION_L * tzi))
{
    if (!tzi)
        return fail(ERR_INVALID_PARAMETER), 0xFFFFFFFFu;
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    long std_off = lt.tm_gmtoff - (lt.tm_isdst > 0 ? 3600 : 0);
    memset(tzi, 0, sizeof *tzi);
    tzi->Bias = (LONG)(-std_off / 60);
    tzi->DaylightBias = -60;
    const char *name = tzname[0] ? tzname[0] : "UTC";
    WCHAR *w = aw_u2w(name, -1);
    if (w) {
        size_t k = 0;
        for (; w[k] && k < 31; k++)
            tzi->StandardName[k] = w[k];
        free(w);
    }
    return 0; /* TIME_ZONE_ID_UNKNOWN */
}
